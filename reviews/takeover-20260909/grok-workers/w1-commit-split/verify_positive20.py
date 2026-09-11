#!/usr/bin/env python3
"""Verify working tree against b2-mac-positive20/positive20.manifest.json."""
from __future__ import annotations

import hashlib
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

REPO = Path(r"D:\Projects\takeover-fixes")
MANIFEST = Path(r"D:\Projects\reviews\takeover-20260909\b2-mac-positive20\positive20.manifest.json")
EXPECTED_MANIFEST_SHA = "29938cd8ba995ab728c3710d66644f4c01659bb29b4a66954bd7fa78ed14c1ad"
SCRATCH = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w1-commit-split\scratch")
FORBIDDEN = [
    "Source/Network/NetRecoveryJournal.h",
    "Source/Network/NetRecoveryJournal.cpp",
]


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    out_name = sys.argv[1] if len(sys.argv) > 1 else "positive20-verify.json"
    out_path = SCRATCH / out_name

    manifest_sha = sha256_file(MANIFEST)
    data = json.loads(MANIFEST.read_text(encoding="utf-8"))
    files = data["files"]

    per_file = {}
    mismatch = []
    missing = []
    extra_type = []
    matched = 0

    for rel, meta in files.items():
        full = REPO / rel
        expected = meta["sha256"] if isinstance(meta, dict) else meta
        if not full.exists():
            per_file[rel] = {"status": "missing", "expected": expected}
            missing.append(rel)
            continue
        if not full.is_file():
            per_file[rel] = {"status": "not_file", "expected": expected}
            extra_type.append(rel)
            continue
        actual = sha256_file(full)
        if actual == expected:
            per_file[rel] = {"status": "match"}
            matched += 1
        else:
            per_file[rel] = {
                "status": "mismatch",
                "expected": expected,
                "actual": actual,
                "expected_size": meta.get("size") if isinstance(meta, dict) else None,
                "actual_size": full.stat().st_size,
            }
            mismatch.append(rel)

    forbidden_status = {}
    forbidden_exist = []
    for rel in FORBIDDEN:
        exists = (REPO / rel).exists()
        forbidden_status[rel] = {"exists": exists}
        if exists:
            forbidden_exist.append(rel)

    result = {
        "captured_utc": datetime.now(timezone.utc).isoformat(),
        "manifest_path": str(MANIFEST),
        "manifest_sha256": manifest_sha,
        "manifest_sha256_expected": EXPECTED_MANIFEST_SHA,
        "manifest_sha_ok": manifest_sha == EXPECTED_MANIFEST_SHA,
        "manifest_file_count": len(files),
        "matched": matched,
        "mismatch_count": len(mismatch),
        "missing_count": len(missing),
        "not_file_count": len(extra_type),
        "mismatch_paths": mismatch,
        "missing_paths": missing,
        "not_file_paths": extra_type,
        "forbidden_status": forbidden_status,
        "forbidden_exist": forbidden_exist,
        "ok": (
            manifest_sha == EXPECTED_MANIFEST_SHA
            and not mismatch
            and not missing
            and not extra_type
            and not forbidden_exist
        ),
        "files": per_file,
    }
    out_path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    summary = {k: result[k] for k in (
        "ok", "manifest_sha_ok", "manifest_sha256", "matched",
        "mismatch_count", "missing_count", "not_file_count",
        "forbidden_exist", "mismatch_paths", "missing_paths",
    )}
    print(json.dumps(summary, indent=2))
    print(f"wrote {out_path}")
    return 0 if result["ok"] else 3


if __name__ == "__main__":
    raise SystemExit(main())
