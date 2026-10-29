#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Developer-only private runtime copy. No installation/signing certification."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil

PLUGINS = ('coreelements debugutilsbad typefindfunctions app videoconvertscale videorate videoparsersbad '
           'x264 libav rtp rtpmanager rtpmanagerbad nice dtls srtp webrtc rswebrtc rsrtp').split()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk-prefix', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    prefix = args.sdk_prefix.resolve(strict=True)
    output = args.output.absolute()
    if output.exists():
        parser.error('Output must be new; existing runtimes are never overwritten')
    output.mkdir(mode=0o700)
    lib = output / 'lib'
    lib.mkdir(mode=0o700)
    plugins = lib / 'gstreamer-1.0'
    plugins.mkdir(mode=0o700)
    roots = [prefix / 'lib', *sorted((prefix / 'lib').glob('*-linux-gnu'))]
    hashes = {}
    def copy(source, target):
        if target.exists():
            if hashlib.sha256(source.read_bytes()).digest() != hashlib.sha256(target.read_bytes()).digest():
                raise RuntimeError(f'Conflicting runtime library: {target.name}')
            return
        shutil.copyfile(source, target)
        target.chmod(0o555)
        hashes[str(target.relative_to(output))] = hashlib.sha256(target.read_bytes()).hexdigest()
    for root in roots:
        for source in sorted(root.iterdir()):
            if source.is_file() and (source.suffix == '.dylib' or '.so' in source.name):
                copy(source, lib / source.name)
    for name in PLUGINS:
        choices = [p for root in roots for p in (root / 'gstreamer-1.0').glob(f'libgst{name}.*')
                   if p.suffix in ('.dylib', '.so')]
        if len(choices) != 1:
            raise RuntimeError(f'Expected exactly one plugin {name}, got {choices}')
        copy(choices[0], plugins / choices[0].name)
    (output / 'runtime-sha256.json').write_text(json.dumps(hashes, indent=2) + '\n')
    print(json.dumps({'runtimeRoot': str(output), 'files': len(hashes)}, indent=2))

if __name__ == '__main__':
    main()
