#!/usr/bin/env python3
"""Classify positive20 manifest mismatches without waiving them."""
from __future__ import annotations

import hashlib
import json
import subprocess
from pathlib import Path

REPO = Path(r"D:\Projects\takeover-fixes")
SCRATCH = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w1-commit-split\scratch")
VERIFY = json.loads((SCRATCH / "positive20-verify.json").read_text(encoding="utf-8"))
MANIFEST = json.loads(
    Path(r"D:\Projects\reviews\takeover-20260909\b2-mac-positive20\positive20.manifest.json").read_text(encoding="utf-8")
)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> None:
    status = subprocess.run(["git", "status", "--porcelain"], cwd=REPO, capture_output=True, text=True)
    dirty = set()
    for line in status.stdout.splitlines():
        path = line[3:].strip().strip('"').replace("\\", "/")
        dirty.add(path)

    rows = []
    dirty_mismatches = []
    crlf_only = []
    other = []
    for rel in VERIFY["mismatch_paths"]:
        full = REPO / rel
        data = full.read_bytes()
        expected = MANIFEST["files"][rel]["sha256"]
        lf = data.replace(b"\r\n", b"\n")
        crlf = lf.replace(b"\n", b"\r\n")
        kind = "content"
        if sha256_bytes(lf) == expected:
            kind = "crlf_tree_lf_manifest"
        elif sha256_bytes(crlf) == expected:
            kind = "lf_tree_crlf_manifest"
        row = {
            "path": rel,
            "kind": kind,
            "tree_size": len(data),
            "manifest_size": MANIFEST["files"][rel].get("size"),
            "in_git_status": rel in dirty,
            "cr_count": data.count(b"\r"),
            "lf_count": data.count(b"\n"),
        }
        rows.append(row)
        if rel in dirty:
            dirty_mismatches.append(rel)
        if kind.startswith("crlf") or kind.startswith("lf"):
            crlf_only.append(rel)
        else:
            other.append(rel)

    # dirty files vs verify status
    dirty_verify = {}
    for rel in sorted(dirty):
        if rel.endswith(".lib"):
            dirty_verify[rel] = "lib_excluded_from_pass_requirement_but_listed_if_in_manifest"
            continue
        info = VERIFY["files"].get(rel)
        if info is None:
            dirty_verify[rel] = "not_in_manifest"
        else:
            dirty_verify[rel] = info["status"]

    out = {
        "mismatch_total": len(VERIFY["mismatch_paths"]),
        "missing": VERIFY["missing_paths"],
        "dirty_mismatches": dirty_mismatches,
        "crlf_only_count": len(crlf_only),
        "content_mismatch_count": len(other),
        "content_mismatches": other,
        "dirty_file_verify_status": dirty_verify,
        "manifest_excluded_extensions": MANIFEST.get("excluded_extensions"),
        "manifest_head": MANIFEST.get("head"),
        "manifest_base_head": MANIFEST.get("base_head"),
        "manifest_status": MANIFEST.get("status"),
        "rows": rows,
    }
    dest = SCRATCH / "positive20-mismatch-inspect.json"
    dest.write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: out[k] for k in (
        "mismatch_total", "missing", "dirty_mismatches", "crlf_only_count",
        "content_mismatch_count", "content_mismatches",
    )}, indent=2))
    print("dirty verify:")
    for k, v in dirty_verify.items():
        print(f"  {v}: {k}")


if __name__ == "__main__":
    main()
