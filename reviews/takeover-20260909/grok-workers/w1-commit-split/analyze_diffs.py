#!/usr/bin/env python3
"""Per-file diff keywords for commit grouping."""
from __future__ import annotations

import json
import re
import subprocess
from pathlib import Path

REPO = Path(r"D:\Projects\takeover-fixes")
OUT = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w1-commit-split\scratch\diff-keywords.json")

KEYS = [
    "codec 18", "Codec18", "kCodec18", "PlayerObs", "PlayerBinding", "complete-input",
    "CompleteInput", "resync", "Resync", "envelope", "Acked", "acknowledged",
    "command sequence", "CommandSeq", "drop map", "DropMap", "ownership",
    "exact-target", "ExactTarget", "prime", "local player", "alias",
    "moderation", "F6", "codec 17", "Codec17", "seat snapshot", "roster",
    "AdoptPersisted", "FaithfulClone", "CurrentExit", "pending exit",
    "module", "SaveGame", "shared clock", "SharedClock", "journal",
    "A7", "inventory role", "controlled actor", "Activity1",
    "NetLocalRestore", "GUICheckpoint", "selected module",
]


def run(args: list[str]) -> str:
    return subprocess.run(args, cwd=REPO, capture_output=True, text=True).stdout


def main() -> None:
    status = run(["git", "status", "--porcelain"])
    items = []
    for line in status.splitlines():
        code = line[:2]
        path = line[3:].strip().strip('"').replace("\\", "/")
        if path.endswith(".lib"):
            continue
        if code.startswith("?"):
            text = (REPO / path).read_text(encoding="utf-8", errors="replace")
            stat = "untracked"
        else:
            text = run(["git", "diff", "-U2", "--", path])
            stat = run(["git", "diff", "--numstat", "--", path]).strip()
        hits = {}
        lower = text.lower()
        for k in KEYS:
            c = lower.count(k.lower())
            if c:
                hits[k] = c
        funcs = sorted(set(re.findall(r"\b(?:void|bool|int|float|std::\w+|static)\s+(\w+)\s*\(", text)))
        added_funcs = sorted(set(re.findall(r"^\+\s*(?:[\w:<>,\s*&]+)\s+(\w+)\s*\(", text, re.M)))
        items.append({
            "path": path,
            "status": code,
            "stat": stat,
            "hits": hits,
            "added_funcs": added_funcs[:30],
            "funcs": funcs[:20],
            "bytes": len(text),
        })
    OUT.write_text(json.dumps(items, indent=2) + "\n", encoding="utf-8")
    for it in items:
        print(f"{it['path']}  {it['stat']}")
        if it["hits"]:
            print("  hits", it["hits"])
        if it["added_funcs"]:
            print("  +fn", it["added_funcs"][:12])


if __name__ == "__main__":
    main()
