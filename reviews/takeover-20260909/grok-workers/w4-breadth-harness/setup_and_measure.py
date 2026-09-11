"""Create scratch, copy pre-fix files, measure pin vs current tree. No engine launch."""
from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

SCRATCH = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w4-breadth-harness")
PRE = SCRATCH / "pre"
SRC_BREADTH = Path(r"D:\Projects\reviews\takeover-20260909\run_breadth.py")
SRC_FIXTURE = Path(
    r"D:\Projects\reviews\claude-review-2026-09-08\lanes\compat-review\fixtures\compat_review_extra.lua"
)
REPO = Path(r"D:\Projects\p4b-interp-validation")
MANIFEST = Path(
    r"D:\Projects\reviews\recovery-2026-09-07\contract-audit\grouped-build-bb3cf264\build.json"
)
EXPORT = Path(
    r"D:\Projects\reviews\recovery-2026-09-07\contract-audit\mac-peer-20260907\combined-source-41.manifest.json"
)
PINNED_HEAD = "c8f8188ae0b4f4524f83ee5cdaebf4ef0ec3e49e"
PINNED_EXE = "bb3cf264b4e7304f45486440998e4b63e37d5d46d0d119caa04f76c2fc378ffb"
TOOLS = (
    "tools/compare_snapshots.py",
    "tools/test_snapshot_inventory_roles.py",
)


def digest(path: Path) -> str:
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def git(*args: str) -> str:
    return subprocess.check_output(["git", "-C", str(REPO), *args], text=True).strip()


def main() -> int:
    PRE.mkdir(parents=True, exist_ok=True)
    shutil.copy2(SRC_BREADTH, PRE / "run_breadth.py")
    shutil.copy2(SRC_FIXTURE, PRE / "compat_review_extra.lua")
    build = json.loads(MANIFEST.read_text(encoding="utf-8-sig"))
    export = json.loads(EXPORT.read_text(encoding="utf-8-sig"))
    files = export["files"]
    head = git("rev-parse", "HEAD")
    show = git("log", "-1", "--format=%H%n%s%n%an%n%ae")
    stat = git("show", "--stat", "--format=", head)
    exe = REPO / "Cortex Command.exe"
    exe_sha = digest(exe) if exe.is_file() else None
    tools = {}
    for name in TOOLS:
        pinned = files.get(name, {}).get("sha256")
        path = REPO / name
        current = digest(path) if path.is_file() else None
        tools[name] = {
            "in_export": name in files,
            "pinned_sha256": pinned,
            "working_sha256": current,
            "match": pinned == current if pinned and current else False,
        }
    source_names = [n for n in files if n.startswith("Source/")]
    mismatches = []
    for name, rec in files.items():
        path = REPO / name
        if not path.is_file() or digest(path) != rec["sha256"]:
            mismatches.append(name)
            if len(mismatches) >= 40:
                break
    out = {
        "copied": {
            "run_breadth.py": str(PRE / "run_breadth.py"),
            "run_breadth_sha256": digest(PRE / "run_breadth.py"),
            "fixture": str(PRE / "compat_review_extra.lua"),
            "fixture_sha256": digest(PRE / "compat_review_extra.lua"),
        },
        "build_json": {
            "path": str(MANIFEST),
            "head": build.get("head"),
            "exe_sha256": build.get("exe_sha256"),
            "has_inputs_key": "inputs" in build,
            "artifact_count": len(build.get("artifacts") or {}),
        },
        "export": {
            "path": str(EXPORT),
            "head": export.get("head"),
            "file_count": len(files),
            "source_file_count": len(source_names),
            "tools_compare_in_export": "tools/compare_snapshots.py" in files,
            "tools_inventory_in_export": "tools/test_snapshot_inventory_roles.py" in files,
        },
        "repo": {
            "head": head,
            "log": show,
            "stat": stat,
            "exe_present": exe.is_file(),
            "exe_sha256": exe_sha,
            "exe_matches_pin": exe_sha == PINNED_EXE,
            "head_matches_pin": head == PINNED_HEAD,
        },
        "tools": tools,
        "first_input_mismatches": mismatches,
        "pin_verify_would_fail": head != PINNED_HEAD or bool(mismatches) or exe_sha != PINNED_EXE,
    }
    (SCRATCH / "pin-measure.json").write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: out[k] for k in ("repo", "pin_verify_would_fail", "tools", "first_input_mismatches")}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
