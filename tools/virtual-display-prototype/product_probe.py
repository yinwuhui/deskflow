#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bounded, read-only product capability probe. Never creates a display."""
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


def probe(args):
    report = dict(send='unsupported', receive='unsupported', permission='unknown', sessionAvailable=False, sessionChecked=False)
    system = platform.system()
    if system not in ('Linux', 'Darwin'):
        return report
    if not shutil.which('ffmpeg') or not os.access(args.viewer, os.X_OK):
        return report
    _, decoders = run(['ffmpeg', '-hide_banner', '-decoders'])
    if ' h264 ' not in decoders:
        return report
    environment = os.environ.copy()
    if system == 'Linux' and environment.get('DISPLAY') and not environment.get('QT_QPA_PLATFORM'):
        environment['QT_QPA_PLATFORM'] = 'xcb'
    code, output = run([args.viewer, '--capabilities'], environment=environment)
    window = json.loads(output) if code == 0 else {}
    if window.get('receive') is not True:
        return report
    report.update(fullscreen=window.get('fullscreen') is True, minimize=window.get('minimize') is True)
    if system == 'Darwin':
        if not args.macos_probe or not os.access(args.macos_probe, os.X_OK):
            return report
        command = [args.macos_probe]
        if args.request_permission:
            command += ['--request-capture-permission']
        code, output = run(command, 45 if args.request_permission else 4)
        data = json.loads(output) if code == 0 else {}
        available = data.get('sessionOnConsole') is True and data.get('screenLockKnown') is True and data.get('screenLocked') is False
        report['sessionChecked'] = code == 0 and 'sessionOnConsole' in data
        report.update(sessionAvailable=available, receive='supported' if available else 'unsupported')
        if available and data.get('privateApiAvailable') is True:
            report['send'] = 'supported'
            report['permission'] = 'granted' if data.get('capturePermission') is True else 'required'
        return report
    # The probe runs in the GUI session's environment, never imports an SSH display.
    if not (os.environ.get('WAYLAND_DISPLAY') or os.environ.get('DISPLAY')):
        return report
    try:
        import dbus
        bus = dbus.SessionBus()
        saver = dbus.Interface(bus.get_object('org.gnome.ScreenSaver', '/org/gnome/ScreenSaver'), 'org.gnome.ScreenSaver')
        locked = bool(saver.GetActive(timeout=3))
        report['sessionChecked'] = True
        if locked:
            return report
        report.update(sessionAvailable=True, receive='supported')
        import gi
        gi.require_version('Gst', '1.0')
        from gi.repository import Gst
        Gst.init(None)
        _, encoders = run(['ffmpeg', '-hide_banner', '-encoders'])
        if not Gst.ElementFactory.find('pipewiresrc') or 'libx264' not in encoders:
            return report
        # Existence and read access, without creating a ScreenCast session.
        dbus.Interface(bus.get_object('org.gnome.Mutter.ScreenCast', '/org/gnome/Mutter/ScreenCast'),
                       'org.freedesktop.DBus.Introspectable').Introspect(timeout=3)
        dbus.Interface(bus.get_object('org.gnome.Mutter.DisplayConfig', '/org/gnome/Mutter/DisplayConfig'),
                       'org.gnome.Mutter.DisplayConfig').GetCurrentState(timeout=3)
        report.update(send='supported', permission='granted')
    except Exception:
        pass
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--viewer', required=True)
    parser.add_argument('--macos-probe', default='')
    parser.add_argument('--request-permission', action='store_true')
    options = parser.parse_args()
    try:
        result = probe(options)
    except Exception:
        result = dict(send='unsupported', receive='unsupported', permission='unknown', sessionAvailable=False, sessionChecked=False)
    print(json.dumps(result, separators=(',', ':')))
