#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bounded, read-only capability probe; role checks never create a display."""
import argparse
import json
import os
import platform
import shutil
import subprocess


def run(arguments, timeout=4, environment=None):
    result = subprocess.run(arguments, capture_output=True, timeout=timeout, check=False, env=environment)
    if len(result.stdout) > 65536 or len(result.stderr) > 65536:
        raise RuntimeError('oversized probe result')
    return result.returncode, result.stdout.decode('utf-8', 'replace')


def initial_report():
    return dict(send='unknown', receive='unknown', permission='unknown',
                sessionAvailable=False, sessionChecked=False,
                sessionReason='CapabilityCheckRequired',
                sendReason='CapabilityCheckRequired', receiveReason='CapabilityCheckRequired')


def probe(args):
    report = initial_report()
    system = platform.system()
    if system not in ('Linux', 'Darwin'):
        report.update(send='unsupported', receive='unsupported',
                      sendReason='UnsupportedPlatform', receiveReason='UnsupportedPlatform')
        return report
    environment = os.environ.copy()
    if system == 'Linux' and environment.get('DISPLAY') and not environment.get('QT_QPA_PLATFORM'):
        environment['QT_QPA_PLATFORM'] = 'xcb'
    ffmpeg = shutil.which('ffmpeg')
    # A missing decoder/window affects only receiving, never sending.
    try:
        if not ffmpeg or not os.access(args.viewer, os.X_OK):
            report.update(receive='unsupported', receiveReason='UnsupportedPlatform')
        else:
            code, decoders = run([ffmpeg, '-hide_banner', '-decoders'])
            if code == 0:
                code, output = run([args.viewer, '--capabilities'], environment=environment)
                if code == 0:
                    window = json.loads(output)
                    if not isinstance(window, dict):
                        raise ValueError("invalid viewer capability object")
                    supported = ' h264 ' in decoders and window.get('receive') is True
                    report.update(receive='supported' if supported else 'unsupported',
                                  receiveReason='None' if supported else 'UnsupportedPlatform',
                                  fullscreen=window.get('fullscreen') is True,
                                  minimize=window.get('minimize') is True)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError):
        pass  # A transient process/probe failure is unknown, not unsupported.
    if system == 'Darwin':
        try:
            if not args.macos_probe or not os.access(args.macos_probe, os.X_OK):
                return report
            command = [args.macos_probe]
            if args.request_permission:
                command += ['--request-capture-permission']
            code, output = run(command, 45 if args.request_permission else 4)
            data = json.loads(output) if code == 0 else {}
            if not isinstance(data, dict):
                raise ValueError('invalid native capability object')
            checked = (isinstance(data.get('sessionOnConsole'), bool) and
                       data.get('screenLockKnown') is True and isinstance(data.get('screenLocked'), bool))
            available = checked and data['sessionOnConsole'] and not data['screenLocked']
            report.update(sessionChecked=checked, sessionAvailable=available,
                          sessionReason=('None' if available else 'SessionUnavailable') if checked else 'CapabilityCheckRequired')
            if isinstance(data.get('privateApiAvailable'), bool):
                supported = data['privateApiAvailable']
                report.update(send='supported' if supported else 'unsupported',
                              sendReason='None' if supported else 'UnsupportedPlatform')
            if isinstance(data.get('capturePermission'), bool):
                report['permission'] = 'granted' if data['capturePermission'] else 'required'
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError):
            pass
        return report
    # Desktop availability is common to both roles. Capture dependencies below
    # are sender-only; screen recording/accessibility grants are not receiver gates.
    if not (environment.get('WAYLAND_DISPLAY') or environment.get('DISPLAY')):
        report.update(sessionChecked=True, sessionReason='SessionUnavailable')
        return report
    try:
        import dbus
        bus = dbus.SessionBus()
        saver = dbus.Interface(bus.get_object('org.gnome.ScreenSaver', '/org/gnome/ScreenSaver'), 'org.gnome.ScreenSaver')
        locked = bool(saver.GetActive(timeout=3))
        report.update(sessionChecked=True, sessionAvailable=not locked,
                      sessionReason='SessionUnavailable' if locked else 'None')
    except Exception:
        return report
    try:
        import gi
        gi.require_version('Gst', '1.0')
        from gi.repository import Gst
        Gst.init(None)
        if not ffmpeg:
            report.update(send='unsupported', sendReason='UnsupportedPlatform')
            return report
        code, encoders = run([ffmpeg, '-hide_banner', '-encoders'])
        if code != 0:
            return report
        if not Gst.ElementFactory.find('pipewiresrc') or 'libx264' not in encoders:
            report.update(send='unsupported', sendReason='UnsupportedPlatform')
            return report
        dbus.Interface(bus.get_object('org.gnome.Mutter.ScreenCast', '/org/gnome/Mutter/ScreenCast'),
                       'org.freedesktop.DBus.Introspectable').Introspect(timeout=3)
        dbus.Interface(bus.get_object('org.gnome.Mutter.DisplayConfig', '/org/gnome/Mutter/DisplayConfig'),
                       'org.gnome.Mutter.DisplayConfig').GetCurrentState(timeout=3)
        report.update(send='supported', sendReason='None', permission='granted')
    except Exception:
        pass
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--viewer', required=True)
    parser.add_argument('--macos-probe', default='')
    parser.add_argument('--request-permission', action='store_true')
    try:
        result = probe(parser.parse_args())
    except Exception:
        result = initial_report()
    print(json.dumps(result, separators=(',', ':')))
