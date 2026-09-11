"""Unified diff pre → fixed for the two edited files."""
import difflib
from pathlib import Path

SCRATCH = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w4-breadth-harness")
pairs = [
    (
        SCRATCH / "pre" / "run_breadth.py",
        Path(r"D:\Projects\reviews\takeover-20260909\run_breadth.py"),
        "pre/run_breadth.py",
        "run_breadth.py",
    ),
    (
        SCRATCH / "pre" / "compat_review_extra.lua",
        Path(r"D:\Projects\reviews\claude-review-2026-09-08\lanes\compat-review\fixtures\compat_review_extra.lua"),
        "pre/compat_review_extra.lua",
        "compat_review_extra.lua",
    ),
]
chunks = []
for old_path, new_path, old_name, new_name in pairs:
    old = old_path.read_text(encoding="utf-8").splitlines(keepends=True)
    new = new_path.read_text(encoding="utf-8").splitlines(keepends=True)
    chunks.append("".join(difflib.unified_diff(old, new, fromfile=old_name, tofile=new_name)))
(SCRATCH / "harness.diff").write_text("".join(chunks), encoding="utf-8")
print("wrote", SCRATCH / "harness.diff", "bytes", (SCRATCH / "harness.diff").stat().st_size)
