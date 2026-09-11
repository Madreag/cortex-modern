#!/usr/bin/env python3
from __future__ import annotations

import json
import re
import subprocess
from datetime import datetime, timezone
from pathlib import Path

REPO = Path(r"D:\Projects\takeover-fixes")
SCRATCH = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w1-commit-split\scratch")
COMMANDS_LOG = SCRATCH.parent / "commands.log"
BASE = "60cb6981462d30400d372ca778d440c82a5bcbe3"
ATTR_RE = re.compile(r"co-authored|claude|codex|grok|chatgpt|openai|anthropic|cursor|generated", re.I)


def log(line: str, extra: str = "") -> None:
    with COMMANDS_LOG.open("a", encoding="utf-8") as fh:
        fh.write(f"{datetime.now(timezone.utc).isoformat()} cwd={REPO}\n{line}\n")
        if extra:
            fh.write(extra)
            if not extra.endswith("\n"):
                fh.write("\n")
        fh.write("\n")


def run(args: list[str]) -> subprocess.CompletedProcess:
    print("+ " + " ".join(args), flush=True)
    result = subprocess.run(args, cwd=REPO, capture_output=True)
    stdout = result.stdout.decode("utf-8", errors="replace")
    stderr = result.stderr.decode("utf-8", errors="replace")
    extra = f"exit={result.returncode}\n"
    if stdout:
        extra += "--- stdout ---\n" + stdout
        if not stdout.endswith("\n"):
            extra += "\n"
    if stderr:
        extra += "--- stderr ---\n" + stderr
        if not stderr.endswith("\n"):
            extra += "\n"
    log(" ".join(args), extra)
    result.stdout_text = stdout
    result.stderr_text = stderr
    return result


def main() -> int:
    diff_head = run(["git", "diff", "--stat", "HEAD"])
    (SCRATCH / "diff-stat-HEAD.txt").write_text(diff_head.stdout_text, encoding="utf-8")
    split = run(["git", "diff", "--stat", BASE, "HEAD"])
    (SCRATCH / "split-stat.txt").write_text(split.stdout_text, encoding="utf-8")
    log_body = run(["git", "log", "--format=%B", f"{BASE}..HEAD"])
    raw = log_body.stdout_text
    hits = []
    for i, line in enumerate(raw.splitlines(), 1):
        if ATTR_RE.search(line):
            hits.append(f"{i}:{line}")
    scan = "attribution scan 60cb698146..HEAD\npattern: co-authored|claude|codex|grok|chatgpt|openai|anthropic|cursor|generated\n"
    if hits:
        scan += "HITS\n" + "\n".join(hits) + "\n"
    else:
        scan += "no matching lines\n"
    (SCRATCH / "attribution-scan.txt").write_text(scan, encoding="utf-8")
    (SCRATCH / "log-60cb698146-HEAD.txt").write_text(raw, encoding="utf-8")
    print("HEAD diff stat:\n" + diff_head.stdout_text)
    print("attribution hits:", hits)
    return 0 if not hits else 4


if __name__ == "__main__":
    raise SystemExit(main())
