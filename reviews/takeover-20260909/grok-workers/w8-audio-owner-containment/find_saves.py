"""Reparse-safe inventory of .ccsave and unpacked Save.ini under D:\\mx and reviews."""
from __future__ import annotations

import json
import os
import stat
from datetime import datetime, timezone
from pathlib import Path

OUT = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w8-audio-owner-containment")
ROOTS = [
    Path(r"D:\mx"),
    Path(r"D:\Projects\reviews"),
]
FILE_ATTRIBUTE_REPARSE_POINT = 0x400
SKIP_DIR_NAMES = {
    "Data",
    "external",
    "modules",
    "$Recycle.Bin",
    "System Volume Information",
}


def is_reparse(entry: os.DirEntry) -> bool:
    if entry.is_symlink():
        return True
    try:
        st = entry.stat(follow_symlinks=False)
    except OSError:
        return True
    attrs = getattr(st, "st_file_attributes", 0)
    return bool(attrs & FILE_ATTRIBUTE_REPARSE_POINT)


def walk_skip_reparse(root: Path):
    if not root.exists():
        return
    stack = [root]
    while stack:
        current = stack.pop()
        try:
            it = os.scandir(current)
        except OSError as exc:
            yield ("error", current, str(exc))
            continue
        with it:
            try:
                entries = list(it)
            except OSError as exc:
                yield ("error", current, str(exc))
                continue
        for entry in entries:
            if is_reparse(entry):
                yield ("reparse", Path(entry.path), None)
                continue
            name = entry.name
            if entry.is_dir(follow_symlinks=False):
                if name in SKIP_DIR_NAMES:
                    yield ("skipdir", Path(entry.path), None)
                    continue
                stack.append(Path(entry.path))
                continue
            yield ("file", Path(entry.path), entry)


def wanted(path: Path) -> bool:
    name = path.name
    if name.lower().endswith(".ccsave"):
        return True
    if name.lower() == "save.ini":
        return True
    return False


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    found = []
    stats = {"files_seen": 0, "reparse": 0, "skipdir": 0, "error": 0, "roots": []}
    for root in ROOTS:
        root_stat = {"root": str(root), "exists": root.exists(), "found": 0}
        if not root.exists():
            stats["roots"].append(root_stat)
            continue
        for kind, path, entry in walk_skip_reparse(root):
            if kind == "reparse":
                stats["reparse"] += 1
                continue
            if kind == "skipdir":
                stats["skipdir"] += 1
                continue
            if kind == "error":
                stats["error"] += 1
                continue
            stats["files_seen"] += 1
            if not wanted(path):
                continue
            try:
                st = entry.stat(follow_symlinks=False) if entry is not None else os.stat(path, follow_symlinks=False)
            except OSError:
                continue
            if st.st_mode & stat.S_IFREG == 0:
                continue
            mtime = datetime.fromtimestamp(st.st_mtime, tz=timezone.utc).isoformat()
            rec = {
                "path": str(path),
                "size": st.st_size,
                "mtime_utc": mtime,
                "kind": "ccsave" if path.suffix.lower() == ".ccsave" else "save.ini",
            }
            found.append(rec)
            root_stat["found"] += 1
        stats["roots"].append(root_stat)
    found.sort(key=lambda r: r["path"].lower())
    inventory = {"command": "python find_saves.py", "stats": stats, "count": len(found), "saves": found}
    out_path = OUT / "save_inventory.json"
    out_path.write_text(json.dumps(inventory, indent=2), encoding="utf-8")
    lines = [
        f"saves={len(found)} files_seen={stats['files_seen']} reparse={stats['reparse']} skipdir={stats['skipdir']} error={stats['error']}",
    ]
    for rec in found:
        lines.append(f"{rec['size']:>12}  {rec['mtime_utc']}  {rec['path']}")
    (OUT / "save_inventory.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
