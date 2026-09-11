#!/usr/bin/env python3
"""Preflight for attempt 2: hashes must equal attempt-1 hashes-before.json."""
from __future__ import annotations

import hashlib
import json
import subprocess
from datetime import datetime, timezone
from pathlib import Path

REPO = Path(r"D:\Projects\takeover-fixes")
SCRATCH = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w1-commit-split\scratch")
BEFORE = json.loads((SCRATCH / "hashes-before.json").read_text(encoding="utf-8"))
HANDOFF = json.loads(Path(r"D:\Projects\reviews\takeover-20260909\handoff-20260910\manifest.json").read_text(encoding="utf-8"))
LOG = SCRATCH.parent / "commands2.log"


def log(line: str, extra: str = "") -> None:
    with LOG.open("a", encoding="utf-8") as fh:
        fh.write(f"{datetime.now(timezone.utc).isoformat()} {line}\n{extra}\n")


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def run(args: list[str]) -> subprocess.CompletedProcess:
    r = subprocess.run(args, cwd=REPO, capture_output=True, text=True)
    log(" ".join(args), f"exit={r.returncode}\n{r.stdout}{r.stderr}")
    r.stdout_text = r.stdout
    return r


def main() -> int:
    LOG.write_text("", encoding="utf-8")
    head = run(["git", "rev-parse", "HEAD"]).stdout_text.strip()
    status = run(["git", "status", "--porcelain"]).stdout_text
    (SCRATCH / "status-before-attempt2.txt").write_text(status, encoding="utf-8")
    tag = run(["git", "rev-parse", "w1-split-attempt1"]).stdout_text.strip()
    stash = run(["git", "stash", "list"]).stdout_text

    status_paths = {}
    for line in status.splitlines():
        if not line:
            continue
        path = line[3:].strip().strip('"').replace("\\", "/")
        status_paths[path] = line[:2]

    hashes = {}
    to_hash = sorted(set(status_paths) | set(BEFORE))
    for path in to_hash:
        full = REPO / path
        if full.is_file():
            hashes[path] = {"sha256": sha256_file(full), "size": full.stat().st_size, "status": status_paths.get(path)}
        else:
            hashes[path] = {"sha256": None, "size": None, "error": "missing", "status": status_paths.get(path)}

    (SCRATCH / "hashes-before-attempt2.json").write_text(json.dumps(hashes, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    changed = []
    only_now = sorted(set(hashes) - set(BEFORE))
    only_old = sorted(set(BEFORE) - set(hashes))
    for path in sorted(set(hashes) & set(BEFORE)):
        if hashes[path].get("sha256") != BEFORE[path].get("sha256"):
            changed.append({
                "path": path,
                "attempt1": BEFORE[path].get("sha256"),
                "now": hashes[path].get("sha256"),
            })

    out = {
        "head": head,
        "head_ok": head == "60cb6981462d30400d372ca778d440c82a5bcbe3",
        "tag_w1_split_attempt1": tag,
        "stash": stash.strip().splitlines(),
        "changed_vs_attempt1": changed,
        "only_now": only_now,
        "only_old": only_old,
        "ok": head == "60cb6981462d30400d372ca778d440c82a5bcbe3" and not changed and not only_now and not only_old,
    }
    (SCRATCH / "preflight-attempt2.json").write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(out, indent=2))
    return 0 if out["ok"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
