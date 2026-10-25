import json
import signal
import subprocess
import sys
import time
import tempfile
from pathlib import Path
import dbus

root = Path(__file__).resolve().parents[2]
output = Path(tempfile.mkdtemp(prefix='deskflow-display-validation-'))
output.mkdir(exist_ok=True)
script = str(root / 'tools/virtual-display-prototype/virtual_display_probe.py')
bus = dbus.SessionBus()
config = dbus.Interface(bus.get_object('org.gnome.Mutter.DisplayConfig', '/org/gnome/Mutter/DisplayConfig'),
                        'org.gnome.Mutter.DisplayConfig')

def connectors():
    return sorted(str(m[0][0]) for m in config.GetCurrentState(timeout=5)[1])

baseline = connectors()
summary = {'baseline': baseline, 'cases': []}
doctor = subprocess.run([sys.executable, script], capture_output=True, text=True, timeout=60)
(output / 'doctor.json').write_text(doctor.stdout)
for index, (width, height) in enumerate([(1920,1080)] * 4 + [(1280,720)]):
    case = f'cycle-{index + 1}'
    result = subprocess.run([sys.executable, script, '--exercise-gnome', '--duration', '1',
                             '--width', str(width), '--height', str(height)],
                            capture_output=True, text=True, timeout=30)
    (output / (case + '.json')).write_text(result.stdout)
    (output / (case + '.stderr')).write_text(result.stderr)
    report = json.loads(result.stdout)
    passed = result.returncode == 0 and report.get('passed') and connectors() == baseline
    summary['cases'].append({'name': case, 'passed': bool(passed), 'frames': report.get('frames'),
                              'mode': report.get('mode'), 'firstFrameMs': report.get('firstFrameMs')})
    print(case, 'PASS' if passed else 'FAIL', flush=True)
    if not passed:
        break

for sig in (signal.SIGTERM, signal.SIGKILL):
    case = 'sigterm' if sig == signal.SIGTERM else 'sigkill'
    proc = subprocess.Popen([sys.executable, script, '--exercise-gnome', '--duration', '30'],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    created = False
    try:
        for _ in range(150):
            if set(connectors()) - set(baseline):
                created = True
                break
            if proc.poll() is not None:
                break
            time.sleep(0.1)
        if proc.poll() is None:
            proc.send_signal(sig)
        stdout, stderr = proc.communicate(timeout=15)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5)
    (output / (case + '.stdout')).write_text(stdout)
    (output / (case + '.stderr')).write_text(stderr)
    for _ in range(50):
        if connectors() == baseline:
            break
        time.sleep(0.1)
    clean = connectors() == baseline
    passed = created and clean
    if sig == signal.SIGTERM:
        report = json.loads(stdout)
        passed = passed and report.get('cleanupVerified') and not report.get('passed')
    summary['cases'].append({'name': case, 'passed': bool(passed), 'createdBeforeSignal': created,
                              'topologyRestored': clean, 'returnCode': proc.returncode})
    print(case, 'PASS' if passed else 'FAIL', flush=True)

summary['passed'] = len(summary['cases']) == 7 and all(item['passed'] for item in summary['cases'])
(output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
print('Results: ' + str(output), flush=True)
print(json.dumps(summary, indent=2), flush=True)
sys.exit(0 if summary['passed'] else 1)
