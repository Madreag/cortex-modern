"""Compare both survivors' rematch after the original host is killed."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import sys
import time

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from run_sim_test import make_run
from feel.launch_budget import install_memory_guard
from feel_measure import input_pattern, private_settings, stage_baseline
from feel.retained_resume import compare_live_hashes, read_live_hashes


def main():
    install_memory_guard()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--port', type=int, default=49730)
    args = parser.parse_args()
    root = args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    script = root / 'input.txt'
    input_pattern(script)
    runs, records = {}, {}
    killed = False
    try:
        for index, peer in enumerate(('host', 'first', 'second')):
            flags = ['-net-match-service-e2e', '-net-port', str(args.port), '-net-match-peers', '3',
                     '-net-match-humans', '3', '-net-match-cpu-slots', '0', '-net-match-auto-delay',
                     '-net-match-service-preset', 'Determinism FeelBaseline', '-net-match-service-module', 'UserScenes.rte',
                     '-net-match-ticks', '1200', '-max-ticks', '1200', '-net-match-e2e-rematch',
                     '-net-match-e2e-end-round-tick', '800', '-net-autosave-seconds', '0',
                     '-net-player-name', peer, '-seed', '42', '-tick-hashes', '-input-script', str(script),
                     '-net-match-report', str(root / f'{peer}_report.json'), '-out', str(root / f'{peer}_trace.json'),
                     '-net-live-tick-hashes', str(root / f'{peer}-live.jsonl')]
            flags += ['-net-host'] if peer == 'host' else ['-net-join', '127.0.0.1']
            run = make_run(args.repo, flags, root / peer, timeout=180, env=dict(os.environ, CCCP_HEADLESS='1'))
            stage_baseline(run, window_ticks=1200, humans=3)
            private_settings(run, 60)
            runs[peer] = run
            run.start()
            if index == 0:
                time.sleep(.5)
        deadline = time.monotonic() + 180
        while time.monotonic() < deadline and any(run.poll() is None for run in runs.values()):
            live = root / 'host-live.jsonl'
            if not killed and live.is_file():
                ticks = read_live_hashes(live)
                if ticks and ticks[-1]['tick'] >= 300 and runs['host'].poll() is None:
                    runs['host'].terminate(reason='original host lost before its survivors rematch')
                    killed = True
            time.sleep(.1)
        for peer, run in runs.items():
            if run.poll() is None:
                run.terminate(reason='migrated rematch deadline')
            records[peer] = run.finish()
    finally:
        for run in runs.values():
            run.close()
    logs = {peer: (root / peer / 'stdout.log').read_text(encoding='utf-8-sig', errors='replace') for peer in runs}
    comparison = compare_live_hashes(root / 'first-live.jsonl', root / 'second-live.jsonl', 1)
    restored = {peer: len(re.findall(r'round start scripts restored bytes=', logs[peer])) for peer in ('first', 'second')}
    fresh_rounds = {peer: re.findall(r'\[net-lockstep\] start round=(\d+) frame=1 ', logs[peer]) for peer in ('first', 'second')}
    checks = dict(host_killed=killed,
                  successor_host=sum('is now hosting' in logs[peer] for peer in ('first', 'second')) == 1,
                  survivors_completed=all(records[peer].get('exit_code') == 0 and not records[peer].get('timed_out') for peer in ('first', 'second')),
                  rematch_launched=all(len(set(rounds)) >= 2 for rounds in fresh_rounds.values()),
                  scripts_restored=all(count >= 2 for count in restored.values()),
                  hashes_equal=len(comparison) >= 2 and all(row['compared_ticks'] >= 30 and row['mismatched_ticks'] == 0 and
                                                          row['mismatched_applied_input_ticks'] == 0 for row in comparison))
    result = dict(passed=all(checks.values()), checks=checks, restored=restored, fresh_rounds=fresh_rounds, comparison=comparison,
                  failures=[name for name, passed in checks.items() if not passed])
    (root / 'result.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result), flush=True)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
