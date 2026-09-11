#!/usr/bin/env python3
"""Execute the planned per-concern commits. Never stages *.lib."""
from __future__ import annotations

import json
import subprocess
from datetime import datetime, timezone
from pathlib import Path

REPO = Path(r"D:\Projects\takeover-fixes")
SCRATCH = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w1-commit-split\scratch")
PLAN = json.loads((SCRATCH / "plan.json").read_text(encoding="utf-8"))
COMMANDS_LOG = SCRATCH.parent / "commands.log"
MSGS = SCRATCH / "commit-msgs"
MSGS.mkdir(parents=True, exist_ok=True)


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
    if result.returncode != 0:
        raise SystemExit(f"command failed ({result.returncode}): {' '.join(args)}\n{stderr}\n{stdout}")
    return result


def status_paths() -> dict[str, str]:
    out = run(["git", "status", "--porcelain"]).stdout_text
    paths = {}
    for line in out.splitlines():
        if not line:
            continue
        path = line[3:].strip().strip('"').replace("\\", "/")
        paths[path] = line[:2]
    return paths


def main() -> int:
    commits = PLAN["commits"]
    excluded = set(PLAN["excluded_from_commits"])
    planned = []
    seen = set()
    for i, commit in enumerate(commits, 1):
        for path in commit["files"]:
            if path in seen:
                raise SystemExit(f"file in two commits: {path}")
            if path in excluded or path.endswith(".lib"):
                raise SystemExit(f"plan includes excluded lib: {path}")
            seen.add(path)
            planned.append(path)

    dirty = status_paths()
    dirty_commit = {p for p in dirty if p not in excluded and not p.endswith(".lib")}
    missing = sorted(dirty_commit - seen)
    extra = sorted(seen - dirty_commit)
    coverage = {
        "planned": len(seen),
        "dirty_non_lib": len(dirty_commit),
        "missing_from_plan": missing,
        "planned_but_not_dirty": extra,
        "libs": sorted(p for p in dirty if p.endswith(".lib") or p in excluded),
    }
    (SCRATCH / "plan-coverage.json").write_text(json.dumps(coverage, indent=2) + "\n", encoding="utf-8")
    if missing or extra:
        print(json.dumps(coverage, indent=2))
        raise SystemExit("plan does not cover the dirty tree exactly")

    results = []
    for i, commit in enumerate(commits, 1):
        msg = commit["subject"] + "\n\n" + commit["body"].rstrip() + "\n"
        msg_path = MSGS / f"{i:02d}.txt"
        msg_path.write_text(msg, encoding="utf-8", newline="\n")
        add_cmd = ["git", "add", "--", *commit["files"]]
        run(add_cmd)
        staged = run(["git", "diff", "--cached", "--name-only"]).stdout_text.splitlines()
        staged_norm = [p.replace("\\", "/") for p in staged]
        if sorted(staged_norm) != sorted(commit["files"]):
            raise SystemExit(f"staged mismatch for commit {i}: {staged_norm} vs {commit['files']}")
        # refuse libs
        if any(p.endswith(".lib") for p in staged_norm):
            raise SystemExit("lib staged")
        run(["git", "commit", "-F", str(msg_path)])
        sha = run(["git", "rev-parse", "HEAD"]).stdout_text.strip()
        subject = run(["git", "log", "-1", "--format=%s"]).stdout_text.strip()
        results.append({
            "index": i,
            "hash": sha,
            "subject": subject,
            "file_count": len(commit["files"]),
            "files": commit["files"],
        })
        print(f"committed {i} {sha[:12]} {subject}", flush=True)

    porcelain = run(["git", "status", "--porcelain"]).stdout_text
    (SCRATCH / "status-after-commits.txt").write_text(porcelain, encoding="utf-8")
    leftover = []
    for line in porcelain.splitlines():
        path = line[3:].strip().strip('"').replace("\\", "/")
        leftover.append(path)
    (SCRATCH / "commit-results.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    print("leftover", leftover)
    if set(leftover) != set(excluded) and set(leftover) != {p.replace("\\", "/") for p in excluded}:
        # allow quoted allegro path variants
        leftover_set = set(leftover)
        if leftover_set != set(excluded):
            print("STATUS:\n" + porcelain)
            raise SystemExit("status after commits is not exactly the two libs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
