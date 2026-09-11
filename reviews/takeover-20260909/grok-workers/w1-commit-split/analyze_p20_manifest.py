#!/usr/bin/env python3
from collections import Counter
from pathlib import Path
import json

p = Path(r"D:\Projects\reviews\takeover-20260909\b2-mac-positive20\positive20.manifest.json")
data = json.loads(p.read_text(encoding="utf-8"))
files = data["files"]
print("keys", list(data.keys()))
print("file_count", len(files))
exts = Counter()
for path in files:
    if "." in path.split("/")[-1]:
        ext = path.rsplit(".", 1)[-1].lower()
    else:
        ext = "(none)"
    exts[ext] += 1
print("top exts:")
for ext, n in exts.most_common(30):
    print(f"  {ext}: {n}")
interesting = [p for p in files if p.endswith((".lib", ".exe", ".dll", ".o", ".obj", ".a", ".so", ".dylib"))]
print("binaries", len(interesting))
for path in interesting[:50]:
    print(" ", path)
# network/source subset
src = [p for p in files if p.startswith("Source/") or p.startswith("tools/") or p.startswith("Data/") or p.endswith((".vcxproj", ".filters", ".sln"))]
print("src-ish", len(src))
