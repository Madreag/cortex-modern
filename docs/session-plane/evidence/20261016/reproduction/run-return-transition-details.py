import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
from datetime import datetime
from zoneinfo import ZoneInfo

root = Path('/home/erol/cortex-workers/session-plane-engineer')
scratch = Path(__file__).parent
gate = scratch / 'deferred-return-tip-final-validation'
parent = 2954409
binary = root / 'build-gcc/CortexCommand'
out = scratch / 'return-transition-details-8fb'
out.mkdir(exist_ok=False)
expected = json.loads((gate / 'summary.json').read_text())['binary_sha256']

def stamp():
    return datetime.now(ZoneInfo('America/Phoenix')).strftime('%Y-%m-%d %I:%M:%S %p MST')

def busy():
    items = []
    for proc in Path('/proc').iterdir():
        if not proc.name.isdecimal():
            continue
        try:
            args = (proc / 'cmdline').read_bytes().split(b'\0')
            if args and Path(os.fsdecode(args[0])).name in ('ninja', 'CortexCommand', 'CortexCommand.exe'):
                items.append({'pid': int(proc.name), 'exe': os.fsdecode(args[0]), 'cwd': str((proc / 'cwd').resolve())})
        except (OSError, ValueError):
            pass
    return items

env = {k: v for k, v in os.environ.items() if not k.startswith(('CC_TEST_HOLD_SCHEDULE', 'CC_TEST_HOLD_FIXED_FAILURE_REPORT', 'CC_TEST_SESSION_PLANE_CASE', 'CC_TEST_LOCKSTEP_SELFTEST_CASE', 'CCCP_TEST_MATCH_CASE'))}
env.pop('CCCP_HEADLESS', None)
env.update(CC_RUNNER_HEADED='1', DISPLAY=':0', CCCP_TEST_BINARY=str(binary), CC_TEST_HOLD_SCHEDULES='0', CC_TEST_RETURN_INPUT_DETAILS='1')
result = {'binary_sha256': expected, 'began': stamp(), 'rows': [], 'complete': False}
try:
    # The full suite owns multiple sequential native children, so await its
    # orchestrator's completed result before considering a quiet process slot.
    while True:
        suite = json.loads((gate / 'suite-54/result.json').read_text())
        if suite['complete'] and not busy():
            break
        print(stamp(), 'awaiting complete suite and quiet slot', suite.get('running'), flush=True)
        time.sleep(10)
    for name in ('two_direct_host_3s', 'joiner_catching_up_at_105_percent_returns_over_100ms', 'four_seats_held_joiner_catches_up_then_runs_at_round_rate_over_100ms'):
        while busy():
            time.sleep(10)
        assert hashlib.sha256(binary.read_bytes()).hexdigest() == expected
        print(stamp(), 'BEGIN', name, flush=True)
        with (out / (name + '.log')).open('w') as log:
            process = subprocess.run([sys.executable, 'tools/run_selftests.py', '--repo', str(root), '--out', str(out / name), '--timeout', '300', '--only', 'net-session-plane'], cwd=root, env=env | {'CC_TEST_SESSION_PLANE_CASE': name}, stdout=log, stderr=subprocess.STDOUT)
        result['rows'].append({'name': name, 'exit_code': process.returncode, 'ended': stamp(), 'binary_unchanged': hashlib.sha256(binary.read_bytes()).hexdigest() == expected})
        print(stamp(), 'END', result['rows'][-1], flush=True)
    result['complete'] = True
finally:
    result['ended'] = stamp()
    (out / 'proof.json').write_text(json.dumps(result, indent=2) + '\n')
    args = (Path('/proc') / str(parent) / 'cmdline').read_bytes()
    assert b'run-current-gate.py\0deferred-return-tip-final-validation\0' in args
    os.kill(parent, signal.SIGCONT)
    (gate / 'parent-resumed-after-return-details.json').write_text(json.dumps({'pid': parent, 'at': stamp(), 'proof': str(out / 'proof.json'), 'binary_unchanged': hashlib.sha256(binary.read_bytes()).hexdigest() == expected}, indent=2) + '\n')
    print(stamp(), 'resumed our gate parent', flush=True)
