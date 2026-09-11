#!/usr/bin/env python3
import subprocess
from pathlib import Path

REPO = Path(r"D:\Projects\takeover-fixes")
BASE = "60cb6981462d30400d372ca778d440c82a5bcbe3"
files = ["RTEA.vcxproj", "RTEA.vcxproj.filters", "Source/Network/meson.build"]
for rel in files:
    wt = (REPO / rel).read_bytes()
    blob = subprocess.run(["git", "show", f"{BASE}:{rel}"], cwd=REPO, capture_output=True).stdout
    print(rel)
    print(f"  wt crlf={wt.count(b'\r\n')} lf={wt.count(b'\n')} ends={wt[-20]!r} len={len(wt)}")
    print(f"  base crlf={blob.count(b'\r\n')} lf={blob.count(b'\n')} ends={blob[-20]!r} len={len(blob)}")
    print(f"  wt_lf==base {wt.replace(b'\r\n', b'\n') == blob.replace(b'\r\n', b'\n')}")
