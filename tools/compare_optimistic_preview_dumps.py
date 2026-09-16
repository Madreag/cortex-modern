"""Compare argv-identical dumps for optimistic preview ghosts.

Same argv on the tip and on the base tree (both binaries already emit the -out
tick-hash file; the base has no event_ledger_ghost_travel probe):

  -net-replay <fixtures>/pickup_fire.ccreplay
  -input-script <fixtures>/pickup_fire.txt
  -max-ticks 221 -num-lua-states 4 -tick-hashes
  -local-prediction-depth 7 -local-prediction-event-ledger 153
  -out <path>

This driver compares those -out files, produced by the same argv, byte-for-byte.
"""

import argparse
import sys
from pathlib import Path


def first_span(left: bytes, right: bytes) -> str:
    n = min(len(left), len(right))
    index = next((i for i in range(n) if left[i] != right[i]), n)
    if index == n and len(left) == len(right):
        return "identical"
    lo = max(0, index - 16)
    hi = index + 16
    return (
        f"first mismatch at {index} lens {len(left)}/{len(right)} "
        f"a={left[lo:hi]!r} b={right[lo:hi]!r}"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tip_dump", type=Path)
    parser.add_argument("base_dump", type=Path)
    args = parser.parse_args()
    tip = args.tip_dump.read_bytes()
    base = args.base_dump.read_bytes()
    if tip != base:
        print("FAIL dumps differ:", first_span(tip, base))
        return 1
    print("PASS dumps byte-identical", len(tip), "bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
