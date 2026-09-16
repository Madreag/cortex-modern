"""Require participant-identity proof and host ban-scope arms from the engine selftests."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from run_selftests import score_selftest
from run_sim_test import make_run


IDENTITY_ROWS = (
    "identity: replay/forgery/cross-host proof rejected",
    "identity: one identity survives reconnect; unproven connections refused",
)
SCOPE_ROWS = (
    "scopes: session ban ends with the session; persistent ban survives",
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--case", choices=["identity", "scopes"], required=True)
    parser.add_argument("--timeout", type=float, default=300)
    options = parser.parse_args()
    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    flags = ["-net-protocol-selftest", "-net-session-selftest"] if options.case == "identity" else ["-net-session-selftest"]
    name = "net-protocol-selftest" if options.case == "identity" else "net-session-selftest"
    rows = IDENTITY_ROWS if options.case == "identity" else SCOPE_ROWS
    stdout = ""
    last_record = {}
    for flag in flags:
        run_name = flag.lstrip("-")
        run = make_run(options.repo, [flag], root / run_name, options.timeout)
        try:
            last_record = run.start().finish()
        finally:
            run.close()
        stdout += (root / run_name / "stdout.log").read_text(errors="replace")
    result = score_selftest(stdout, last_record.get("exit_code"), last_record.get("timed_out"), name)
    result["suite_pass"] = result["pass"]
    result["checks"] = {row: f"PASS {row}" in stdout for row in rows}
    result["exe_sha256"] = last_record.get("exe_sha256")
    result["detector_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    result["pass"] = result["pass"] and all(result["checks"].values())
    for row, ok in result["checks"].items():
        print(f"{'PASS' if ok else 'FAIL'} {row}: " + ("present" if ok else "missing"))
    (root / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
