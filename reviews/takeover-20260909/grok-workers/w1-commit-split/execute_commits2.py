#!/usr/bin/env python3
"""Attempt-2 commits: same plan, build lines split into the commits that name them."""
from __future__ import annotations

import hashlib
import json
import subprocess
from datetime import datetime, timezone
from pathlib import Path

REPO = Path(r"D:\Projects\takeover-fixes")
SCRATCH = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w1-commit-split\scratch")
LOG = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w1-commit-split\commands2.log")
PLAN = json.loads((SCRATCH / "plan.json").read_text(encoding="utf-8"))
LINEMAP = json.loads((SCRATCH / "build-lines-map.json").read_text(encoding="utf-8"))
BUILD_FILES = ["RTEA.vcxproj", "RTEA.vcxproj.filters", "Source/Network/meson.build"]
COMMITS = [c for c in PLAN["commits"] if c["subject"] != "Add the new network and moderation sources to the build"]
MSGS = SCRATCH / "commit-msgs"
EXCLUDED = set(PLAN["excluded_from_commits"])
FINAL_DIR = SCRATCH / "positive20-build-final"
INTER_DIR = SCRATCH / "build-intermediates"


def log(line: str, extra: str = "") -> None:
    with LOG.open("a", encoding="utf-8") as fh:
        fh.write(f"{datetime.now(timezone.utc).isoformat()} cwd={REPO}\n{line}\n")
        if extra:
            fh.write(extra if extra.endswith("\n") else extra + "\n")
        fh.write("\n")


def run(args: list[str]) -> subprocess.CompletedProcess:
    print("+ " + " ".join(args), flush=True)
    r = subprocess.run(args, cwd=REPO, capture_output=True)
    stdout = r.stdout.decode("utf-8", errors="replace")
    stderr = r.stderr.decode("utf-8", errors="replace")
    extra = f"exit={r.returncode}\n"
    if stdout:
        extra += "--- stdout ---\n" + stdout
        if not stdout.endswith("\n"):
            extra += "\n"
    if stderr:
        extra += "--- stderr ---\n" + stderr
        if not stderr.endswith("\n"):
            extra += "\n"
    log(" ".join(args), extra)
    r.stdout_text = stdout
    r.stderr_text = stderr
    if r.returncode != 0:
        raise SystemExit(f"failed {args}\n{stderr}\n{stdout}")
    return r


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def construct_intermediate(rel: str, k: int, final_lines: list[str]) -> bytes:
    drop = set()
    for rec in LINEMAP["files"][rel]["added"]:
        if rec["commit_index"] > k:
            drop.add(rec["line_no"] - 1)
    kept = [ln for j, ln in enumerate(final_lines) if j not in drop]
    return "".join(kept).encode("utf-8")


def stage_blob(rel: str, content: bytes) -> str:
    tmp = SCRATCH / "tmp-index-blob"
    tmp.write_bytes(content)
    blob = run(["git", "hash-object", "-w", str(tmp)]).stdout_text.strip()
    run(["git", "update-index", "--cacheinfo", f"100644,{blob},{rel}"])
    return blob


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
    FINAL_DIR.mkdir(parents=True, exist_ok=True)
    INTER_DIR.mkdir(parents=True, exist_ok=True)
    finals = {}
    final_lines = {}
    for rel in BUILD_FILES:
        data = (REPO / rel).read_bytes()
        finals[rel] = data
        dest = FINAL_DIR / rel.replace("/", "__")
        dest.write_bytes(data)
        if sha256_bytes(data) != LINEMAP["files"][rel]["final_sha256"]:
            raise SystemExit(f"{rel} working tree != mapped FINAL")
        final_lines[rel] = data.decode("utf-8").splitlines(keepends=True)

    # Coverage: every dirty non-lib path is either in some commit or a build file
    planned = set()
    for c in COMMITS:
        planned.update(c["files"])
    planned.update(BUILD_FILES)
    dirty = status_paths()
    dirty_commit = {p for p in dirty if p not in EXCLUDED and not p.endswith(".lib")}
    missing = sorted(dirty_commit - planned)
    extra = sorted(planned - dirty_commit)
    if missing or extra:
        raise SystemExit(f"coverage fail missing={missing} extra={extra}")

    results = []
    for i, commit in enumerate(COMMITS, 1):
        regular = [p for p in commit["files"] if p not in BUILD_FILES]
        run(["git", "add", "--", *regular])
        staged_build = []
        for rel in BUILD_FILES:
            touches = any(r["commit_index"] == i for r in LINEMAP["files"][rel]["added"])
            if not touches:
                continue
            content = construct_intermediate(rel, i, final_lines[rel])
            expected = LINEMAP["files"][rel]["intermediates"][str(i)]["sha256"]
            actual = sha256_bytes(content)
            if actual != expected:
                raise SystemExit(f"intermediate mismatch {rel} commit {i}: {actual} != {expected}")
            if i == LINEMAP["files"][rel]["last_touch_commit"] and content != finals[rel]:
                raise SystemExit(f"{rel} last-touch intermediate != FINAL bytes")
            inter_path = INTER_DIR / f"{i:02d}" / rel.replace("/", "__")
            inter_path.parent.mkdir(parents=True, exist_ok=True)
            inter_path.write_bytes(content)
            blob = stage_blob(rel, content)
            staged_build.append({"path": rel, "blob": blob, "sha256": actual, "nlines": content.count(b"\n")})
            print(f"  staged {rel} commit {i} blob={blob} sha={actual[:12]}", flush=True)

        staged = [p.replace("\\", "/") for p in run(["git", "diff", "--cached", "--name-only"]).stdout_text.splitlines()]
        expected_staged = sorted(regular + [b["path"] for b in staged_build])
        if sorted(staged) != expected_staged:
            raise SystemExit(f"staged mismatch commit {i}: {staged} vs {expected_staged}")
        if any(p.endswith(".lib") for p in staged):
            raise SystemExit("lib staged")

        msg_path = MSGS / f"{i:02d}.txt"
        run(["git", "commit", "-F", str(msg_path)])
        sha = run(["git", "rev-parse", "HEAD"]).stdout_text.strip()
        subject = run(["git", "log", "-1", "--format=%s"]).stdout_text.strip()
        results.append({
            "index": i,
            "hash": sha,
            "subject": subject,
            "file_count": len(staged),
            "files": staged,
            "build": staged_build,
        })
        print(f"committed {i} {sha} {subject}", flush=True)

    # Working tree still FINAL; HEAD after last touch == FINAL for build files
    for rel in BUILD_FILES:
        wt = (REPO / rel).read_bytes()
        if wt != finals[rel]:
            raise SystemExit(f"working tree drifted for {rel}")
        head_blob = run(["git", "show", f"HEAD:{rel}"]).stdout
        # git show may be the committed FINAL
        if head_blob != finals[rel]:
            raise SystemExit(f"HEAD:{rel} != FINAL ({len(head_blob)} vs {len(finals[rel])})")

    porcelain = run(["git", "status", "--porcelain"]).stdout_text
    (SCRATCH / "status-after-commits-attempt2.txt").write_text(porcelain, encoding="utf-8")
    leftover = [line[3:].strip().strip('"').replace("\\", "/") for line in porcelain.splitlines() if line]
    (SCRATCH / "commit-results-attempt2.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    print("leftover", leftover)
    if set(leftover) != EXCLUDED:
        raise SystemExit(f"status not exactly the two libs: {leftover}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
