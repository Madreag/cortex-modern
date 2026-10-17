"""Archive compact completed kept-source records without changing any test result."""
from pathlib import Path
import hashlib
import json
import re
import subprocess
import sys

root = Path('/home/erol/cortex-workers/session-plane-engineer')
scratch = Path(__file__).parent
gate = scratch / sys.argv[1]
out = root / 'docs/session-plane/evidence' / sys.argv[2]
manifest = json.loads((gate / 'summary.json').read_text())
completion = json.loads((gate / 'completion-audit.json').read_text())
assert manifest['complete']
assert completion['random_5000']['requested'] == completion['random_5000']['ran'] == 5000
assert completion['binary_unchanged_all_stages']
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
assert digest(root / 'build-gcc/CortexCommand') == manifest['binary_sha256']
changed = [n for n, d in manifest['source_files'].items() if digest(root / n) != d]
assert not changed, changed

def git(*args):
    return subprocess.check_output(['git', *args], cwd=root, text=True).strip()

scan = subprocess.run(['git', 'grep', '-n', '-E', '^(<<<<<<< |=======|>>>>>>> )', '--',
                       'Source', 'external/patches', 'tools', 'docs/MENU-UX.md'],
                      cwd=root, text=True, capture_output=True)
assert scan.returncode == 1 and not scan.stdout and not scan.stderr
provenance = {
    'source_head': git('rev-parse', 'HEAD'),
    'origin_relay_findings': git('rev-parse', 'origin/cm-wip/relay-findings'),
    'engine_source_head': manifest['source_head'],
    'cm_dev_e7cdf1dca1_is_ancestor': subprocess.run(['git', 'merge-base', '--is-ancestor', 'e7cdf1dca1', 'HEAD'], cwd=root).returncode == 0,
    'fixture_06a44fb82_is_ancestor': subprocess.run(['git', 'merge-base', '--is-ancestor', '06a44fb82', 'HEAD'], cwd=root).returncode == 0,
    'source_changes_since_freeze': changed,
    'working_tree': git('status', '--porcelain'),
    'binary_sha256': manifest['binary_sha256'],
    'conflict_marker_scan_exit_code': scan.returncode,
    'conflict_markers': scan.stdout,
}
assert provenance['cm_dev_e7cdf1dca1_is_ancestor'] and provenance['fixture_06a44fb82_is_ancestor']
(gate / 'provenance-audit.json').write_text(json.dumps(provenance, indent=2) + '\n')

slow = json.loads((gate / 'slow-host-schedules/summary.json').read_text())
link = {'binary_sha256': manifest['binary_sha256'],
        'scope': 'All positive completed survivor waits in each entire 30-second simulated match; link samples after 5000ms. Unfinished endpoint waits below the bound are not enumerated by these log records.',
        'rows': []}
for row in slow:
    log = gate / 'slow-host-schedules' / row['name'] / 'net-session-plane-selftest/stdout.log'
    samples = []
    for line in log.read_text().splitlines():
        if '[net-peer-path]' not in line:
            continue
        fields = dict(re.findall(r'([a-z0-9_]+)=([^ ]+)', line))
        if int(fields.get('now_ms', 0)) < 5000:
            continue
        samples.append({k: int(fields[k]) for k in ('local', 'peer', 'now_ms', 'frame', 'transport_rtt_ms', 'receipt_p95_ms', 'input_delay_ticks')})
    assert samples
    delays = [s['input_delay_ticks'] for s in samples]
    link['rows'].append({k: v for k, v in row.items() if k != 'survivor_waits'} | {
        'settled_path_sample_count': len(samples), 'settled_delay_min': min(delays),
        'settled_delay_max': max(delays), 'log_sha256': digest(log), 'settled_path_samples': samples})
(gate / 'link-delay-and-survivor-waits.json').write_text(json.dumps(link, indent=2) + '\n')

