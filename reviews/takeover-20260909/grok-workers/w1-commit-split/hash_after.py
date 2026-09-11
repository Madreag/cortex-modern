#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import json
import subprocess
from datetime import datetime, timezone
from pathlib import Path

REPO = Path(r"D:\Projects\takeover-fixes")
SCRATCH = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w1-commit-split\scratch")
HANDOFF = Path(r"D:\Projects\reviews\takeover-20260909\handoff-20260910\manifest.json")
COMMANDS_LOG = SCRATCH.parent / "commands.log"


def log(line: str, extra: str = "") -> None:
    with COMMANDS_LOG.open("a", encoding="utf-8") as fh:
        fh.write(f"{datetime.now(timezone.utc).isoformat()} cwd={REPO}\n{line}\n{extra}\n")


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def run(args: list[str]) -> str:
    r = subprocess.run(args, cwd=REPO, capture_output=True, text=True)
    log(" ".join(args), f"exit={r.returncode}\n{r.stdout}")
    return r.stdout


def main() -> int:
    status = run(["git", "status", "--porcelain"])
    (SCRATCH / "status-after-wip.txt").write_text(status, encoding="utf-8")
    status_paths = {}
    for line in status.splitlines():
        if not line:
            continue
        path = line[3:].strip().strip('"').replace("\\", "/")
        status_paths[path] = line[:2]

    manifest = json.loads(HANDOFF.read_text(encoding="utf-8"))
    files = manifest["files"]
    hashes = {}
    to_hash = sorted(set(status_paths) | set(files))
    for path in to_hash:
        full = REPO / path
        entry = {"status": status_paths.get(path, "committed-or-absent")}
        if full.is_file():
            entry["sha256"] = sha256_file(full)
            entry["size"] = full.stat().st_size
        else:
            entry["sha256"] = None
            entry["size"] = None
            entry["error"] = "missing"
        hashes[path] = entry
    (SCRATCH / "hashes-after.json").write_text(json.dumps(hashes, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    mismatches = []
    missing = []
    extra = []
    for path in sorted(status_paths):
        if path.endswith(".lib"):
            continue
        if path not in files:
            extra.append(path)
    for path, meta in sorted(files.items()):
        full = REPO / path
        if not full.is_file():
            missing.append(path)
            continue
        actual = hashes[path]["sha256"]
        if actual != meta["sha256"]:
            mismatches.append({
                "path": path,
                "manifest": meta["sha256"],
                "tree": actual,
                "manifest_size": meta.get("size"),
                "tree_size": hashes[path].get("size"),
            })

    lines = [
        "manifest-compare-after",
        f"head_now={run(['git', 'rev-parse', 'HEAD']).strip()}",
        f"manifest_head={manifest['head']}",
        f"mismatch_count={len(mismatches)}",
        f"missing_count={len(missing)}",
        f"extra_count={len(extra)}",
    ]
    if mismatches:
        lines.append("MISMATCHES")
        for row in mismatches:
            lines.append(f"  {row['path']} manifest={row['manifest']} tree={row['tree']}")
    if missing:
        lines.append("MISSING")
        for p in missing:
            lines.append(f"  {p}")
    if extra:
        lines.append("EXTRA")
        for p in extra:
            lines.append(f"  {p}")
    if not mismatches and not missing and not extra:
        lines.append("all manifest files match the working tree; no extra non-lib dirty files")
    text = "\n".join(lines) + "\n"
    (SCRATCH / "manifest-compare-after.txt").write_text(text, encoding="utf-8")
    print(text)
    return 0 if not mismatches and not missing and not extra else 3


if __name__ == "__main__":
    raise SystemExit(main())
