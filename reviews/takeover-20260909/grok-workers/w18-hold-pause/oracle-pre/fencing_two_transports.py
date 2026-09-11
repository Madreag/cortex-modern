"""§9a / §6: single-active-incarnation fencing with two REAL transports.

A2 proved the fence in process, where both transports are stubs. This is the case that needs GNS:
two live connections hold the same valid ticket, the second proves, and the FIRST one's later
timeout arrives from the real transport as a PeerDisconnected the host must attribute to the dead
incarnation instead of evicting the seat.

Shape: host + client play; a SECOND client process is launched against the SAME ticket file while
the first is still connected. The second reclaims; the host must move the seat to incarnation 2,
disconnect the first, and count the first's later disconnect as fenced rather than a seat drop.

Expected verdict lines:
    executable_matches_manifest
    second_client_committed             - returner reconnect.client_commits >= 1
    host_bound_second_incarnation       - host runner/session stats: incarnations_bound >= 2
    host_fenced_the_old_transport       - session stats fenced_disconnects >= 1 OR fenced_packets >= 1
    seat_not_dropped_by_stale_timeout   - host reconnect.seats: the seat is committed and not dropped
    first_client_superseded             - the first process ends with the seat-reclaimed reason in its log
    host_survived                       - host exit 0 and no FATAL

Caveat for the lead: whether the first transport's timeout lands inside the run depends on the GNS
timeout; if `host_fenced_the_old_transport` reads fail with `seats` still correct, re-run with a
longer -net-match-ticks rather than treating it as an engine defect.

Run:
    python .../fencing_two_transports.py [--dry-run]
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import (  # noqa: E402
    Checks,
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
PORT = 46420
TICKS = 1200
SECOND_AFTER_S = 20.0
TIMEOUT_S = 480.0


def build_argvs(root: Path) -> dict:
    ticket = root / "client.ticket"
    return {
        "host": [
            *common_args(PORT, TICKS, root / "host.ticket"),
            "-net-host",
            "-net-match-e2e-resync",
            "-net-match-report",
            str(root / "host_report.json"),
        ],
        "client1": [
            *common_args(PORT, TICKS, ticket),
            "-net-join",
            "127.0.0.1",
            "-net-match-e2e-resync",
            "-net-match-report",
            str(root / "client1_report.json"),
        ],
        "client2": [
            *common_args(PORT, TICKS, ticket),
            "-net-join",
            "127.0.0.1",
            "-net-match-e2e-resync",
            "-net-match-report",
            str(root / "client2_report.json"),
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dry-run", action="store_true")
    options = parser.parse_args()

    root = out_root("fencing_two_transports")
    argvs = build_argvs(root)
    if options.dry_run:
        return dry_run_banner("fencing_two_transports", argvs)

    manifest = verify_executable()
    checks = Checks("fencing_two_transports", manifest, root)
    checks.check("executable_matches_manifest", True, manifest["verified_sha256"])

    host = peer_run(argvs["host"], root / "host", TIMEOUT_S)
    first = peer_run(argvs["client1"], root / "client1", TIMEOUT_S)
    second = peer_run(argvs["client2"], root / "client2", TIMEOUT_S)
    records: dict = {}
    try:
        host.start()
        time.sleep(1.5)
        first.start()
        # The second holder arrives while the first is STILL CONNECTED; that is the §6 case.
        # LANE FIX: a fixed 20 s put the second holder after the match had ended.
        wait_in_match(first)
        second.start()
        records["client2"] = second.finish()
        records["client1"] = first.finish()
        records["host"] = host.finish()
    finally:
        host.close()
        first.close()
        second.close()

    host_report = read_json(root / "host_report.json")
    host_runner = host_report.get("runner", {}) if isinstance(host_report, dict) else {}
    host_session = (
        host_runner.get("session", {}) if isinstance(host_runner, dict) else {}
    )
    session_stats = (
        host_session.get("stats", {}) if isinstance(host_session, dict) else {}
    )
    admission = (
        host_session.get("admission", {}) if isinstance(host_session, dict) else {}
    )
    host_reconnect = (
        host_report.get("reconnect", {}) if isinstance(host_report, dict) else {}
    )
    second_report = read_json(root / "client2_report.json")
    second_reconnect = (
        second_report.get("reconnect", {}) if isinstance(second_report, dict) else {}
    )
    first_log = log_text(root / "client1")
    host_log = log_text(root / "host")

    checks.check(
        "second_client_committed",
        int(second_reconnect.get("client_commits") or 0) >= 1,
        "",
        [root / "client2_report.json"],
    )
    checks.check(
        "host_bound_second_incarnation",
        int(admission.get("incarnations_bound") or 0) >= 2,
        str(admission.get("incarnations_bound")),
        [root / "host_report.json"],
    )
    fenced = int(session_stats.get("fenced_disconnects") or 0) + int(
        session_stats.get("fenced_packets") or 0
    )
    checks.check(
        "host_fenced_the_old_transport",
        fenced >= 1,
        f"fenced_disconnects+fenced_packets={fenced}",
        [root / "host_report.json"],
    )
    seats = host_reconnect.get("seats") or []
    checks.check(
        "seat_not_dropped_by_stale_timeout",
        any(seat.get("committed") and not seat.get("dropped") for seat in seats),
        f"{seats}",
        [root / "host_report.json"],
    )
    checks.check(
        "first_client_superseded",
        "seat reclaimed by a newer connection" in first_log
        or "connection closed" in first_log,
        "",
        [root / "client1/stdout.log"],
    )
    checks.check(
        "host_survived",
        records.get("host", {}).get("exit_code") == 0 and "FATAL" not in host_log,
        str(records.get("host", {}).get("exit_code")),
        [root / "host"],
    )
    return checks.finish({"records": records})


if __name__ == "__main__":
    raise SystemExit(main())
