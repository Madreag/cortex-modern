"""Run the tools' own suites: the checks that read the tree and never launch the engine.

`run_selftests.py` scores engine selftests; these read source and fixtures instead, so they run
here. Every suite is a separate process and the exit code is the worst of them.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys

SUITES = (
    ("compare-snapshots", ["test_compare_snapshots.py"]),
    ("snapshot-inventory-roles", ["test_snapshot_inventory_roles.py"]),
    ("snapshot-runtime", ["snapshot_runtime.py", "--self-test"]),
    ("print-discipline", ["test_print_discipline.py"]),
    ("main-arg-loop", ["test_main_arg_loop.py"]),
)


def run(repo: Path, name: str, argv: list[str], timeout: float) -> tuple[str, int, str]:
    command = [sys.executable, str(repo / "tools" / argv[0]), *argv[1:]]
    try:
        done = subprocess.run(command, cwd=str(repo / "tools"), capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return name, 124, f"timed out after {timeout}s"
    return name, done.returncode, (done.stdout + done.stderr)[-2000:]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--timeout", type=float, default=600.0)
    parser.add_argument("--only", action="append", default=[], help="run just these suite names")
    args = parser.parse_args()
    worst = 0
    for name, argv in SUITES:
        if args.only and name not in args.only:
            continue
        name, code, output = run(args.repo.resolve(), name, argv, args.timeout)
        print(f"[tools-suites] {'PASS' if code == 0 else 'FAIL'} {name} exit={code}")
        if code != 0:
            print(output)
            worst = code
    return worst


if __name__ == "__main__":
    raise SystemExit(main())
