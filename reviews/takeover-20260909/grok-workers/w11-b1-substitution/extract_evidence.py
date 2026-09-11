"""Dump B1 report-field comparison. Read-only on run roots and RESUME.md."""
from __future__ import annotations

import json
from pathlib import Path

OUT = Path(__file__).resolve().parent / "evidence"
OUT.mkdir(exist_ok=True)

ROOTS = {
    "commit": Path(r"D:\mx\s41b3\j43\substitute_commit_20260910_022955"),
    "returner": Path(r"D:\mx\s41b3\j44\substitute_returner_wins_20260910_023039"),
    "cancel": Path(r"D:\mx\s41b3\j45\substitute_host_cancel_20260910_023124"),
    "bounds": Path(r"D:\mx\s41b3\j46\substitute_bounds_20260910_023246"),
}


def load(path: Path):
    if not path.exists():
        return None
    return json.loads(path.read_text(encoding="utf-8-sig"))


def pick(report):
    if not isinstance(report, dict):
        return {"missing": True}
    service = report.get("service") if isinstance(report.get("service"), dict) else {}
    reconnect = service.get("reconnect") if isinstance(service.get("reconnect"), dict) else {}
    runner = service.get("runner") if isinstance(service.get("runner"), dict) else None
    session = runner.get("session") if isinstance(runner, dict) else None
    admission = session.get("admission") if isinstance(session, dict) else None
    return {
        "exit_code": report.get("exit_code"),
        "runtime_error": report.get("runtime_error"),
        "activity_state": report.get("activity_state"),
        "winner_team": report.get("winner_team"),
        "resyncs": report.get("resyncs"),
        "running_ticks": report.get("running_ticks"),
        "sim_ticks": (report.get("pace") or {}).get("sim_ticks"),
        "service_state": service.get("state"),
        "service_status": service.get("status"),
        "service_error": service.get("error"),
        "is_host": service.get("is_host"),
        "local_peer_id": service.get("local_peer_id"),
        "has_runner": runner is not None,
        "admission_is_none": admission is None,
        "admission": admission,
        "reconnect_subset": {
            k: reconnect.get(k)
            for k in (
                "admission_attached",
                "host_seats_dropped",
                "host_ledger_drops_recorded",
                "host_reseats_issued",
                "host_reseats_without_a_ledger",
                "host_reseats_without_survivors",
                "client_state",
                "client_commits",
                "client_applications_sent",
                "client_applications_acknowledged",
                "client_substitution_offers",
                "client_substitution_acks",
                "client_used_stored_ticket",
                "ticket_stored",
                "moderation",
                "seats",
            )
        },
    }


summary = {}
for name, root in ROOTS.items():
    peers = {}
    for peer in ("host", "stayer", "substitute", "returner", "leaver"):
        report = load(root / f"{peer}_report.json")
        if report is not None:
            peers[peer] = pick(report)
    verdict = load(root / "verdict.json")
    summary[name] = {
        "root": str(root),
        "verdict_passed": None if verdict is None else verdict.get("passed"),
        "verdict_fails": []
        if verdict is None
        else [
            {"name": c.get("name"), "detail": c.get("detail")}
            for c in verdict.get("checks", [])
            if c.get("status") == "fail"
        ],
        "peers": peers,
    }

(OUT / "report_fields.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")

needles = ("B1", "substitution", "substitute_commit", "returner_wins")
hits = []
resume = Path(r"D:\Projects\RESUME.md")
for i, line in enumerate(resume.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
    if any(n in line for n in needles):
        hits.append(f"{i}:{line[:600]}")
(OUT / "resume_hits.txt").write_text("\n".join(hits) + "\n", encoding="utf-8")
print(f"wrote {OUT / 'report_fields.json'} and {OUT / 'resume_hits.txt'} hits={len(hits)}")
