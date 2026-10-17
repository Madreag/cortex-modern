import json
from pathlib import Path
import re
import sys

run = Path(sys.argv[1])
summary = json.loads((run / 'summary.json').read_text())
assert summary['complete'], 'Do not report an incomplete gate as complete'
suite = json.loads((run / 'suite-54/result.json').read_text())
inventory = json.loads((run / 'shape-inventory/result.json').read_text())
coverage = json.loads((run / 'shape-inventory/shape-coverage-audit.json').read_text())
driver = json.loads((run / 'random-5000/result.json').read_text())
slow = json.loads((run / 'slow-host-schedules/summary.json').read_text())
driver_log = (run / 'random-5000/net-session-plane-hold-schedules-selftest/stdout.log').read_text()
match = re.search(r'\[net-session-plane-selftest\] random_hold_schedules schedules=(\d+) ran=(\d+) seeds=(\d+)\.\.(\d+) failed=(\d+) threads=(\d+) elapsed_s=(\d+)', driver_log)
assert match, 'Missing the requested schedule completion evidence'
names = ('requested', 'ran', 'first_seed', 'last_seed', 'failed', 'threads', 'elapsed_s')
schedule = dict(zip(names, map(int, match.groups())))
assert schedule['requested'] == schedule['ran'] == 5000
assert schedule['first_seed'] == 1 and schedule['last_seed'] == 5000
def native_fixed_counts(folder):
    text = (folder / 'net-session-plane-selftest/stdout.log').read_text()
    counts = re.findall(r'\[net-session-plane-selftest\] (\d+) of (\d+) passed', text)
    assert counts, 'Missing completed fixed-row summary'
    return dict(zip(('passed', 'total'), map(int, counts[-1])))

failure_rows = {}
for line in driver_log.splitlines():
    if '[net-session-plane-selftest] FAIL random_hold_schedules' in line:
        key = re.search(r'(?:group|shape)=([^ ]+)', line)
        failure_rows[key.group(0) if key else 'summary'] = line
audit = {
    'source_head': summary['source_head'],
    'binary_sha256': summary['binary_sha256'],
    'binary_unchanged_all_stages': all(r['binary_unchanged'] for r in summary['results']),
    'gate_stages': summary['results'],
    'suite': {'passed': suite['passed'], 'total': suite['total'], 'complete': suite['complete'],
              'failed_rows': {k: v for k, v in suite['results'].items() if not v['pass']}},
    'inventory': {'passed': inventory['passed'], 'total': inventory['total'], 'complete': inventory['complete'], 'native_fixed': native_fixed_counts(run / 'shape-inventory')},
    'suite_native_fixed': native_fixed_counts(run / 'suite-54'),
    'shape_counts': {state: sum(v['state'] == state for v in coverage.values()) for state in ('GREEN', 'RED', 'MISSING')},
    'red_shape_keys': [key for key, value in coverage.items() if value['state'] == 'RED'],
    'feel_1': suite['results']['single-player-reference'],
    'slow_host': [{k: v for k, v in row.items() if k not in ('schedule', 'survivor_waits')} for row in slow],
    'random_5000': schedule,
    'random_failures': failure_rows,
    'random_runner': driver,
}
(run / 'completion-audit.json').write_text(json.dumps(audit, indent=2) + '\n')
print(json.dumps({
    'binary_sha256': audit['binary_sha256'], 'binary_unchanged_all_stages': audit['binary_unchanged_all_stages'],
    'suite': {k: v for k, v in audit['suite'].items() if k != 'failed_rows'},
    'failed_rows': list(audit['suite']['failed_rows']),
    'inventory': audit['inventory'], 'suite_native_fixed': audit['suite_native_fixed'], 'shape_counts': audit['shape_counts'],
    'feel_1': {'pass': audit['feel_1']['pass'], 'observed': audit['feel_1'].get('observed')},
    'slow_host': audit['slow_host'], 'random_5000': schedule,
}, indent=2))
