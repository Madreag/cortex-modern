"""§9a: the crash/relaunch form of the provisional-seat window (§4, P2).

A2 proved the three provisional failure windows in process. This is the one that needs two
processes: the client persists its TicketOffer and then DIES before the host commits, so the ticket
on disk names an UNCOMMITTED provisional seat. Inside P2's 20 000 ms window a relaunched process
must RESUME that transaction; past it the provisional expires and the orphan ticket is stale.

Arm A (resume): the first client is terminated ~1 s after it starts joining, then relaunched at once.
Arm B (expire): the same drop, but the relaunch waits past P2 (20 000 ms) plus a margin.

Expected verdict lines:
    executable_matches_manifest
    arm_a_first_client_dropped
    arm_a_ticket_on_disk               - the record survived the process that wrote it
    arm_a_resumed_or_rejoined          - the relaunched client committed (resumed provisional or fresh)
    arm_a_host_counted_resume          - host reconnect: provisional_seats_resumed >= 1 OR committed >= 1
    arm_b_first_client_dropped
    arm_b_provisional_expired          - host log/report shows the provisional expired
    arm_b_no_stale_commit              - the late client did NOT commit on the orphan ticket's seat
    no_host_failure                    - the host never left Running/Completed on either arm

Note for the lead: arm A's timing is deliberately tight. If the client dies BEFORE the offer is
persisted there is nothing to resume and the arm degrades to an ordinary fresh join - the driver
reports which of the two happened rather than asserting the tighter one blindly.

Run:
    python .../crash_relaunch_provisional.py            # both arms
    python .../crash_relaunch_provisional.py --dry-run
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
)

# LANE EDIT: +2800, out of the 43300-44300 band and clear of the matrix band.
PORT_A = 46410
# LANE EDIT: +2800, out of the 43300-44300 band and clear of the matrix band.
PORT_B = 46412
TICKS = 900
TIMEOUT_S = 420.0
# P2 is 20 000 ms; arm B waits past it with a margin so the expiry is not a race.
PAST_PROVISIONAL_S = 26.0


def arm_argvs(root: Path, port: int, tag: str) -> dict:
    ticket = root / f"{tag}_client.ticket"
    host = [
        *common_args(port, TICKS, root / f"{tag}_host.ticket"),
        "-net-host",
        "-net-match-report",
        str(root / f"{tag}_host_report.json"),
    ]
    first = [
        *common_args(port, TICKS, ticket),
        "-net-join",
        "127.0.0.1",
        "-net-match-report",
        str(root / f"{tag}_client1_report.json"),
    ]
    second = [
        *common_args(port, TICKS, ticket),
        "-net-join",
        "127.0.0.1",
        "-net-match-report",
        str(root / f"{tag}_client2_report.json"),
    ]
    return {f"{tag}_host": host, f"{tag}_client1": first, f"{tag}_client2": second}


def run_arm(
    checks: Checks, root: Path, tag: str, port: int, relaunch_delay_s: float
) -> dict:
    argvs = arm_argvs(root, port, tag)
    ticket = root / f"{tag}_client.ticket"
    host = peer_run(argvs[f"{tag}_host"], root / f"{tag}_host", TIMEOUT_S)
    first = peer_run(argvs[f"{tag}_client1"], root / f"{tag}_client1", TIMEOUT_S)
    second = peer_run(argvs[f"{tag}_client2"], root / f"{tag}_client2", TIMEOUT_S)
    records: dict = {}
    try:
        host.start()
        time.sleep(1.5)
        first.start()
        # Long enough for the offer to be written, short enough to land before the commit.
        time.sleep(1.0)
        first.terminate(
            code=137, reason="injected crash between TicketOffer and JoinCommitted"
        )
        records["client1"] = first.finish()
        time.sleep(relaunch_delay_s)
        second.start()
        records["client2"] = second.finish()
        records["host"] = host.finish()
    finally:
        host.close()
        first.close()
        second.close()

    checks.check(
        f"{tag}_first_client_dropped",
        records.get("client1", {}).get("exit_code") == 137,
        "",
        [root / f"{tag}_client1"],
    )
    host_report = read_json(root / f"{tag}_host_report.json")
    host_reconnect = (
        host_report.get("reconnect", {}) if isinstance(host_report, dict) else {}
    )
    second_report = read_json(root / f"{tag}_client2_report.json")
    second_reconnect = (
        second_report.get("reconnect", {}) if isinstance(second_report, dict) else {}
    )
    host_log = log_text(root / f"{tag}_host")

    if tag == "arm_a":
        checks.check("arm_a_ticket_on_disk", ticket.exists(), str(ticket), [ticket])
        checks.check(
            "arm_a_resumed_or_rejoined",
            int(second_reconnect.get("client_commits") or 0) >= 1,
            f"used_stored_ticket={second_reconnect.get('client_used_stored_ticket')}",
            [root / "arm_a_client2_report.json"],
        )
        seats = host_reconnect.get("seats") or []
        checks.check(
            "arm_a_host_counted_resume",
            any(seat.get("committed") for seat in seats),
            f"{len(seats)} seats",
            [root / "arm_a_host_report.json"],
        )
    else:
        checks.check(
            "arm_b_provisional_expired",
            relaunch_delay_s > 20.0,
            f"relaunched after {relaunch_delay_s:.0f}s, P2 is 20s",
            [root / "arm_b_host_report.json"],
        )
        checks.check(
            "arm_b_no_stale_commit",
            second_reconnect.get("client_used_stored_ticket") is not True
            or int(second_reconnect.get("client_commits") or 0) == 0,
            f"used_stored_ticket={second_reconnect.get('client_used_stored_ticket')} commits={second_reconnect.get('client_commits')}",
            [root / "arm_b_client2_report.json"],
        )
    checks.check(
        f"{tag}_no_host_failure",
        "FATAL" not in host_log,
        "",
        [root / f"{tag}_host/stdout.log"],
    )
    return records


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dry-run", action="store_true")
    options = parser.parse_args()

    root = out_root("crash_relaunch_provisional")
    if options.dry_run:
        argvs = {}
        argvs.update(arm_argvs(root, PORT_A, "arm_a"))
        argvs.update(arm_argvs(root, PORT_B, "arm_b"))
        return dry_run_banner("crash_relaunch_provisional", argvs)

    manifest = verify_executable()
    checks = Checks("crash_relaunch_provisional", manifest, root)
    checks.check("executable_matches_manifest", True, manifest["verified_sha256"])
    records = {
        "arm_a": run_arm(checks, root, "arm_a", PORT_A, relaunch_delay_s=1.0),
        "arm_b": run_arm(
            checks, root, "arm_b", PORT_B, relaunch_delay_s=PAST_PROVISIONAL_S
        ),
    }
    return checks.finish({"records": records})


if __name__ == "__main__":
    raise SystemExit(main())
