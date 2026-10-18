"""Throw inside the real MovableMan update batch and verify draining, failure propagation and worker context cleanup.

The empty-capture arm requires a fault build that returns an empty exception_ptr from the pool wrapper's catch.
It checks two actual injected captures, so a normal build cannot accidentally satisfy that arm.
For that build only, forward-declare RTE::CapturePoolUpdateFailureForTest() before namespace BS in both pool
headers, and use it in place of std::current_exception() in each submit wrapper. Apply the same injection to
the baseline and fix, then restore both headers byte for byte before building the production executable.
"""
import argparse
import json
from pathlib import Path
import re
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--repo', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--expect-empty', action='store_true')
a = p.parse_args()
sys.path.insert(0, str(a.repo / 'tools'))
from run_sim_test import make_run, seed_settings

a.out.mkdir(parents=True, exist_ok=False)
(a.out / 'driver-as-run.py').write_bytes(Path(__file__).read_bytes())
env = {'CCCP_HEADLESS': '1', 'CC_RUNNER_IGNORE_FULLSCREEN': '1'}
if a.expect_empty:
    env['CC_TEST_POOL_EMPTY_CAPTURE'] = '1'
# The extra update needs ordinary SP input; replay input for this frame was already consumed by Main.
run = make_run(a.repo, ['-scenario', 'SimBaseline', '-seed', '42',
                       '-max-ticks', '152', '-pool-update-selftest'], a.out / 'update', 1800, env=env)
seed_settings(run, {'AutosaveSeconds': '0', 'EnableLuaDebugging': '0'})
try:
    record = run.start().finish()
finally:
    run.close()
stdout = (a.out / 'update/stdout.log').read_text(errors='replace')
rows = re.findall(r'^\[pool-update-selftest\] (PASS|FAIL) (.+)$', stdout, re.M)
values = {key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', rows[0][1])} if len(rows) == 1 else {}
checks = {'normal_exit': record.get('exit_code') == 0 and not record.get('timed_out'),
          'one_passing_consumer': len(rows) == 1 and rows[0][0] == 'PASS',
          'all_tasks_finished': values.get('states', 0) >= 2 and values.get('entered') == values.get('completed') == values.get('states'),
          'every_failure_observed': values.get('thrown') == values.get('observed') == 2,
          'partial_tick_stopped': values.get('continued') == 0,
          'all_workers_clean': values.get('clean_workers') == 1 and values.get('workers', 0) >= 1
                               and values.get('checked_workers') == values.get('workers'),
          'expected_exception': values.get('bad_alloc') == int(not a.expect_empty)
                                and values.get('fallback') == int(a.expect_empty)
                                and values.get('unexpected') == 0,
          'injection_exercised': values.get('empty_capture') == int(a.expect_empty)
                                 and values.get('empty_pointers') == (2 if a.expect_empty else 0)}
result = {'record': record, 'expect_empty_capture': a.expect_empty, 'consumer': values, 'checks': checks,
          'lines': [f'[pool-update-selftest] {status} {fields}' for status, fields in rows], 'pass': all(checks.values())}
(a.out / 'summary.json').write_text(json.dumps(result, indent=2))
print(json.dumps({key: value for key, value in result.items() if key != 'record'}))
raise SystemExit(0 if result['pass'] else 1)
