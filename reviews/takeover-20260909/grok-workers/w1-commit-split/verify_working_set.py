#!/usr/bin/env python3
"""Verify the 78 planned files (+ no NetRecoveryJournal) against positive20.manifest.json."""
from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

REPO = Path(r"D:\Projects\takeover-fixes")
SCRATCH = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w1-commit-split\scratch")
MANIFEST = json.loads(
    Path(r"D:\Projects\reviews\takeover-20260909\b2-mac-positive20\positive20.manifest.json").read_text(encoding="utf-8")
)
PLAN = json.loads((SCRATCH / "plan.json").read_text(encoding="utf-8"))
EXPECTED_SHA = "29938cd8ba995ab728c3710d66644f4c01659bb29b4a66954bd7fa78ed14c1ad"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    out_name = sys.argv[1] if len(sys.argv) > 1 else "working-set-verify.json"
    files = []
    for c in PLAN["commits"]:
        if c["subject"] == "Add the new network and moderation sources to the build":
            files.extend(c["files"])  # still verify the three build files as FINAL
            continue
        files.extend(c["files"])
    files = sorted(set(files))

    man_sha = sha256_file(Path(r"D:\Projects\reviews\takeover-20260909\b2-mac-positive20\positive20.manifest.json"))
    rows = {}
    bad = []
    for rel in files:
        meta = MANIFEST["files"].get(rel)
        full = REPO / rel
        if meta is None:
            rows[rel] = "not_in_manifest"
            bad.append(rel)
            continue
        if not full.is_file():
            rows[rel] = "missing"
            bad.append(rel)
            continue
        actual = sha256_file(full)
        if actual == meta["sha256"]:
            rows[rel] = "match"
        else:
            rows[rel] = {"status": "mismatch", "expected": meta["sha256"], "actual": actual}
            bad.append(rel)

    journals = {
        "Source/Network/NetRecoveryJournal.h": (REPO / "Source/Network/NetRecoveryJournal.h").exists(),
        "Source/Network/NetRecoveryJournal.cpp": (REPO / "Source/Network/NetRecoveryJournal.cpp").exists(),
    }
    result = {
        "manifest_sha256": man_sha,
        "manifest_sha_ok": man_sha == EXPECTED_SHA,
        "file_count": len(files),
        "bad": bad,
        "journals_exist": journals,
        "ok": man_sha == EXPECTED_SHA and not bad and not any(journals.values()),
        "files": rows,
    }
    dest = SCRATCH / out_name
    dest.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: result[k] for k in ("ok", "manifest_sha_ok", "file_count", "bad", "journals_exist")}, indent=2))
    return 0 if result["ok"] else 3


if __name__ == "__main__":
    raise SystemExit(main())
