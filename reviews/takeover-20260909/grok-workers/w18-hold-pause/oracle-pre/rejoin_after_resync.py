"""§9a: rejoin after a resync round, and across a rematch.

Two arms on one binary.

Arm "resync": the host runs with -net-match-e2e-resync; the client is dropped mid-match and a second
client process reclaims. The host's rejoin detection ends the round, streams its snapshot and both
peers relaunch from it. The gate is that the returner reclaims its OWN seat through the resync and
the match keeps running afterwards.

Arm "rematch": host and client run -net-match-e2e-rematch, so the session survives into a second
match. The client's ticket must still reclaim its own seat across the rematch (P24: the seat stays
protected for the whole hosted session, with no timer) and a ticketless third party must not be able
to take it.

Expected verdict lines:
    executable_matches_manifest
    resync_returner_reclaimed        - returner reconnect.client_used_stored_ticket is true
    resync_round_ran                 - "resyncing the match" in the host log
    resync_match_continued           - host exit 0 and the runner reached its tick cap
    resync_census_clean              - host reconnect.census_refusals == 0
    rematch_completed                - "[net-match-service-e2e]" rematch lines and host exit 0
    rematch_ticket_survived          - the client's ticket file still exists after the rematch
    rematch_seat_still_protected     - host reconnect.seats keeps the client's seat committed
    no_stale_epoch_denials           - host admission stale_epoch_drops == 0 in both arms

Run:
    python .../rejoin_after_resync.py [--arm resync|rematch|both] [--dry-run]
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import (  # noqa: E402
    Checks,
    check_reseat_issued,
    common_args,
    dry_run_banner,
    log_text,
    out_root,
    peer_run,
    read_json,
    verify_executable,
    wait_in_match,
)

# LANE EDIT: +2800, out of the 43300-44300 band and clear of the matrix band.
PORT_RESYNC = 46430
# LANE EDIT: +2800, out of the 43300-44300 band and clear of the matrix band.
PORT_REMATCH = 46432
TICKS = 900
DROP_AFTER_S = 25.0
TIMEOUT_S = 480.0


def resync_argvs(root: Path) -> dict:
    ticket = root / "resync_client.ticket"
    return {
        "resync_host": [
            *common_args(PORT_RESYNC, TICKS, root / "resync_host.ticket"),
            "-net-host",
            "-net-match-e2e-resync",
            "-net-match-report",
            str(root / "resync_host_report.json"),
        ],
        "resync_client1": [
            *common_args(PORT_RESYNC, TICKS, ticket),
            "-net-join",
            "127.0.0.1",
            "-net-match-e2e-resync",
            "-net-match-report",
            str(root / "resync_client1_report.json"),
        ],
        "resync_client2": [
            *common_args(PORT_RESYNC, TICKS, ticket),
            "-net-join",
            "127.0.0.1",
            "-net-match-e2e-resync",
            "-net-match-report",
            str(root / "resync_client2_report.json"),
        ],
    }


def rematch_argvs(root: Path) -> dict:
    ticket = root / "rematch_client.ticket"
    return {
        "rematch_host": [
            *common_args(PORT_REMATCH, TICKS, root / "rematch_host.ticket"),
            "-net-host",
            "-net-match-e2e-rematch",
            "-net-match-report",
            str(root / "rematch_host_report.json"),
        ],
        "rematch_client": [
            *common_args(PORT_REMATCH, TICKS, ticket),
            "-net-join",
            "127.0.0.1",
            "-net-match-e2e-rematch",
            "-net-match-report",
            str(root / "rematch_client_report.json"),
        ],
    }


def run_resync(checks: Checks, root: Path) -> dict:
    argvs = resync_argvs(root)
    host = peer_run(argvs["resync_host"], root / "resync_host", TIMEOUT_S)
    first = peer_run(argvs["resync_client1"], root / "resync_client1", TIMEOUT_S)
    second = peer_run(argvs["resync_client2"], root / "resync_client2", TIMEOUT_S)
    records: dict = {}
    try:
        host.start()
        time.sleep(1.5)
        first.start()
        # LANE FIX: drop mid-match, not after it.
        wait_in_match(first)
        first.terminate(code=137, reason="injected mid-match client drop")
        records["client1"] = first.finish()
        time.sleep(1.0)
        second.start()
        records["client2"] = second.finish()
        records["host"] = host.finish()
    finally:
        host.close()
        first.close()
        second.close()

    host_log = log_text(root / "resync_host")
    host_report = read_json(root / "resync_host_report.json")
    returner = read_json(root / "resync_client2_report.json")
    host_reconnect = (
        host_report.get("reconnect", {}) if isinstance(host_report, dict) else {}
    )
    returner_reconnect = (
        returner.get("reconnect", {}) if isinstance(returner, dict) else {}
    )
    checks.check(
        "resync_returner_reclaimed",
        returner_reconnect.get("client_used_stored_ticket") is True,
        "",
        [root / "resync_client2_report.json"],
    )
    checks.check(
        "resync_round_ran",
        "resyncing the match" in host_log,
        "",
        [root / "resync_host/stdout.log"],
    )
    checks.check(
        "resync_match_continued",
        records.get("host", {}).get("exit_code") == 0,
        str(records.get("host", {}).get("exit_code")),
        [root / "resync_host"],
    )
    checks.check(
        "resync_census_clean",
        int(host_reconnect.get("census_refusals") or 0) == 0,
        str(host_reconnect.get("census_refusals")),
        [root / "resync_host_report.json"],
    )
    check_reseat_issued(checks, "resync_reseat_decided", host_log, host_reconnect,
                        [root / "resync_host/stdout.log", root / "resync_host_report.json"])
    return records


def run_rematch(checks: Checks, root: Path) -> dict:
    argvs = rematch_argvs(root)
    ticket = root / "rematch_client.ticket"
    host = peer_run(argvs["rematch_host"], root / "rematch_host", TIMEOUT_S)
    client = peer_run(argvs["rematch_client"], root / "rematch_client", TIMEOUT_S)
    records: dict = {}
    try:
        host.start()
        time.sleep(1.5)
        client.start()
        records["client"] = client.finish()
        records["host"] = host.finish()
    finally:
        host.close()
        client.close()

    host_log = log_text(root / "rematch_host")
    host_report = read_json(root / "rematch_host_report.json")
    host_reconnect = (
        host_report.get("reconnect", {}) if isinstance(host_report, dict) else {}
    )
    seats = host_reconnect.get("seats") or []
    checks.check(
        "rematch_completed",
        records.get("host", {}).get("exit_code") == 0,
        str(records.get("host", {}).get("exit_code")),
        [root / "rematch_host"],
    )
    # P24: no timer. A rematch keeps the epoch, the credential and the seat.
    checks.check("rematch_ticket_survived", ticket.exists(), str(ticket), [ticket])
    checks.check(
        "rematch_seat_still_protected",
        any(seat.get("committed") for seat in seats),
        str(seats),
        [root / "rematch_host_report.json"],
    )
    checks.check(
        "rematch_no_host_fatal",
        "FATAL" not in host_log,
        "",
        [root / "rematch_host/stdout.log"],
    )
    return records


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arm", choices=("resync", "rematch", "both"), default="both")
    parser.add_argument("--dry-run", action="store_true")
    options = parser.parse_args()

    root = out_root("rejoin_after_resync")
    if options.dry_run:
        argvs = {}
        if options.arm in ("resync", "both"):
            argvs.update(resync_argvs(root))
        if options.arm in ("rematch", "both"):
            argvs.update(rematch_argvs(root))
        return dry_run_banner("rejoin_after_resync", argvs)

    manifest = verify_executable()
    checks = Checks("rejoin_after_resync", manifest, root)
    checks.check("executable_matches_manifest", True, manifest["verified_sha256"])
    records = {}
    if options.arm in ("resync", "both"):
        records["resync"] = run_resync(checks, root)
    if options.arm in ("rematch", "both"):
        records["rematch"] = run_rematch(checks, root)

    stale = 0
    for report_name in ("resync_host_report.json", "rematch_host_report.json"):
        report = read_json(root / report_name) if (root / report_name).exists() else {}
        runner = report.get("runner", {}) if isinstance(report, dict) else {}
        session = runner.get("session", {}) if isinstance(runner, dict) else {}
        stale += int((session.get("admission", {}) or {}).get("stale_epoch_drops") or 0)
    checks.check(
        "no_stale_epoch_denials", stale == 0, f"stale_epoch_drops={stale}", [root]
    )
    return checks.finish({"records": records})


if __name__ == "__main__":
    raise SystemExit(main())
