#!/usr/bin/env python3
# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT
"""Explicit Mac real-display test: kill session owner and verify display/helper cleanup."""
import argparse,json,os,selectors,signal,subprocess,sys,tempfile,time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from display_transport import init_identity
from validate_live_session import ready
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--macos-probe',required=True)
args=parser.parse_args()
if sys.platform!='darwin':parser.error('This real-display test requires macOS')
probe=str(Path(args.macos_probe).resolve())
script=str(Path(__file__).with_name('display_session.py'))
def displays():
    return json.loads(subprocess.check_output([probe],text=True,timeout=10))['displaysBefore']
def alive(pid):
    result=subprocess.run(['ps','-o','stat=','-p',str(pid)],capture_output=True,text=True)
    return result.returncode==0 and not result.stdout.strip().startswith('Z')
before=displays();host=receiver=None;helpers=[]
with tempfile.TemporaryDirectory(prefix='deskflow-mac-owner-') as directory:
    root=Path(directory)
    for name in ('host','receiver'):init_identity(root/name)
    try:
        host=subprocess.Popen([sys.executable,script,'serve','--identity',str(root/'host'),'--peer-cert',str(root/'receiver/cert.pem'),'--source','macos','--macos-probe',probe,'--duration','30','--port','0'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        port=ready(host)
        receiver=subprocess.Popen([sys.executable,script,'receive','--identity',str(root/'receiver'),'--peer-cert',str(root/'host/cert.pem'),'--port',str(port)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        with selectors.DefaultSelector() as selector:
            selector.register(receiver.stderr,selectors.EVENT_READ)
            if not selector.select(15):raise RuntimeError('No first video')
            if json.loads(receiver.stderr.readline()).get('event')!='firstVideo':raise RuntimeError('Failed before video')
        during=displays()
        helpers=[int(value) for value in subprocess.check_output(['pgrep','-P',str(host.pid)],text=True).split()]
        assert set(during)-set(before) and helpers,'No active source'
        start=time.monotonic();host.kill();host.communicate(timeout=5)
        out,err=receiver.communicate(timeout=15)
        deadline=start+15;after=displays()
        while time.monotonic()<deadline:
            after=displays()
            if after==before and not any(alive(pid) for pid in helpers):break
            time.sleep(.1)
        report={'fault':'Mac host SIGKILL','hostExitCode':host.returncode,'receiverExitCode':receiver.returncode,'displaysBefore':before,'displaysDuring':during,'displaysAfter':after,'helpersExited':not any(alive(pid) for pid in helpers),'cleanupMs':round((time.monotonic()-start)*1000,2)}
        report['passed']=host.returncode==-9 and receiver.returncode==1 and after==before and report['helpersExited']
        print(json.dumps(report,indent=2));sys.exit(0 if report['passed'] else 1)
    finally:
        for process in (receiver,host):
            if process and process.poll() is None:process.terminate();process.communicate(timeout=25)
        for pid in helpers:
            if alive(pid):os.kill(pid,signal.SIGTERM)
