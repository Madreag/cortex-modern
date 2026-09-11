"""Extract reconnect/admission/runner fields from copied reports."""
from __future__ import annotations

import json
from pathlib import Path

ROOT = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w10-h4-gates\quotes")
KEYS = (
    "client_used_stored_ticket",
    "client_commits",
    "client_leave_acks",
    "census_refusals",
    "seats",
    "seats_dropped",
    "reseats_issued",
    "reseats_without_survivors",
    "ledger_drops_recorded",
)


def pick(obj, depth=0):
    found = {}
    if not isinstance(obj, dict) or depth > 6:
        return found
    for k, v in obj.items():
        if k in {
            "reconnect",
            "admission",
            "stats",
            "session",
            "runner",
            "service",
            "exit",
            "error",
            "result",
            "setupError",
            "setup_error",
            "state",
            "activityState",
            "activity_state",
        } or k in KEYS:
            if isinstance(v, (dict, list)):
                found[k] = v if k in KEYS or k in {"seats", "error", "result"} else pick(v, depth + 1) or v
            else:
                found[k] = v
        elif isinstance(v, dict):
            sub = pick(v, depth + 1)
            if sub:
                found[k] = sub
    return found


def main() -> None:
    lines = []
    for path in sorted(ROOT.rglob("*_report.json")):
        data = json.loads(path.read_text(encoding="utf-8-sig"))
        lines.append(f"===== {path.relative_to(ROOT)} keys={list(data)[:20]} =====")
        extracted = pick(data)
        lines.append(json.dumps(extracted, indent=2)[:8000])
        lines.append("")
    dest = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w10-h4-gates\report_summaries.json.txt")
    dest.write_text("\n".join(lines), encoding="utf-8")
    print("wrote", dest, dest.stat().st_size)


if __name__ == "__main__":
    main()
