import json
from pathlib import Path
OUT = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w8-audio-owner-containment")
payload = json.loads((OUT / "phase1_results.json").read_text(encoding="utf-8"))
lines = []
for r in payload["results"]:
    if 9310 in (r.get("ini_identities") or []):
        lines.append(f"{r['path']} voices={len(r.get('voices') or [])}")
    if r.get("fatal"):
        lines.append(f"FATAL {r['fatal']} {r['path']}")
(OUT / "phase1_9310_and_fatal.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
print("\n".join(lines[:40]))
print("total_lines", len(lines))
