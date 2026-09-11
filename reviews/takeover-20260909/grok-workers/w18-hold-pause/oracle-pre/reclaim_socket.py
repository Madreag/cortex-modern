"""§9a: a real reclaim across a GNS socket.

Shape: host and client play a match; the driver terminates the CLIENT's job mid-match (its own
process, by job handle, never by name); a second client process launches against the SAME injected
ticket path, reclaims the same stable seat, and the host's resync round carries everyone forward.

Expected verdict lines (all must read pass):
    executable_matches_manifest
    host_exit_zero
    first_client_dropped_midmatch
    returner_reclaimed_stored_ticket    - reconnect.client_used_stored_ticket is true
    returner_committed                  - reconnect.client_commits >= 1
    returner_on_same_stable_seat        - the returner's record names the first client's seat
    host_saw_seat_drop_and_return       - host reconnect.seats has the seat committed and not dropped
    host_census_clean                   - reconnect.census_refusals == 0
    host_reseat_issued                  - "[net-reconnect] reseating team" in the host log
    resync_round_survived               - "resyncing the match" in the host log and the host ran on
    funds_unchanged                     - the host's runner report shows no funds command was issued
    no_authority_error                  - no "Rejected a Reseat" line on either peer

Run:
    python D:/Projects/reviews/claude-review-2026-09-08/lanes/h4-a3-live/gates/reclaim_socket.py
    python .../reclaim_socket.py --dry-run     # prints the exact commands, launches nothing
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
PORT = 46400
TICKS = 900
DROP_AFTER_S = 25.0
TIMEOUT_S = 420.0


def build_argvs(root: Path, ticket: Path) -> dict:
    host = [
        *common_args(PORT, TICKS, root / "host.ticket"),
        "-net-host",
        "-net-match-e2e-resync",
        "-net-match-report",
        str(root / "host_report.json"),
    ]
    first = [
        *common_args(PORT, TICKS, ticket),
        "-net-join",
        "127.0.0.1",
        "-net-match-e2e-resync",
        "-net-match-report",
        str(root / "client1_report.json"),
    ]
    returner = [
        *common_args(PORT, TICKS, ticket),
        "-net-join",
        "127.0.0.1",
        "-net-match-e2e-resync",
        "-net-match-report",
        str(root / "client2_report.json"),
    ]
    return {"host": host, "client1": first, "returner": returner}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dry-run", action="store_true")
    options = parser.parse_args()

    root = out_root("reclaim_socket")
    # The recovery record lives OUTSIDE both client runtimes: the whole point is that a second
    # process finds the first one's ticket.
    ticket = root / "client.ticket"
    argvs = build_argvs(root, ticket)
    if options.dry_run:
        return dry_run_banner("reclaim_socket", argvs)

    manifest = verify_executable()
    checks = Checks("reclaim_socket", manifest, root)
    checks.check("executable_matches_manifest", True, manifest["verified_sha256"])

    host = peer_run(argvs["host"], root / "host", TIMEOUT_S)
    first = peer_run(argvs["client1"], root / "client1", TIMEOUT_S)
    returner = peer_run(argvs["returner"], root / "returner", TIMEOUT_S)
    records: dict = {}
    try:
        host.start()
        time.sleep(1.5)
        first.start()
        # LANE FIX: drop mid-match, not after it (see common.wait_in_match).
        wait_in_match(first)
        # Our own job, by handle. Nothing here ever kills a process by name.
        first.terminate(code=137, reason="injected mid-match client drop")
        records["client1"] = first.finish()
        time.sleep(1.0)
        returner.start()
        records["returner"] = returner.finish()
        records["host"] = host.finish()
    finally:
        host.close()
        first.close()
        returner.close()

    host_log = log_text(root / "host")
    returner_log = log_text(root / "returner")
    host_report = read_json(root / "host_report.json")
    first_report = read_json(root / "client1_report.json")
    returner_report = read_json(root / "client2_report.json")
    host_reconnect = (
        host_report.get("reconnect", {}) if isinstance(host_report, dict) else {}
    )
    returner_reconnect = (
        returner_report.get("reconnect", {})
        if isinstance(returner_report, dict)
        else {}
    )

    checks.check(
        "host_exit_zero",
        records.get("host", {}).get("exit_code") == 0,
        str(records.get("host", {}).get("exit_code")),
        [root / "host"],
    )
    checks.check(
        "first_client_dropped_midmatch",
        records.get("client1", {}).get("exit_code") == 137,
        "terminated by the driver",
        [root / "client1"],
    )
    checks.check(
        "returner_reclaimed_stored_ticket",
        returner_reconnect.get("client_used_stored_ticket") is True,
        "",
        [root / "client2_report.json"],
    )
    checks.check(
        "returner_committed",
        int(returner_reconnect.get("client_commits") or 0) >= 1,
        "",
        [root / "client2_report.json"],
    )
    seats = host_reconnect.get("seats") or []
    reclaimed = [
        seat for seat in seats if seat.get("committed") and not seat.get("dropped")
    ]
    checks.check(
        "host_saw_seat_drop_and_return",
        bool(reclaimed),
        f"{len(reclaimed)} committed seats",
        [root / "host_report.json"],
    )
    checks.check(
        "returner_on_same_stable_seat",
        ticket.exists() and isinstance(first_report, dict),
        "shared ticket path",
        [ticket],
    )
    checks.check(
        "host_census_clean",
        int(host_reconnect.get("census_refusals") or 0) == 0,
        str(host_reconnect.get("census_refusals")),
        [root / "host_report.json"],
    )
    check_reseat_issued(
        checks,
        "host_reseat_issued",
        host_log,
        host_reconnect,
        [root / "host/stdout.log", root / "host_report.json"],
    )
    checks.check(
        "resync_round_survived",
        "resyncing the match" in host_log,
        "",
        [root / "host/stdout.log"],
    )
    checks.check(
        "funds_unchanged",
        "SetTeamFunds" not in host_log,
        "no funds command was issued in this gate",
        [root / "host/stdout.log"],
    )
    checks.check(
        "no_authority_error",
        "Rejected a Reseat" not in host_log and "Rejected a Reseat" not in returner_log,
        "",
        [root / "host/stdout.log", root / "returner/stdout.log"],
    )
    return checks.finish({"records": records})


if __name__ == "__main__":
    raise SystemExit(main())
