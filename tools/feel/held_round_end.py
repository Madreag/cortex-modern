"""End the retention round while its client is held and require equal final ticks."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from feel.launch_budget import install_memory_guard
from run_sim_test import engine_executable
from test_autosave_restore import judge_retention, peer_log, run_pair


def main():
    install_memory_guard()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--port', type=int, default=49724)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    with engine_executable(args.repo).open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    plan = dict(ticks=700, client_stall='650:1500', end_round_tick=700, port=args.port, exe_sha256=digest)
    (args.out / 'plan.json').write_text(json.dumps(plan, indent=2), encoding='utf-8')
    try:
        records = run_pair(args.repo, args.out, args.port, 700, 2,
                           dict(host=['-net-match-e2e-end-round-tick', '700'],
                                client=['-net-match-e2e-end-round-tick', '700', '-net-test-live-stall', '650:1500']))
        assert '[net-test] live stall frame=650 ' in peer_log(args.out, 'client'), 'the final-window stall never fired'
        assert '[net-match] hold peer=2 ' in peer_log(args.out, 'host'), 'the client was never held'
        details = judge_retention(args.out, 700, records)
        result = dict(passed=True, details=details, plan=plan)
    except Exception as error:
        result = dict(passed=False, reason=f'{type(error).__name__}: {error}', plan=plan)
    (args.out / 'result.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result), flush=True)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
