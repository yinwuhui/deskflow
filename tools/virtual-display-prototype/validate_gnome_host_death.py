#!/usr/bin/env python3
# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT
"""Explicit real-GNOME topology test: kill the session owner, verify helper cleanup."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

from display_transport import init_identity
from validate_live_session import ready


def running(pid):
    try:
        return Path(f'/proc/{pid}/stat').read_text().split(') ', 1)[1][0] != 'Z'
    except FileNotFoundError:
        return False


def main():
    import dbus
    bus = dbus.SessionBus()
    config = dbus.Interface(bus.get_object('org.gnome.Mutter.DisplayConfig',
        '/org/gnome/Mutter/DisplayConfig'), 'org.gnome.Mutter.DisplayConfig')
    def topology():
        return sorted(str(monitor[0][0]) for monitor in config.GetCurrentState(timeout=5)[1])
    before = topology()
    host = receiver = None
    helpers = []
    with tempfile.TemporaryDirectory(prefix='deskflow-host-death-') as directory:
        root = Path(directory)
        for name in ('host', 'receiver'):
            init_identity(root/name)
        script = str(Path(__file__).with_name('display_session.py'))
        try:
            host = subprocess.Popen([sys.executable, script, 'serve', '--identity', str(root/'host'),
                '--peer-cert', str(root/'receiver/cert.pem'), '--source', 'gnome', '--duration', '30',
                '--port', '0'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            port = ready(host)
            receiver = subprocess.Popen([sys.executable, script, 'receive', '--identity', str(root/'receiver'),
                '--peer-cert', str(root/'host/cert.pem'), '--port', str(port)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            deadline = time.monotonic() + 15
            during = before
            while time.monotonic() < deadline and host.poll() is None:
                during = topology()
                helpers = [int(value) for value in Path(
                    f'/proc/{host.pid}/task/{host.pid}/children').read_text().split()]
                if set(during) - set(before) and helpers:
                    break
                time.sleep(0.1)
            if not set(during) - set(before) or not helpers:
                raise RuntimeError('No live source display/helper before fault injection')
            time.sleep(0.5)
            started = time.monotonic()
            host.kill()
            host.communicate(timeout=5)
            output, error = receiver.communicate(timeout=15)
            deadline = started + 15
            after = topology()
            while time.monotonic() < deadline:
                after = topology()
                if after == before and not any(running(pid) for pid in helpers):
                    break
                time.sleep(0.1)
            report = {'source': 'gnome', 'fault': 'host SIGKILL', 'hostExitCode': host.returncode,
                'receiverExitCode': receiver.returncode, 'receiver': json.loads(output),
                'connectorsBefore': before, 'connectorsDuring': during, 'connectorsAfter': after,
                'helpersExited': not any(running(pid) for pid in helpers),
                'cleanupMs': round((time.monotonic()-started)*1000, 2)}
            report['passed'] = (host.returncode == -signal.SIGKILL and receiver.returncode == 1 and
                                after == before and report['helpersExited'])
            print(json.dumps(report, indent=2), flush=True)
            return 0 if report['passed'] else 1
        finally:
            for process in (receiver, host):
                if process and process.poll() is None:
                    process.terminate()
                    try:
                        process.communicate(timeout=25)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.communicate(timeout=5)
            for pid in helpers:
                if running(pid):
                    os.kill(pid, signal.SIGTERM)


if __name__ == '__main__':
    sys.exit(main())
