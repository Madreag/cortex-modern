"""Same -script-graph-selftest argv on the base tree and the tip.

The dump-identity detector is the in-process highlight_dump_unchanged /
highlight_sp_mp_dump_identity guard. Both binaries already print
[piemenu-checkpoint-selftest]; this driver scores that token, not a tip-only
[highlight-dump] line. Written, not run.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys

REPO = Path(__file__).resolve().parents[1]
SELFTEST = re.compile(r"\[piemenu-checkpoint-selftest\]")
ARGV = ["-script-graph-selftest"]


def sha(path: Path) -> str:
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def peer_log(run_dir: Path) -> str:
    text = ""
    for name in ("stdout.log", "runtime/LogConsole.txt"):
        path = run_dir / name
        if path.exists():
            text += path.read_text(encoding="utf-8", errors="replace")
    return text


def inspect(root: Path) -> dict:
    checks = {}
    details = {"detector": "in-process highlight_dump_unchanged"}
    for who in ("base", "tip"):
        found = bool(SELFTEST.search(peer_log(root / who)))
        details[who] = found
        checks[f"{who}_selftest"] = found
    return {"pass": all(checks.values()), "checks": checks, "details": details}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", type=Path, default=Path(r"D:/Projects/takeover-build"))
    parser.add_argument("--tip", type=Path, default=REPO)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=180)
    options = parser.parse_args()
    sys.path.insert(0, str(options.tip / "tools"))
    from run_sim_test import make_run

    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    os.environ["CCCP_HEADLESS"] = "1"
    runs = {}
    try:
        for who, repo in (("base", options.base), ("tip", options.tip)):
            env = {"CCCP_HEADLESS": "1"}
            run = make_run(repo, list(ARGV), root / who, timeout=options.timeout, env=env)
            runs[who] = run
        for run in runs.values():
            run.start()
        for run in runs.values():
            run.wait()
        result = inspect(root)
        result["argv"] = ARGV
        result["driver_sha256"] = sha(Path(__file__))
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print("PASS" if result["pass"] else "FAIL", "highlight_dump_identity", result["checks"])
        return 0 if result["pass"] else 1
    finally:
        for run in runs.values():
            run.close()


if __name__ == "__main__":
    raise SystemExit(main())
