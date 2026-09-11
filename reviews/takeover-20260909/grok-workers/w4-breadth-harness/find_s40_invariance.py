"""Reparse-safe search for Source40 invariance input_delay. Read-only."""
from __future__ import annotations

import json
import os
import stat
from pathlib import Path

SKIP = {"Data", "external", "modules", "runtime"}
LIMIT = 20 * 1024 * 1024
ROOT = Path(r"D:\mx\s40lanes")
OUT = Path(__file__).with_name("s40-invariance-search.json")


def is_reparse(path: Path) -> bool:
    try:
        return bool(path.lstat().st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT)
    except OSError:
        return False


hits = []
if ROOT.exists():
    for dirpath, dirnames, filenames in os.walk(ROOT, followlinks=False):
        current = Path(dirpath)
        if is_reparse(current) and current != ROOT:
            dirnames[:] = []
            continue
        dirnames[:] = [
            name for name in dirnames
            if name not in SKIP and not is_reparse(current / name)
        ]
        lowered = current.name.lower()
        if "invariance" not in lowered and "invar" not in lowered:
            continue
        for name in filenames:
            if name.lower() not in {"result.json", "provenance.json"}:
                continue
            path = current / name
            if is_reparse(path):
                continue
            try:
                if path.stat().st_size > LIMIT:
                    continue
                data = json.loads(path.read_text(encoding="utf-8-sig"))
            except (OSError, json.JSONDecodeError):
                continue
            hits.append({
                "path": str(path),
                "input_delay": data.get("input_delay"),
                "keys": sorted(data)[:20],
            })

payload = {"root": str(ROOT), "hits": hits, "count": len(hits)}
OUT.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
print(json.dumps(payload, indent=2))
