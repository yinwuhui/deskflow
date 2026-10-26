#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Manual integration: real outputs/native windows, simulated input ACKs."""
import argparse
import json
import subprocess
import tempfile
from pathlib import Path
from display_transport import init_identity


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--viewer', type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='deskflow-product-runtime-') as directory:
        root = Path(directory)
        for name in ('a', 'b'):
            init_identity(root/name)
            pem = root/name/'combined.pem'
            pem.write_bytes((root/name/'cert.pem').read_bytes()+(root/name/'key.pem').read_bytes())
            pem.chmod(0o600)
        try:
            result = subprocess.run([str(args.binary.resolve()), str(root), str(Path(__file__).resolve().parent), str(args.viewer.resolve())], timeout=100, capture_output=True, text=True)
        except subprocess.TimeoutExpired as error:
            result = subprocess.CompletedProcess([], 1, '', 'Product runtime validation timed out: '+str(error))
        print(result.stdout, end='')
        print(result.stderr, end='')
        if result.returncode:
            # Preserve ownership evidence on a failed run; never erase a dirty journal.
            import shutil
            destination = Path(tempfile.mkdtemp(prefix='deskflow-product-runtime-failed-'))
            shutil.copytree(root, destination/'evidence')
            print(json.dumps({'passed': False, 'evidence': str(destination)}))
        return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
