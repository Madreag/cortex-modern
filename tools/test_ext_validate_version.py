"""A stub module with a bad SupportedGameVersion fails -ext-validate with the reason on the console.

    python tools/test_ext_validate_version.py --repo <tree> --out <dir> [--timeout 60]
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


REASON = "[module] StubBad.rte"


def run_case(repo: Path, out: Path, timeout: float = 60.0) -> dict:
    sys.path.insert(0, str(Path(repo) / "tools"))
    from run_sim_test import make_run  # noqa: PLC0415

    out.mkdir(parents=True, exist_ok=True)
    run = make_run(repo, ["-ext-validate-version-selftest"], out / "run", timeout)
    try:
        record = run.start().finish()
    finally:
        run.close()
    stdout = (out / "run/stdout.log").read_text(encoding="utf-8", errors="replace") if (out / "run/stdout.log").is_file() else ""
    reason = next((line for line in stdout.splitlines() if REASON in line), "")
    boxes = 0
    for line in stdout.splitlines():
        if REASON in line and "boxes=" in line:
            try:
                boxes = int(line.rsplit("boxes=", 1)[1].split()[0])
            except ValueError:
                boxes = -1
    dialog = boxes > 0
    exit_code = record.get("exit_code")
    result = {
        "selftest": "ext-validate-version",
        "exit_code": exit_code,
        "timed_out": bool(record.get("timed_out")),
        "reason_line": reason,
        "dialog": dialog,
        "continued": "continued after bad version" in stdout,
        "exe_sha256": record.get("exe_sha256"),
        "stdout": str(out / "run/stdout.log"),
    }
    result["pass"] = bool(reason and exit_code not in (0, None) and not result["timed_out"] and not dialog and not result["continued"])
    result["reason"] = "" if result["pass"] else (
        "no reason line" if not reason else
        f"exit_code={exit_code}" if exit_code in (0, None) else
        "timed_out" if result["timed_out"] else
        "dialog shown" if dialog else
        "continued after abort")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=60.0)
    options = parser.parse_args()
    result = run_case(options.repo.resolve(), options.out.resolve(), options.timeout)
    print(json.dumps(result, indent=2))
    print(f"[ext-validate-version] {'PASS' if result['pass'] else 'FAIL'} {result['reason']}")
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
