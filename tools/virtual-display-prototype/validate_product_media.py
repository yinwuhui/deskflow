#!/usr/bin/env python3
# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT
"""Explicit real GNOME/native-window media check; not product request/consent certification."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from display_transport import init_identity
from validate_live_session import ready


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--viewer',required=True)
    parser.add_argument('--duration',type=int,default=8)
    args=parser.parse_args()
    if not 2<=args.duration<=60:parser.error('duration must be 2..60')
    script=str(Path(__file__).with_name('display_session.py'))
    host=receiver=None
    with tempfile.TemporaryDirectory(prefix='deskflow-product-media-') as directory:
        root=Path(directory)
        for name in ('host','receiver'):init_identity(root/name)
        try:
            host=subprocess.Popen([sys.executable,script,'serve','--identity',str(root/'host'),
                '--peer-cert',str(root/'receiver/cert.pem'),'--source','gnome','--duration',str(args.duration),
                '--port','0','--product-events'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            port=ready(host)
            receiver=subprocess.Popen([sys.executable,script,'receive','--identity',str(root/'receiver'),
                '--peer-cert',str(root/'host/cert.pem'),'--port',str(port),'--native-viewer',args.viewer,
                '--product-events'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            out,err=receiver.communicate(timeout=args.duration+25)
            host_out,host_err=host.communicate(timeout=25)
            a,b=json.loads(host_out),json.loads(out)
            passed=(host.returncode==receiver.returncode==0 and a.get('passed') and b.get('passed') and
                a.get('firstFramePresented') and b.get('firstFramePresented') and
                a.get('sourceCleanupVerified') and
                a['sentFrames']==b['receivedFrames']==b.get('nativeSubmittedFrames') and a['sentFrames']>0)
            print(json.dumps({'passed':bool(passed),'scope':'real topology/TLS/native paint path, no product consent UI',
                'host':a,'receiver':b,'hostStderr':host_err,'receiverStderr':err},indent=2))
            return 0 if passed else 1
        finally:
            for process in (receiver,host):
                if process and process.poll() is None:
                    process.terminate()
                    try:process.communicate(timeout=25)
                    except subprocess.TimeoutExpired:
                        process.kill();process.communicate(timeout=5)


if __name__=='__main__':sys.exit(main())
