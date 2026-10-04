"""Compare two single-player runs of the same fixture: raw dump bytes and per-tick hashes."""
import hashlib
import json
from pathlib import Path
import sys

control, tip, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def run_of(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))['runs'][0]

# Only the run's wall-clock measurement is left out of the comparison; its two values are reported.
def hashes(path):
    run = run_of(path)
    compared = {key: run[key] for key in ('scenario', 'seed', 'ticks', 'passed', 'sim_config', 'numeric', 'strings', 'final_total_hash', 'tick_hashes')}
    compared['numeric'] = {key: value for key, value in compared['numeric'].items() if key != '__wall_seconds'}
    return compared

report = {
    'dump_sha256': [digest(root / 'trace.json.simdump.txt') for root in (control, tip)],
    'dump_bytes': [(root / 'trace.json.simdump.txt').stat().st_size for root in (control, tip)],
    'trace_file_sha256': [digest(root / 'trace.json') for root in (control, tip)],
    'exit_codes': [json.loads((root / 'sp_summary.json').read_text(encoding='utf-8-sig'))['exit_code'] for root in (control, tip)],
    'timed_out': [json.loads((root / 'sp_summary.json').read_text(encoding='utf-8-sig'))['timed_out'] for root in (control, tip)],
}
report['wall_seconds'] = [run_of(root / 'trace.json')['numeric']['__wall_seconds'] for root in (control, tip)]
report['tick_hashes_equal'] = hashes(control / 'trace.json') == hashes(tip / 'trace.json')
report['tick_hashes_only_equal'] = run_of(control / 'trace.json')['tick_hashes'] == run_of(tip / 'trace.json')['tick_hashes']
report['final_total_hash'] = [run_of(root / 'trace.json')['final_total_hash'] for root in (control, tip)]
report['tick_count'] = len(hashes(control / 'trace.json')['tick_hashes'])
report['dump_equal'] = report['dump_sha256'][0] == report['dump_sha256'][1]
report['trace_file_equal'] = report['trace_file_sha256'][0] == report['trace_file_sha256'][1]
report['pass'] = report['dump_equal'] and report['tick_hashes_equal'] and report['exit_codes'] == [0, 0] and report['timed_out'] == [False, False]
out.write_text(json.dumps(report, indent=2), encoding='utf-8')
print(json.dumps(report, indent=2))
raise SystemExit(0 if report['pass'] else 1)
