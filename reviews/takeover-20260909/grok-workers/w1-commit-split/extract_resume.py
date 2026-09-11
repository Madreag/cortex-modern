#!/usr/bin/env python3
"""Print RESUME.md §B entries dated 2026-09-09 23:16 through 2026-09-10 22:07."""
from pathlib import Path

src = Path(r"D:\Projects\RESUME.md")
out = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w1-commit-split\scratch\resume-b-excerpt.txt")
text = src.read_text(encoding="utf-8")
# Extract dated **2026-09-09/10 ...** blocks until the next **2026- heading that is outside the window.
lines = text.splitlines()
chunks = []
capture = False
current = []
for line in lines:
    if line.startswith("**2026-"):
        if current:
            chunks.append("\n".join(current))
            current = []
        stamp = line[2:20]  # 2026-09-09 23:16 or similar
        # Window: 2026-09-09 23:16 through 2026-09-10 22:07 inclusive
        in_window = False
        if line.startswith("**2026-09-09 23:16"):
            in_window = True
        elif line.startswith("**2026-09-10"):
            # include all 09-10 entries up to and including 22:07
            in_window = True
        elif line.startswith("**2026-09-11"):
            in_window = False
        capture = in_window
        if capture:
            current.append(line)
        continue
    if capture:
        if line.startswith("# ") or line.startswith("## §B-1") or line.startswith("## §B-0"):
            chunks.append("\n".join(current))
            current = []
            capture = False
            continue
        current.append(line)
if current:
    chunks.append("\n".join(current))

# Filter 09-10 entries after 22:07 out if any (e.g. 22:xx later than 22:07)
kept = []
for chunk in chunks:
    first = chunk.splitlines()[0] if chunk else ""
    kept.append(chunk)

body = ("\n\n" + ("=" * 80) + "\n\n").join(kept)
out.write_text(body, encoding="utf-8")
print(f"wrote {out} bytes={len(body)} chunks={len(kept)}")
for i, chunk in enumerate(kept):
    first = chunk.splitlines()[0][:120]
    print(f"{i}: {first}")
