from pathlib import Path
import json
import re

OUT = Path(__file__).resolve().parent
files = [
    Path(r"D:\mx\s41r2\heal_global_d0\fresh\e2e\resync_heal\host\trace.json.simdump.txt"),
    Path(r"D:\mx\s41r2\heal_global_d0\fresh\e2e\resync_heal\client\trace.json.simdump.txt"),
    Path(r"D:\mx\s41r2\heal_global_d3\fresh\e2e\resync_heal\host\trace.json.simdump.txt"),
    Path(r"D:\mx\s41r2\heal_global_d3\fresh\e2e\resync_heal\client\trace.json.simdump.txt"),
]
rows = []
for path in files:
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    for i, line in enumerate(lines, 1):
        if "uid=1048653" not in line:
            continue
        match = re.search(r" nv=\[(.*?)\]", line)
        rows.append({
            "path": str(path),
            "line": i,
            "kind": line.split()[1] if len(line.split()) > 1 else "",
            "nv": match.group(1) if match else None,
            "has_stuck": "AI_StuckForTime" in line,
            "status": re.search(r" status=(\S+)", line).group(1) if re.search(r" status=(\S+)", line) else None,
            "health": re.search(r" health=(\S+)", line).group(1) if re.search(r" health=(\S+)", line) else None,
            "aimode": re.search(r" aimode=(\S+)", line).group(1) if re.search(r" aimode=(\S+)", line) else None,
            "team": re.search(r" team=(\S+)", line).group(1) if re.search(r" team=(\S+)", line) else None,
            "mode": re.search(r" mode=(\S+)", line).group(1) if re.search(r" mode=(\S+)", line) else None,
        })
print("d0 host", float.fromhex("0x1.d4bb333333333p+11"))
print("d3 host", float.fromhex("0x1.963bd70a3d70ap+12"))
(OUT / "dummy_rows.json").write_text(json.dumps(rows, indent=2), encoding="utf-8")
print(json.dumps(rows, indent=2))
