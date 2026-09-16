"""Argv-identical highlight dumps: same flags on the base tree and the tip.

Launches -script-graph-selftest on both executables with identical argv and
compares the [highlight-dump] PieMenuRuntime1 blobs byte-for-byte. The in-process
highlight_sp_mp_dump_identity row stays as a tip-only guard. Written, not run.
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
DUMP = re.compile(r"\[highlight-dump\] role=(sp|mp) (15 PieMenuRuntime1 .*)")
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


def dumps_of(text: str) -> dict[str, str]:
    found = {}
    for role, blob in DUMP.findall(text):
        found[role] = blob
    return found


def inspect(root: Path) -> dict:
    checks = {}
    details = {"dumps": {}}
    blobs = {}
    for who in ("base", "tip"):
        found = dumps_of(peer_log(root / who))
        details["dumps"][who] = {role: found[role][:32] + "..." if role in found else None for role in ("sp", "mp")}
        checks[f"{who}_sp"] = "sp" in found
        checks[f"{who}_mp"] = "mp" in found
        if "sp" in found and "mp" in found:
            checks[f"{who}_sp_mp_identical"] = found["sp"] == found["mp"]
            blobs[who] = found["sp"]
        else:
            checks[f"{who}_sp_mp_identical"] = False
    checks["base_tip_identical"] = "base" in blobs and "tip" in blobs and blobs["base"] == blobs["tip"]
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
