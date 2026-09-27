#!/usr/bin/env python3
"""The historical official 13 self-test rows on a POSIX build, through posix_test_runner.make_run and score_selftest.

    python3 run_official13.py --repo <tree> --binary <tree>/build-gcc/CortexCommand --out <dir> --head <sha>

The Mac lane's run_official13.py with its paths as arguments: SELFTESTS is longer than 13 on the wave, so the adapter
names the set instead of asserting the list's length.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

OFFICIAL13 = [
    "controller-frame",
    "net-protocol",
    "net-identity",
    "net-session",
    "net-lockstep",
    "net-match",
    "net-auth",
    "net-admission",
    "net-reconnect",
    "net-reconnect-session",
    "camera-null-scene",
    "rotate-primitive",
    "float-text",
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--head", required=True)
    parser.add_argument("--timeout", type=int, default=300)
    options = parser.parse_args()
    repo, binary, out = options.repo.resolve(), options.binary.resolve(), options.out.resolve()
    tools = repo / "tools"
    os.environ["CCCP_HEADLESS"] = "1"
    os.environ["CCCP_TEST_BINARY"] = str(binary)
    sys.path.insert(0, str(tools))
    import posix_test_runner as posix_runner
    from run_selftests import SELFTESTS, score_selftest

    if Path(posix_runner.__file__).resolve() != tools / "posix_test_runner.py":
        raise SystemExit(f"unexpected posix_test_runner import: {posix_runner.__file__}")
    if not binary.is_file():
        raise SystemExit(f"missing binary: {binary}")
    missing = [name for name in OFFICIAL13 if name not in SELFTESTS]
    if missing:
        raise SystemExit(f"official13 names missing from SELFTESTS: {missing}")
    exe_hash = posix_runner.sha256_of(binary)
    out.mkdir(parents=True, exist_ok=False)
    results = {}
    for name in OFFICIAL13:
        case = out / f"{name}-selftest"
        print(f"running official13 {name}", flush=True)
        isolated = posix_runner.make_run(repo, ["-headless", f"-{name}-selftest"], case, options.timeout, binary=binary)
        try:
            record = isolated.start().finish()
        finally:
            isolated.close()
        stdout_path = case / "stdout.log"
        stdout = stdout_path.read_text(errors="replace") if stdout_path.exists() else ""
        scored = score_selftest(stdout, record.get("exit_code"), bool(record.get("timed_out")), f"{name}-selftest")
        scored.update(binary=record.get("exe_sha256"), runner=record.get("runner"), exit_code_raw=record.get("exit_code"))
        results[name] = scored
        print(json.dumps({"selftest": name, **{k: v for k, v in scored.items() if k != "binary"}}), flush=True)
        if record.get("exe_sha256") != exe_hash:
            raise SystemExit(f"launch binary changed for {name}: {record.get('exe_sha256')}")
    summary = {
        "exe_sha256": exe_hash,
        "head": options.head,
        "repo": str(repo),
        "binary": str(binary),
        "passed": sum(1 for row in results.values() if row["pass"]),
        "total": len(OFFICIAL13),
        "selftests": list(OFFICIAL13),
        "suite_len": len(SELFTESTS),
        "results": results,
        "adapter": "posix_test_runner.make_run + score_selftest; historical official 13",
    }
    (out / "result.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps({"exe_sha256": exe_hash, "passed": summary["passed"], "total": summary["total"]}, indent=2), flush=True)
    return 0 if summary["passed"] == summary["total"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
