"""Run check_save.py over every inventoried save. Writes phase1_results.json."""
from __future__ import annotations

import hashlib
import json
import traceback
from collections import defaultdict
from pathlib import Path

import check_save

OUT = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w8-audio-owner-containment")


def file_sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def analyze_one(path: Path) -> dict:
    try:
        return check_save.analyze(path)
    except Exception as exc:
        return {
            "path": str(path),
            "fatal": str(exc),
            "traceback": traceback.format_exc(),
            "voices": [],
            "missing": [],
            "audio": None,
        }


def main() -> None:
    inventory = json.loads((OUT / "save_inventory.json").read_text(encoding="utf-8"))
    by_size = defaultdict(list)
    for rec in inventory["saves"]:
        by_size[rec["size"]].append(rec)
    digest_of = {}
    unique_paths = {}
    for size, recs in by_size.items():
        if len(recs) == 1:
            digest = f"size:{size}:unique"
            digest_of[recs[0]["path"]] = digest
            unique_paths[digest] = recs[0]["path"]
            continue
        for rec in recs:
            path = Path(rec["path"])
            try:
                digest = file_sha256(path)
            except OSError as exc:
                digest = f"unreadable:{exc}"
            digest_of[rec["path"]] = digest
            unique_paths.setdefault(digest, rec["path"])
    analyzed = {}
    for digest, path in unique_paths.items():
        analyzed[digest] = analyze_one(Path(path))
        analyzed[digest]["content_sha256"] = digest
        analyzed[digest]["analyzed_path"] = path
    results = []
    summary_lines = []
    for rec in inventory["saves"]:
        digest = digest_of[rec["path"]]
        result = dict(analyzed[digest])
        result["path"] = rec["path"]
        result["duplicate_of"] = analyzed[digest]["analyzed_path"] if analyzed[digest]["analyzed_path"] != rec["path"] else None
        results.append(result)
        audio = result.get("audio") or {}
        nvoices = len(result.get("voices") or [])
        nmiss = len(result.get("missing") or [])
        summary_lines.append(
            f"{rec['path']} size={rec['size']} voices={nvoices} missing={nmiss} "
            f"audio={audio.get('version')} token={audio.get('voice_count_token')} "
            f"dup={result['duplicate_of']} errors={result.get('parse_errors') or result.get('fatal')}"
        )
        for row in result.get("missing") or []:
            summary_lines.append(
                f"  MISSING owner={row['owner']} path={row['path']!r} clues={row.get('carrier_clues')}"
            )
    payload = {
        "command": "python run_phase1.py",
        "inventory": str(OUT / "save_inventory.json"),
        "save_count": len(results),
        "unique_analyzed": len(analyzed),
        "saves_with_voices": sum(1 for r in results if r.get("voices")),
        "saves_with_missing": sum(1 for r in results if r.get("missing")),
        "results": results,
    }
    (OUT / "phase1_results.json").write_text(json.dumps(payload, indent=2, default=str), encoding="utf-8")
    (OUT / "phase1_summary.txt").write_text("\n".join(summary_lines) + "\n", encoding="utf-8")
    print("\n".join(summary_lines))
    print(
        f"saves={payload['save_count']} unique={payload['unique_analyzed']} "
        f"with_voices={payload['saves_with_voices']} with_missing={payload['saves_with_missing']}"
    )


if __name__ == "__main__":
    main()
