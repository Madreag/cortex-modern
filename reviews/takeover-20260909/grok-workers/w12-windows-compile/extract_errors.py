import re
import sys
from pathlib import Path

log = Path(sys.argv[1] if len(sys.argv) > 1 else r"D:\Projects\reviews\takeover-20260909\grok-workers\w12-windows-compile\build.log")
text = log.read_text(encoding="utf-8", errors="replace")
pat = re.compile(r"^(.*)\((\d+),(\d+)\): error (C\d+|LNK\d+): (.*)$", re.M)
rows = []
for m in pat.finditer(text):
    rows.append(
        {
            "file": m.group(1),
            "line": int(m.group(2)),
            "col": int(m.group(3)),
            "code": m.group(4),
            "msg": m.group(5).split(" [")[0].strip(),
        }
    )
print(f"count={len(rows)}")
for r in rows:
    print(f"{r['file']}:{r['line']}:{r['col']} {r['code']} {r['msg']}")
