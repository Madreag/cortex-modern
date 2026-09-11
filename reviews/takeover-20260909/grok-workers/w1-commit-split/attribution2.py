#!/usr/bin/env python3
import re
from pathlib import Path
import subprocess

REPO = Path(r"D:\Projects\takeover-fixes")
SCRATCH = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w1-commit-split\scratch")
ATTR_RE = re.compile(r"co-authored|claude|codex|grok|chatgpt|openai|anthropic|cursor|generated", re.I)
raw = subprocess.run(
    ["git", "log", "--format=%B", "60cb698146..HEAD"],
    cwd=REPO, capture_output=True, text=True,
).stdout
hits = [f"{i}:{ln}" for i, ln in enumerate(raw.splitlines(), 1) if ATTR_RE.search(ln)]
text = "attribution scan 60cb698146..HEAD (attempt 2)\npattern: co-authored|claude|codex|grok|chatgpt|openai|anthropic|cursor|generated\n"
text += ("HITS\n" + "\n".join(hits) + "\n") if hits else "no matching lines\n"
(SCRATCH / "attribution-scan-attempt2.txt").write_text(text, encoding="utf-8")
print(text)
raise SystemExit(0 if not hits else 4)
