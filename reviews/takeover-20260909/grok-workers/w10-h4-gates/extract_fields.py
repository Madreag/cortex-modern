"""Pull specific report fields the summaries truncated."""
from __future__ import annotations

import json
from pathlib import Path

OUT = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w10-h4-gates")
PATHS = [
    OUT / r"quotes\j33\clean_leave_20260910_021812\ambiguous_host_report.json",
    OUT / r"quotes\j33\clean_leave_20260910_021812\clean_stale_report.json",
    OUT / r"quotes\j33\clean_leave_20260910_021812\clean_host2_report.json",
    OUT / r"quotes\j33\clean_leave_20260910_021812\clean_host_report.json",
    OUT / r"quotes\j34\reclaim_socket_20260910_021916\host_report.json",
    OUT / r"quotes\j36\fencing_two_transports_20260910_022107\host_report.json",
    OUT / r"quotes\j36\fencing_two_transports_20260910_022107\client2_report.json",
    OUT / r"quotes\j37\rejoin_after_resync_20260910_022132\resync_host_report.json",
]


def main() -> None:
    lines = []
    for path in PATHS:
        data = json.loads(path.read_text(encoding="utf-8-sig"))
        svc = data.get("service") or {}
        rec = svc.get("reconnect") or {}
        runner = svc.get("runner") or {}
        session = runner.get("session") or {}
        admission = session.get("admission") or {}
        lines.append(f"===== {path.name} =====")
        for key in (
            "exit_code",
            "runtime_error",
            "activity_state",
            "winner_team",
            "running_ticks",
            "resyncs",
            "actors",
            "actors_peak",
        ):
            lines.append(f"  {key}={data.get(key)}")
        lines.append(f"  service.error={svc.get('error')!r}")
        lines.append(f"  service.state={svc.get('state')}")
        lines.append(f"  runner_present={bool(runner)} runner.state={runner.get('state')}")
        lines.append(f"  admission_keys={sorted(admission)}")
        for key in (
            "incarnations_bound",
            "fenced_disconnects",
            "fenced_packets",
            "reclaims_accepted",
            "new_joins",
            "seats_closed_by_leave",
            "seats_dropped",
        ):
            lines.append(f"  admission.{key}={admission.get(key)}")
        stats = session.get("stats") or {}
        for key in ("fenced_disconnects", "fenced_packets", "incarnations_bound"):
            lines.append(f"  session.stats.{key}={stats.get(key)}")
        for key in (
            "client_used_stored_ticket",
            "client_commits",
            "client_reject_reason",
            "host_seats_dropped",
            "host_reseats_without_survivors",
            "host_reseats_issued",
            "seats",
        ):
            lines.append(f"  reconnect.{key}={rec.get(key)}")
        lines.append("")
    dest = OUT / "report_fields.txt"
    dest.write_text("\n".join(lines), encoding="utf-8")
    print("wrote", dest)


if __name__ == "__main__":
    main()
