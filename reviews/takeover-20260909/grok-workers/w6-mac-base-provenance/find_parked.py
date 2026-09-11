"""Walk D:\\Projects for parked patch names. Skip junctions and excluded dirs."""
from __future__ import annotations

import json
import os
import sys
from pathlib import Path

ROOT = Path(r"D:\Projects")
OUT = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w6-mac-base-provenance")
SKIP_NAMES = {
    "runtime",
    "data",
    "external",
    ".git",
    "node_modules",
    "__pycache__",
}
SKIP_PREFIXES = (
    Path(r"D:\mx"),
)
WANT_FILE = "primitive_checkpoint_pending.patch"
WANT_DIR = "parked-20260908-tools-contracts"

hits_file: list[str] = []
hits_dir: list[str] = []
errors: list[str] = []
scanned = 0


def is_reparse(path: Path) -> bool:
    try:
        return bool(path.stat().st_reparse_point)
    except AttributeError:
        pass
    try:
        attrs = os.lstat(path).st_file_attributes  # type: ignore[attr-defined]
        return bool(attrs & 0x400)  # FILE_ATTRIBUTE_REPARSE_POINT
    except Exception:
        return False


def should_skip_dir(path: Path) -> bool:
    name = path.name.lower()
    if name in SKIP_NAMES:
        return True
    try:
        resolved = path.resolve()
    except Exception:
        resolved = path
    for prefix in SKIP_PREFIXES:
        try:
            if resolved == prefix or prefix in resolved.parents or resolved in prefix.parents:
                if resolved == prefix or prefix in resolved.parents:
                    return True
        except Exception:
            pass
    if is_reparse(path):
        return True
    return False


def walk(root: Path) -> None:
    global scanned
    stack = [root]
    while stack:
        current = stack.pop()
        scanned += 1
        if scanned % 5000 == 0:
            print(f"scanned_dirs={scanned} file_hits={len(hits_file)} dir_hits={len(hits_dir)}", flush=True)
        try:
            with os.scandir(current) as it:
                for entry in it:
                    try:
                        name = entry.name
                        path = Path(entry.path)
                        if entry.is_symlink() or is_reparse(path):
                            continue
                        if entry.is_dir(follow_symlinks=False):
                            if name.lower() in SKIP_NAMES:
                                continue
                            if name == WANT_DIR:
                                hits_dir.append(str(path))
                            stack.append(path)
                        elif entry.is_file(follow_symlinks=False):
                            if name == WANT_FILE:
                                hits_file.append(str(path))
                    except OSError as e:
                        errors.append(f"{entry.path}: {e}")
        except OSError as e:
            errors.append(f"{current}: {e}")


if __name__ == "__main__":
    walk(ROOT)
    result = {
        "scanned_dirs": scanned,
        "file_hits": hits_file,
        "dir_hits": hits_dir,
        "error_count": len(errors),
        "errors_head": errors[:40],
    }
    out = OUT / "parked-search.json"
    out.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2))
    sys.exit(0)
