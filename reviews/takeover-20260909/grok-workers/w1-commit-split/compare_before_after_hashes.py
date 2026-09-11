#!/usr/bin/env python3
import json
from pathlib import Path

SCRATCH = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w1-commit-split\scratch")
before = json.loads((SCRATCH / "hashes-before.json").read_text(encoding="utf-8"))
after = json.loads((SCRATCH / "hashes-after.json").read_text(encoding="utf-8"))
only_before = sorted(set(before) - set(after))
only_after = sorted(set(after) - set(before))
changed = []
for path in sorted(set(before) & set(after)):
    b, a = before[path], after[path]
    if b.get("sha256") != a.get("sha256") or b.get("size") != a.get("size"):
        changed.append({
            "path": path,
            "before": b.get("sha256"),
            "after": a.get("sha256"),
            "before_size": b.get("size"),
            "after_size": a.get("size"),
        })
out = {
    "only_before": only_before,
    "only_after": only_after,
    "changed": changed,
    "ok": not only_before and not only_after and not changed,
}
(SCRATCH / "hashes-before-vs-after.json").write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
print(json.dumps(out, indent=2))
