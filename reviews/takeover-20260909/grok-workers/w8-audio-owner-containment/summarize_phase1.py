"""Compact Phase 1 tables from phase1_results.json."""
from __future__ import annotations

import json
from collections import Counter
from pathlib import Path

OUT = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w8-audio-owner-containment")
payload = json.loads((OUT / "phase1_results.json").read_text(encoding="utf-8"))
results = payload["results"]

voice_saves = [r for r in results if r.get("voices")]
missing_saves = [r for r in results if r.get("missing")]
valid = [r for r in results if (r.get("validation") or {}).get("voice_count_matches_token")]
valid_tags = [r for r in results if (r.get("validation") or {}).get("voice_count_matches_audiovoice1_tags")]
no_globals = [r for r in results if "no RuntimeGlobals property" in (r.get("parse_errors") or [])]
fatal = [r for r in results if r.get("fatal")]
carrier = Counter()
owner_zero_playing = 0
nonzero_voices = 0
for r in results:
    for v in r.get("voices") or []:
        carrier[v.get("carried_by")] += 1
        if v.get("owner") == 0 and v.get("playing"):
            owner_zero_playing += 1
        if v.get("owner"):
            nonzero_voices += 1

# representative match saves
needles = ("p5resync", "p5snap", "load_seed", "load_candidate")
rep = []
seen = set()
for r in results:
    p = r["path"]
    key = Path(p).name + "|" + str((r.get("audio") or {}).get("voice_count_token"))
    if any(n in p.lower() for n in needles) and key not in seen and r.get("audio"):
        seen.add(key)
        if len(rep) < 25:
            rep.append(r)

lines = [
    f"save_count={payload['save_count']} unique={payload['unique_analyzed']}",
    f"with_voices={payload['saves_with_voices']} with_missing={payload['saves_with_missing']}",
    f"valid_token={len(valid)} valid_audiovoice1={len(valid_tags)} no_RuntimeGlobals={len(no_globals)} fatal={len(fatal)}",
    f"voice_rows_nonzero_owner={nonzero_voices} owner0_playing={owner_zero_playing}",
    f"carried_by={dict(carrier)}",
    "",
    "representative parsed saves:",
]
for r in voice_saves[:5] + [x for x in voice_saves if "p5resync_56264" in x["path"]][:1]:
    audio = r.get("audio") or {}
    lines.append(
        f"  voices={len(r['voices'])} missing={len(r.get('missing') or [])} "
        f"token={audio.get('voice_count_token')} tags={audio.get('audio_voice1_tag_count')} "
        f"{r['path']}"
    )

# 9310 as voice owner?
as_owner = []
as_ini = 0
for r in results:
    ids = set(r.get("ini_identities") or [])
    if 9310 in ids:
        as_ini += 1
    for v in r.get("voices") or []:
        if v.get("owner") == 9310:
            as_owner.append((r["path"], v))

lines.append(f"saves_with_9310_in_ini={as_ini} saves_with_9310_as_voice_owner={len(as_owner)}")
for path, v in as_owner[:10]:
    lines.append(f"  9310 voice {v} in {path}")

(OUT / "phase1_compact.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
print("\n".join(lines))
