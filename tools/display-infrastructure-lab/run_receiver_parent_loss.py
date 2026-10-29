#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real active-media parent EOF test; synthetic local sender, visible receiver.

Kills only the Popen-owned diagnostic supervisor after its receiver confirms 30
native paints. Process disappearance is not evidence of virtual-output cleanup.
"""
import argparse
import json
import os
from pathlib import Path
import re
import selectors
import signal
import subprocess
import time


def alive(pid):
    status = Path(f"/proc/{pid}/stat")
    if status.exists():
        # Linux zombies have exited and cannot retain a window or media engine.
        return status.read_text().rsplit(")", 1)[1].split()[0] != "Z"
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("smoke")
    parser.add_argument("runtime")
    parser.add_argument("receiver")
    args = parser.parse_args()
    proc = subprocess.Popen([args.smoke, args.runtime, args.receiver],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    receiver = None
    killed = False
    transcript = []
    try:
        selector = selectors.DefaultSelector()
        selector.register(proc.stderr, selectors.EVENT_READ)
        deadline = time.monotonic() + 20
        pending = b""
        while time.monotonic() < deadline and proc.poll() is None and receiver is None:
            for key, _ in selector.select(timeout=0.2):
                data = os.read(key.fileobj.fileno(), 4096)
                if not data:
                    selector.unregister(key.fileobj)
                    break
                pending += data
                while b"\n" in pending:
                    line, pending = pending.split(b"\n", 1)
                    transcript.append(line.decode(errors="replace"))
                    match = re.fullmatch(rb"receiver-presented-30 pid=([1-9][0-9]*)", line)
                    if match:
                        receiver = int(match[1])
        if receiver is None or proc.poll() is not None:
            if proc.poll() is not None:
                transcript.append(proc.stderr.read().decode(errors="replace"))
                transcript.append(proc.stdout.read().decode(errors="replace"))
            raise RuntimeError("supervised receiver did not confirm 30 visible frames: " + " | ".join(transcript))
        # Popen retains the running direct child's identity. Never kill by name.
        proc.kill()
        proc.wait(timeout=5)
        killed = True
        deadline = time.monotonic() + 5
        while alive(receiver) and time.monotonic() < deadline:
            time.sleep(0.05)
        if alive(receiver):
            raise RuntimeError("receiver survived parent IPC EOF deadline")
        print(json.dumps({"scope": "active receiver process parent EOF, no output resource created",
                          "nativePaintsBeforeParentLoss": 30, "parentKilled": killed,
                          "receiverExited": True, "transcript": transcript}))
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5)
        # Do not kill an orphan by PID: the supervisor no longer owns its
        # identity. A surviving worker is a failed test requiring investigation.
        if proc.stdout:
            proc.stdout.close()
        if proc.stderr:
            proc.stderr.close()


if __name__ == "__main__":
    main()
