#!/usr/bin/env python3
"""Record Mac manifest size vs Windows HEAD blob for the 21 content mismatches."""
from __future__ import annotations

import json
import subprocess
from pathlib import Path

REPO = Path(r"D:\Projects\takeover-fixes")
SCRATCH = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w1-commit-split\scratch")
MANIFEST = json.loads(
    Path(r"D:\Projects\reviews\takeover-20260909\b2-mac-positive20\positive20.manifest.json").read_text(encoding="utf-8")
)
INSPECT = json.loads((SCRATCH / "positive20-mismatch-inspect.json").read_text(encoding="utf-8"))
rows = []
for rel in INSPECT["content_mismatches"]:
    meta = MANIFEST["files"][rel]
    blob = subprocess.run(["git", "show", f"60cb698146:{rel}"], cwd=REPO, capture_output=True).stdout
    rows.append({
        "path": rel,
        "mac_size": meta["size"],
        "head_blob_size": len(blob),
        "delta": meta["size"] - len(blob),
        "mac_sha256": meta["sha256"],
    })
dest = SCRATCH / "mac-vs-head-21.json"
dest.write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
print(f"count={len(rows)} min_delta={min(r['delta'] for r in rows)} max_delta={max(r['delta'] for r in rows)}")
for r in rows:
    print(f"  {r['path']}: mac={r['mac_size']} head={r['head_blob_size']} d={r['delta']}")
