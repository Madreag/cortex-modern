#!/usr/bin/env python3
import json
from pathlib import Path

SCRATCH = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w1-commit-split\scratch")
m = json.loads((SCRATCH / "build-lines-map.json").read_text(encoding="utf-8"))
subjects = m["commit_subjects"]
by = {}
for rel, info in m["files"].items():
    for rec in info["added"]:
        by.setdefault(rec["commit_index"], {}).setdefault(rel, []).append(rec)
lines = []
for i, subj in enumerate(subjects, 1):
    if i not in by:
        continue
    lines.append(f"### {i}. {subj}")
    for rel, recs in by[i].items():
        lines.append(f"`{rel}`")
        for r in recs:
            lines.append(f"- L{r['line_no']}: `{r['text']}`")
    lines.append("")
text = "\n".join(lines)
(SCRATCH / "build-entries.md").write_text(text, encoding="utf-8")
print(text)
