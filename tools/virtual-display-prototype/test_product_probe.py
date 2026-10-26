# SPDX-License-Identifier: MIT
"""Role, permission and fail-closed session regression tests (no real desktop)."""
import argparse
import json
import subprocess
import unittest
import types
import os
from unittest.mock import patch
import product_probe


class CapabilityProbeTests(unittest.TestCase):
    def probe(self, data=None, viewer=True, decoder=True, native=True, permission=False, failed_viewer=False):
        data = data if data is not None else dict(sessionOnConsole=True, screenLockKnown=True,
                screenLocked=False, privateApiAvailable=True, capturePermission=False)
        calls = []
        def run(command, *args, **kwargs):
            calls.append(command)
            if command[0] == 'native':
                return 0, json.dumps(data)
            if command[0] == 'viewer':
                if failed_viewer:
                    raise subprocess.TimeoutExpired(command, 4)
                return 0, json.dumps(dict(receive=True, fullscreen=True, minimize=True))
            return 0, ' h264 ' if decoder else ''
        def access(path, _):
            return viewer if path == 'viewer' else native
        with patch('product_probe.platform.system', return_value='Darwin'), \
             patch('product_probe.shutil.which', return_value='/bin/ffmpeg'), \
             patch('product_probe.os.access', side_effect=access), \
             patch('product_probe.run', side_effect=run):
            result = product_probe.probe(argparse.Namespace(viewer='viewer', macos_probe='native', request_permission=permission))
        return result, calls

    def test_sender_does_not_require_receiver(self):
        for options in [dict(viewer=False), dict(decoder=False), dict(failed_viewer=True)]:
            with self.subTest(options=options):
                report, _ = self.probe(**options)
                self.assertEqual(report['send'], 'supported')
                self.assertEqual(report['permission'], 'required')
                self.assertTrue(report['sessionAvailable'])

    def test_receiver_does_not_require_capture_api_or_permission(self):
        report, _ = self.probe(dict(sessionOnConsole=True, screenLockKnown=True, screenLocked=False,
                                   privateApiAvailable=False, capturePermission=False))
        self.assertEqual(report['receive'], 'supported')
        self.assertEqual(report['send'], 'unsupported')
        self.assertTrue(report['sessionAvailable'])

    def test_unknown_lock_is_not_unlocked_or_unsupported(self):
        report, _ = self.probe(dict(sessionOnConsole=True, screenLockKnown=False, privateApiAvailable=True))
        self.assertFalse(report['sessionAvailable'])
        self.assertFalse(report['sessionChecked'])
        self.assertEqual(report['sessionReason'], 'CapabilityCheckRequired')
        self.assertEqual(report['receive'], 'supported')

    def test_locked_or_off_console_blocks_session(self):
        for on_console, locked in [(True, True), (False, False)]:
            report, _ = self.probe(dict(sessionOnConsole=on_console, screenLockKnown=True, screenLocked=locked,
                                       privateApiAvailable=True))
            self.assertTrue(report['sessionChecked'])
            self.assertFalse(report['sessionAvailable'])
            self.assertEqual(report['sessionReason'], 'SessionUnavailable')

    def test_permission_only_requested_explicitly(self):
        for request in [False, True]:
            report, calls = self.probe(permission=request)
            native = next(call for call in calls if call[0] == 'native')
            self.assertEqual('--request-capture-permission' in native, request)
            self.assertEqual(report['receive'], 'supported')

    def test_native_sender_does_not_need_ffmpeg(self):
        with patch('product_probe.shutil.which', return_value=None):
            # Use the underlying call directly because self.probe supplies FFmpeg.
            with patch('product_probe.platform.system', return_value='Darwin'), \
                 patch('product_probe.os.access', return_value=True), \
                 patch('product_probe.run', return_value=(0, json.dumps(dict(
                     privateApiAvailable=True, capturePermission=True, sessionOnConsole=True,
                     screenLockKnown=True, screenLocked=False)))):
                report = product_probe.probe(argparse.Namespace(viewer='viewer', macos_probe='native', request_permission=False))
        self.assertEqual(report['send'], 'supported')
        self.assertEqual(report['receive'], 'unsupported')

    def test_linux_receiver_survives_missing_capture_dependencies(self):
        saver = types.SimpleNamespace(GetActive=lambda **kw: False)
        dbus = types.SimpleNamespace(SessionBus=lambda: types.SimpleNamespace(get_object=lambda *a: saver),
                                     Interface=lambda obj, interface: obj)
        def run(command, *a, **kw):
            if command[0] == 'viewer':
                return 0, json.dumps(dict(receive=True))
            return 0, ' h264 '
        with patch('product_probe.platform.system', return_value='Linux'), \
             patch('product_probe.shutil.which', return_value='/bin/ffmpeg'), \
             patch('product_probe.os.access', return_value=True), \
             patch('product_probe.run', side_effect=run), \
             patch.dict(os.environ, {'DISPLAY': ':0'}), \
             patch.dict('sys.modules', {'dbus': dbus, 'gi': None}):
            report = product_probe.probe(argparse.Namespace(viewer='viewer', macos_probe='', request_permission=False))
        self.assertEqual(report['receive'], 'supported')
        self.assertTrue(report['sessionAvailable'])
        self.assertEqual(report['send'], 'unknown')

    def test_native_probe_missing_keeps_receiver_capability(self):
        report, _ = self.probe(native=False)
        self.assertEqual(report['receive'], 'supported')
        self.assertEqual(report['send'], 'unknown')
        self.assertFalse(report['sessionAvailable'])


if __name__ == '__main__':
    unittest.main()
