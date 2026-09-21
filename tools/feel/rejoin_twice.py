"""Hold the same live client twice and require both returns to finish and agree."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import feel_measure as measure
from feel.retained_resume import compare_live_hashes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--port', type=int, default=49471)
    parser.add_argument('--timeout', type=int, default=300)
    args = parser.parse_args()
    if not 49470 <= args.port <= 49499:
        parser.error('the diagnostic port must remain in 49470..49499')
    root = args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    script = root / 'input.txt'
    measure.input_pattern(script)
    run = measure.launch_case(root, 'twice', 100, 60, True, args.port, script,
                              measure.file_record(measure.REPO / 'Cortex Command.exe')['sha256'], args.timeout,
                              silent_tick=600, live_stalls=[(600, 1500), (1500, 1500)])
    log = (run / 'client/stdout.log').read_text(encoding='utf-8-sig', errors='replace')
    events = re.findall(r'\[net-test\] live stall frame=(\d+) ms=1500|\[net-match\] private catch-up complete frame=(\d+)', log)
    pending, returns = None, []
    for stalled, returned in events:
        if stalled:
            pending = int(stalled)
        elif pending is not None:
            returns.append(dict(stalled=pending, returned=int(returned)))
            pending = None
    comparisons = {peer: compare_live_hashes(run / 'host-live.jsonl', run / f'{peer}-live.jsonl', 1)
                   for peer in ('client', 'survivor')}
    manifest = json.loads((run / 'manifest.json').read_text())
    passed = (manifest['launches_complete'] and len(returns) == 2 and pending is None and
              'resync failed:' not in log and all(passes and all(row['mismatched_ticks'] == 0 and row['compared_ticks'] > 0 for row in passes)
                                                for passes in comparisons.values()))
    line = f"[second-rejoin] {'PASS' if passed else 'FAIL'} returns={len(returns)}"
    measure.write_json(root / 'result.json', dict(passed=passed, final_line=line, returns=returns,
                                                comparisons=comparisons, launches_complete=manifest['launches_complete']))
    print(line, flush=True)
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
