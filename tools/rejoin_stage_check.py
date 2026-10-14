"""A returning seat whose base cannot be staged is told so quickly and with the reason, and a later attempt brings it back.

Reads a host's stdout.log. Every rejoin the host refused because no base could be staged (`rejoin refused connection=C after Nms:
its base could not be staged: <reason>`, or the older `rejoin refused connection=C: no image could be staged for it in Nms`) must
come within LIMIT_MS of its attempt and name its reason, and after the last such refusal a returning seat must catch up
(`[roster] ... CaughtUp REJOIN_CATCHUP->RUNNING`).

    python tools/rejoin_stage_check.py <host stdout.log> [--expect-refusal]
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

LIMIT_MS = 5000
REFUSED = re.compile(r'^\[net-match\] rejoin refused connection=(\d+) after (\d+)ms: its base could not be staged: (.*)$', re.M)
REFUSED_OLD = re.compile(r'^\[net-match\] rejoin refused connection=(\d+): no image could be staged for it in (\d+)ms(.*)$', re.M)
CAUGHT_UP = re.compile(r'^\[roster\] rev=\d+ seat=(\d+) CaughtUp REJOIN_CATCHUP->RUNNING', re.M)


def check(text: str, expect_refusal: bool = False) -> dict:
    refusals = [dict(at=m.start(), connection=m[1], waited_ms=int(m[2]), reason=m[3].strip()) for m in REFUSED.finditer(text)]
    refusals += [dict(at=m.start(), connection=m[1], waited_ms=int(m[2]), reason=m[3].strip(' ()')) for m in REFUSED_OLD.finditer(text)]
    refusals.sort(key=lambda row: row['at'])
    returns = [m.start() for m in CAUGHT_UP.finditer(text)]
    problems = []
    for refusal in refusals:
        if refusal['waited_ms'] > LIMIT_MS:
            problems.append(f"connection {refusal['connection']} was refused {refusal['waited_ms']} ms into its attempt, over {LIMIT_MS} ms")
        if not refusal['reason']:
            problems.append(f"connection {refusal['connection']} was refused without the reason its base could not be staged")
    if refusals and not any(at > refusals[-1]['at'] for at in returns):
        problems.append('no returning seat caught up after the last refused attempt')
    if expect_refusal and not refusals:
        problems.append('no rejoin was refused for want of a base, though the run made its base fail')
    return dict(status='PASS' if not problems else 'FAIL', refusals=[{k: v for k, v in row.items() if k != 'at'} for row in refusals],
                returns_caught_up=len(returns), problems=problems)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('log', type=Path)
    parser.add_argument('--expect-refusal', action='store_true', help='the run injected a refused base; one refusal must show')
    args = parser.parse_args()
    result = check(args.log.read_text(encoding='utf-8', errors='replace'), args.expect_refusal)
    print(json.dumps(result, indent=2))
    return 0 if result['status'] == 'PASS' else 1


if __name__ == '__main__':
    sys.exit(main())