out.mkdir(parents=True, exist_ok=False)
records = {}
def keep(source, name):
    data = source.read_bytes()
    target = out / name
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    records[name] = {'original': str(source), 'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data)}

for source, name in [
    (gate / 'completion-audit.json', 'gate.json'),
    (gate / 'summary.json', 'run-manifest.json'),
    (gate / 'provenance-audit.json', 'provenance.json'),
    (gate / 'shape-inventory/shape-coverage-audit.json', 'shapes.json'),
    (gate / 'link-delay-and-survivor-waits.json', 'link-delay.json'),
    (gate / 'hold-trigger-audit.json', 'hold-triggers.json'),
    (gate / 'remaining-timing-events.json', 'remaining-timing-events.json'),
    (gate / 'suite-54/result.json', 'suite.json'),
    (gate / 'random-5000/result.json', 'random-driver.json'),
    (gate / 'driver-runtime-audit.json', 'driver-runtime.json'),
    (gate / 'driver-native-dependency.json', 'native-dependency.json'),
    (gate / 'random-5000/net-session-plane-hold-schedules-selftest/launch.json', 'driver-launch.json'),
    (gate / 'suite-54/net-session-plane-selftest/fixed-failure-groups.json', 'fixed-failure-groups.json'),
    (gate / 'build.log', 'build.txt'),
    (scratch / 'C-final-preflight/build.log', 'C-preflight-build.txt'),
    (scratch / 'run-current-gate.py', 'reproduction/run-current-gate.py'),
    (scratch / 'run-slow-host-proof.py', 'reproduction/run-slow-host-proof.py'),
    (scratch / 'audit-shape-coverage.py', 'reproduction/audit-shape-coverage.py'),
    (scratch / 'collect-gate-evidence.py', 'reproduction/collect-gate-evidence.py'),
    (Path(__file__), 'reproduction/archive-kept-gate.py'),
]:
    keep(source, name)

for stage, folder, dest in [
    ('random-5000', 'net-session-plane-hold-schedules-selftest', 'random-5000.txt'),
    ('suite-54', 'net-session-plane-hold-schedules-selftest', 'suite-200.txt'),
    ('shape-inventory', 'net-session-plane-selftest', 'inventory-fixed.txt'),
    ('suite-54', 'net-session-plane-selftest', 'suite-fixed.txt'),
    ('random-5000', 'net-session-plane-selftest', 'driver-fixed.txt'),
]:
    path = gate / stage / folder / 'stdout.log'
    lines = [line for line in path.read_text().splitlines() if '[net-session-plane-selftest]' in line]
    (out / dest).write_text('\n'.join(lines) + '\n')

coordinator = []
for name in ('stdout.log', 'stderr.log'):
    path = gate / 'suite-54/net-lockstep-selftest' / name
    coordinator.append('SOURCE ' + str(path))
    coordinator.extend(line for line in path.read_text().splitlines() if '[net-lockstep-selftest]' in line)
(out / 'coordinator-tests.txt').write_text('\n'.join(coordinator) + '\n')

native = gate / 'suite-54/net-match-selftest/stdout.log'
native_lines = native.read_text().splitlines()
health = [line for line in native_lines if '[net-transport-health]' in line]
(out / 'native-transport.json').write_text(json.dumps({
    'log_sha256': digest(native),
    'native_message_test': [line for line in native_lines if 'two_threads_send_on_one_transport' in line],
    'transport_health_line_count': len(health), 'transport_health_lines': health,
    'scope': 'Native localhost/direct transport only; TURN refresh/drop and spread match remain unproved.',
}, indent=2) + '\n')
for name in ('shape-inventory', 'slow-host-schedules', 'suite-54', 'random-5000'):
    load = json.loads((gate / (name + '-load.json')).read_text())
    summary = {
        'sample_count': len(load),
        'sample_start': load[0]['time'] if load else None,
        'sample_end': load[-1]['time'] if load else None,
        'max_load_1m': max((v['loadavg'][0] for v in load), default=0),
        'foreign_engine_pids': sorted({p['pid'] for v in load for p in v['processes'] if Path(p['exe']).name.startswith('CortexCommand') and p['exe'] != str(root / 'build-gcc/CortexCommand')}),
        'ninja_samples': [v for v in load if any(Path(p['exe']).name == 'ninja' for p in v['processes'])],
    }
    assert not summary['foreign_engine_pids'] and not summary['ninja_samples']
    (out / (name + '-load.json')).write_text(json.dumps(summary, indent=2) + '\n')

index = {
    'engine_source_head': manifest['source_head'], 'binary_sha256': manifest['binary_sha256'],
    'spread_proof': False, 'scope': 'Final kept-source headed Linux single-box validation; every RED retained.',
    'record_copies': records,
    'earlier_evidence': ['../20261016/index.json', '../capture-contract', '../neutral-gap-voters',
                         '../delivered-return-hold', '../remaining-safety', '../return-readiness-trial',
                         '../return-neutral-runway', '../C-final-preflight'],
    'field_A': '../20261016/field-A-frames.json',
    'needs_testing': '../../NEEDS-TESTING.md',
}
index['derived_records'] = {str(p.relative_to(out)): {'sha256': digest(p), 'bytes': p.stat().st_size}
                            for p in out.rglob('*') if p.is_file() and str(p.relative_to(out)) not in records}
(out / 'index.json').write_text(json.dumps(index, indent=2) + '\n')
print(json.dumps({'directory': str(out), 'files': sum(p.is_file() for p in out.rglob('*')),
                  'bytes': sum(p.stat().st_size for p in out.rglob('*') if p.is_file())}))
