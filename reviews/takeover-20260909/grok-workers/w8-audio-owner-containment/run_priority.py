"""Check match-like p5resync / p5snap saves first."""
from __future__ import annotations

import json
from pathlib import Path

import check_save

OUT = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w8-audio-owner-containment")
NEEDLE = ("p5resync", "p5snap")


def main() -> None:
    inventory = json.loads((OUT / "save_inventory.json").read_text(encoding="utf-8"))
    picked = [r for r in inventory["saves"] if any(n in r["path"].lower() for n in NEEDLE)]
    lines = [f"priority_saves={len(picked)}"]
    results = []
    for rec in picked:
        path = Path(rec["path"])
        try:
            result = check_save.analyze(path)
        except Exception as exc:
            result = {"path": str(path), "fatal": str(exc), "voices": [], "missing": [], "audio": None}
        results.append(result)
        audio = result.get("audio") or {}
        lines.append(
            f"{path.name} size={rec['size']} voices={len(result.get('voices') or [])} "
            f"missing={len(result.get('missing') or [])} token={audio.get('voice_count_token')} "
            f"valid={result.get('validation')} errors={result.get('parse_errors') or result.get('fatal')}"
        )
        for row in result.get("missing") or []:
            lines.append(
                f"  MISSING owner={row['owner']} path={row['path']!r} clues={row.get('carrier_clues')[:8]}"
            )
        for row in result.get("voices") or []:
            if row.get("carried_by") not in ("MISSING",):
                continue
    (OUT / "phase1_priority.json").write_text(json.dumps(results, indent=2, default=str), encoding="utf-8")
    (OUT / "phase1_priority.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
