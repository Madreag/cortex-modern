"""Run socket-free engine selftests and score them from stdout PASS tokens.

A suite that exits 0 with no `[<selftest>] PASS` line is FAIL. A FAIL token or a
missing final PASS is FAIL. Use --score-stdout to score a captured log without a launch.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

SELFTESTS = [
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
]
FATAL = re.compile(
    r"^.*(?:\bFAIL\b|RTE Assert|RTE Abort|stack traceback|Stack trace \(most recent call last\)).*$",
    re.M,
)
SUITE_PASS = re.compile(r"^\[(?P<tag>[^\]]+)\] PASS\s*$", re.M)
SUITE_FAIL = re.compile(r"^\[(?P<tag>[^\]]+)\] FAIL", re.M)


def score_selftest(stdout: str, exit_code, timed_out=False, name=None) -> dict:
    """PASS only with exit 0, no timeout, no FATAL, at least one [tag] PASS, no [tag] FAIL, last suite token PASS."""
    fatal = FATAL.findall(stdout or "")
    pass_matches = list(SUITE_PASS.finditer(stdout or ""))
    fail_matches = list(SUITE_FAIL.finditer(stdout or ""))
    pass_lines = [m.group(0) for m in pass_matches]
    fail_lines = [m.group(0) for m in fail_matches]
    expected = f"[{name}] PASS" if name else None
    named_pass = any(m.group("tag") == name for m in pass_matches) if name else bool(pass_matches)
    last_pass = False
    if pass_matches or fail_matches:
        last = max(
            [(m.end(), "PASS") for m in pass_matches] + [(m.end(), "FAIL") for m in fail_matches],
            key=lambda item: item[0],
        )
        last_pass = last[1] == "PASS"
    ok = (
        exit_code == 0
        and not timed_out
        and not fatal
        and not fail_lines
        and len(pass_lines) >= 1
        and named_pass
        and last_pass
    )
    reason = ""
    if exit_code != 0:
        reason = f"exit_code={exit_code}"
    elif timed_out:
        reason = "timed_out"
    elif fatal:
        reason = f"fatal={fatal[0]}"
    elif fail_lines:
        reason = f"FAIL token: {fail_lines[0]}"
    elif len(pass_lines) < 1:
        reason = "zero PASS tokens"
    elif name and not named_pass:
        reason = f"missing {expected}"
    elif not last_pass:
        reason = "missing final PASS"
    return {
        "pass": bool(ok),
        "exit_code": exit_code,
        "timed_out": timed_out,
        "pass_lines": len(pass_lines),
        "fail_lines": fail_lines[:3],
        "fatal": fatal[:3],
        "reason": reason,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--score-stdout", type=Path, help="score a captured stdout log; no engine")
    parser.add_argument("--exit-code", type=int, default=0)
    parser.add_argument("--timed-out", action="store_true")
    parser.add_argument("--name", default="controller-frame-selftest")
    options = parser.parse_args()

    if options.score_stdout:
        stdout = options.score_stdout.read_text(encoding="utf-8", errors="replace")
        scored = score_selftest(stdout, options.exit_code, options.timed_out, options.name)
        print(json.dumps(scored, indent=2))
        if scored["pass"]:
            print(f"PASS {options.name} pass_lines={scored['pass_lines']}")
        else:
            print(f"FAIL {options.name}: {scored['reason']}")
        return 0 if scored["pass"] else 1

    if options.repo is None or options.out is None:
        parser.error("--repo and --out are required unless --score-stdout is set")
    sys.path.insert(0, str(options.repo / "tools"))
    from run_sim_test import make_run  # noqa: PLC0415

    out = options.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    exe = options.repo / "Cortex Command.exe"
    with exe.open("rb") as stream:
        exe_hash = hashlib.file_digest(stream, "sha256").hexdigest()
    results = {}
    for name in SELFTESTS:
        case = out / f"{name}-selftest"
        run = make_run(options.repo, [f"-{name}-selftest"], case, options.timeout)
        try:
            record = run.start().finish()
        finally:
            run.close()
        stdout = (
            (case / "stdout.log").read_text(errors="replace")
            if (case / "stdout.log").exists()
            else ""
        )
        scored = score_selftest(
            stdout, record.get("exit_code"), record.get("timed_out"), f"{name}-selftest"
        )
        scored["binary"] = record.get("exe_sha256")
        results[name] = scored
        print(json.dumps({"selftest": name, **{k: v for k, v in scored.items() if k != "binary"}}), flush=True)
    summary = {
        "exe_sha256": exe_hash,
        "repo": str(options.repo),
        "passed": sum(1 for r in results.values() if r["pass"]),
        "total": len(SELFTESTS),
        "results": results,
    }
    (out / "result.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps({k: summary[k] for k in ("exe_sha256", "passed", "total")}, indent=2))
    return 0 if summary["passed"] == summary["total"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
