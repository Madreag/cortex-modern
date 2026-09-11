"""Print INI / carrier clues for a given checkpoint identity in one save."""
from __future__ import annotations

import json
import sys
from pathlib import Path

import check_save

path = Path(sys.argv[1])
want = int(sys.argv[2])
ini, _ = check_save.load_save_ini(path)
props = check_save.extract_ini_props(ini)
hits = []
for start, b64 in props["SoundCheckpoints"]:
    try:
        parsed = check_save.parse_sound_container_identity(check_save.decode_b64_text(b64))
    except Exception:
        continue
    if parsed["identity"] != want:
        continue
    entity = parsed.get("entity") or {}
    hits.append(
        {
            "identity": want,
            "preset": entity.get("preset"),
            "copied_from": entity.get("copied_from"),
            "nearby": check_save.nearby_ini_clues(ini, start, window=4000),
        }
    )
out = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w8-audio-owner-containment") / f"clue_{want}.json"
out.write_text(json.dumps(hits, indent=2), encoding="utf-8")
print(json.dumps(hits, indent=2))
