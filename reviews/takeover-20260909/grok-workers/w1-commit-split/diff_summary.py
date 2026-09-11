#!/usr/bin/env python3
"""Summarize each dirty file's diff against HEAD for commit planning."""
from __future__ import annotations

import json
import subprocess
from pathlib import Path

REPO = Path(r"D:\Projects\takeover-fixes")
OUT = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w1-commit-split\scratch\diff-summaries.json")


def run(args: list[str]) -> str:
    r = subprocess.run(args, cwd=REPO, capture_output=True, text=True)
    return r.stdout


def main() -> None:
    status = run(["git", "status", "--porcelain"])
    entries = []
    for line in status.splitlines():
        code = line[:2]
        path = line[3:].strip().strip('"').replace("\\", "/")
        if path.endswith(".lib"):
            continue
        item = {"path": path, "status": code, "stat": "", "names": []}
        if code.startswith("?"):
            item["stat"] = "untracked"
            text = (REPO / path).read_text(encoding="utf-8", errors="replace")
        else:
            item["stat"] = run(["git", "diff", "--stat", "--", path]).strip()
            text = run(["git", "diff", "-U0", "--", path])
        # crude symbol harvest
        names = []
        for ln in text.splitlines():
            s = ln.lstrip("+").lstrip("-").strip()
            if s.startswith(("class ", "struct ", "void ", "bool ", "int ", "static ", "const ", "std::", "namespace ")):
                names.append(s[:160])
            elif "codec" in s.lower() or "resync" in s.lower() or "codec 18" in s.lower() or "PlayerObs" in s or "F6" in s or "moderation" in s.lower() or "AdoptPersisted" in s or "CurrentExit" in s or "FaithfulClone" in s or "module" in s.lower() and "save" in s.lower():
                if len(s) < 200:
                    names.append(s)
        item["names"] = names[:40]
        item["added_hint_count"] = text.count("\n+")
        entries.append(item)
    OUT.write_text(json.dumps(entries, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {len(entries)} entries to {OUT}")


if __name__ == "__main__":
    main()
