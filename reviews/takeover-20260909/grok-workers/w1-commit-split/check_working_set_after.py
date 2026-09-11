#!/usr/bin/env python3
import json
from pathlib import Path

SCRATCH = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w1-commit-split\scratch")
plan = json.loads((SCRATCH / "plan.json").read_text(encoding="utf-8"))
after = json.loads((SCRATCH / "positive20-verify-after.json").read_text(encoding="utf-8"))
before = json.loads((SCRATCH / "positive20-verify.json").read_text(encoding="utf-8"))

files = []
for c in plan["commits"]:
    files.extend(c["files"])

rows = {}
bad = []
for path in files:
    info = after["files"].get(path)
    if info is None:
        rows[path] = "not_in_manifest"
        bad.append(path)
    else:
        rows[path] = info["status"]
        if info["status"] != "match":
            bad.append(path)

# 91 set identity
same_mismatch = before["mismatch_paths"] == after["mismatch_paths"]
same_missing = before["missing_paths"] == after["missing_paths"]
out = {
    "working_set_statuses": rows,
    "working_set_bad": bad,
    "working_set_ok": not bad,
    "forbidden_exist": after["forbidden_exist"],
    "full_tree_same_as_pre_commit": same_mismatch and same_missing and before["matched"] == after["matched"],
    "matched": after["matched"],
    "mismatch_count": after["mismatch_count"],
}
(SCRATCH / "positive20-working-set-after.json").write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
print(json.dumps({k: out[k] for k in ("working_set_ok", "working_set_bad", "forbidden_exist", "full_tree_same_as_pre_commit", "matched", "mismatch_count")}, indent=2))
