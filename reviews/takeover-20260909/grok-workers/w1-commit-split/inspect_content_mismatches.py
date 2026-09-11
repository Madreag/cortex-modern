#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import json
import subprocess
from pathlib import Path

REPO = Path(r"D:\Projects\takeover-fixes")
SCRATCH = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w1-commit-split\scratch")
INSPECT = json.loads((SCRATCH / "positive20-mismatch-inspect.json").read_text(encoding="utf-8"))
MANIFEST = json.loads(
    Path(r"D:\Projects\reviews\takeover-20260909\b2-mac-positive20\positive20.manifest.json").read_text(encoding="utf-8")
)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def first_diff(a: bytes, b: bytes) -> dict:
    n = min(len(a), len(b))
    i = 0
    while i < n and a[i] == b[i]:
        i += 1
    return {
        "offset": i,
        "a_len": len(a),
        "b_len": len(b),
        "a_around": a[max(0, i - 20): i + 20].hex(),
        "b_around": b[max(0, i - 20): i + 20].hex(),
    }


def main() -> None:
    rows = []
    for rel in INSPECT["content_mismatches"]:
        tree = (REPO / rel).read_bytes()
        blob = subprocess.run(["git", "show", f"HEAD:{rel}"], cwd=REPO, capture_output=True).stdout
        expected = MANIFEST["files"][rel]["sha256"]
        tree_lf = tree.replace(b"\r\n", b"\n")
        blob_lf = blob.replace(b"\r\n", b"\n")
        rows.append({
            "path": rel,
            "tree_sha": sha256_bytes(tree),
            "tree_lf_sha": sha256_bytes(tree_lf),
            "head_blob_sha": sha256_bytes(blob),
            "head_blob_lf_sha": sha256_bytes(blob_lf),
            "manifest_sha": expected,
            "tree_equals_head": tree == blob,
            "tree_lf_equals_head_lf": tree_lf == blob_lf,
            "tree_lf_equals_manifest": sha256_bytes(tree_lf) == expected,
            "head_lf_equals_manifest": sha256_bytes(blob_lf) == expected,
            "tree_vs_head": first_diff(tree_lf, blob_lf),
        })
    dest = SCRATCH / "positive20-content-mismatch-detail.json"
    dest.write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
    for r in rows:
        print(f"{r['path']}")
        print(f"  tree==head {r['tree_equals_head']} lf== {r['tree_lf_equals_head_lf']} head_lf==manifest {r['head_lf_equals_manifest']}")
        print(f"  tree_vs_head offset={r['tree_vs_head']['offset']} alen={r['tree_vs_head']['a_len']} blen={r['tree_vs_head']['b_len']}")


if __name__ == "__main__":
    main()
