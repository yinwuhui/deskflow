#!/usr/bin/env python3
# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT
"""Loopback live TLS/decoder validation. Synthetic by default, never claims display proof."""
import argparse
import json
from pathlib import Path
import selectors
import signal
import subprocess
import sys
import tempfile
import time

from display_transport import init_identity


def ready(process):
    with selectors.DefaultSelector() as selector:
        selector.register(process.stderr, selectors.EVENT_READ)
        if not selector.select(15):
            raise RuntimeError("Host did not become ready")
        line = process.stderr.readline()
    value = json.loads(line)
    if value.get("event") != "listening":
        raise RuntimeError("Unexpected readiness event")
    return value["port"]


def run(root, source, duration, disconnect=False, macos_probe=None):
    script = str(Path(__file__).with_name("display_session.py"))
    command = [sys.executable, script, "serve", "--identity", str(root/"host"),
               "--peer-cert", str(root/"receiver/cert.pem"), "--source", source,
               "--duration", str(duration), "--port", "0"]
    if macos_probe:
        command += ["--macos-probe", macos_probe]
    host = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    receiver = None
    try:
        port = ready(host)
        receiver = subprocess.Popen([sys.executable, script, "receive", "--identity", str(root/"receiver"),
            "--peer-cert", str(root/"host/cert.pem"), "--port", str(port)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        prefix = ""
        if disconnect:
            with selectors.DefaultSelector() as selector:
                selector.register(receiver.stderr, selectors.EVENT_READ)
                if not selector.select(15):
                    raise RuntimeError("No first video before disconnect injection")
                prefix = receiver.stderr.readline()
                if json.loads(prefix).get("event") != "firstVideo":
                    raise RuntimeError("Receiver failed before live disconnect injection")
            time.sleep(0.05)
            receiver.send_signal(signal.SIGTERM)
        out, error = receiver.communicate(timeout=duration+25)
        error = prefix + error
        host_out, host_error = host.communicate(timeout=20)
        a, b = json.loads(host_out), json.loads(out)
        result = {"source":source, "hostExitCode":host.returncode, "receiverExitCode":receiver.returncode,
                  "host":a,"receiver":b,"hostStderr":host_error,"receiverStderr":error}
        cleanup_events = [json.loads(line) for line in host_error.splitlines()
                          if line.startswith('{')]
        cleanup_ok = source == "test" or any(
            event.get("event") == "sourceStopped" and event.get("cleanupVerified") is True
            for event in cleanup_events)
        result["cleanupVerified"] = cleanup_ok
        if disconnect:
            # A failed launch is not proof of handling a live disconnect.
            sent = max((event.get("sentFrames", 0) for event in cleanup_events), default=0)
            result["passed"] = (host.returncode == receiver.returncode == 1 and sent > 0 and
                                b.get("error") == "Interrupted" and cleanup_ok)
        else:
            result["passed"] = (host.returncode == receiver.returncode == 0 and cleanup_ok and
                                a.get("passed") and b.get("passed") and b["decodedFrames"] > 0 and
                                a["sentFrames"] == b["receivedFrames"] == b["decodedFrames"] and
                                b.get("decodedWhileStreaming") and a["tls"] == b["tls"] == "TLSv1.3")
        return result
    finally:
        for process in (receiver,host):
            if process and process.poll() is None:
                process.kill()
                process.communicate(timeout=5)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',choices=('test','gnome','macos'),default='test')
    parser.add_argument('--duration',type=int,default=4)
    parser.add_argument('--macos-probe')
    parser.add_argument('--skip-disconnect',action='store_true')
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='deskflow-live-test-') as directory:
        root=Path(directory)
        for name in ('host','receiver'):init_identity(root/name)
        results=[run(root,args.source,args.duration,macos_probe=args.macos_probe)]
        if not args.skip_disconnect:
            results.append(run(root,args.source,10,disconnect=True,macos_probe=args.macos_probe))
    print(json.dumps({'cases':results,'passed':all(r['passed'] for r in results)},indent=2),flush=True)
    return 0 if all(r['passed'] for r in results) else 1


if __name__=='__main__':sys.exit(main())
