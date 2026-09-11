"""§9a / §7: the clean-leave protocol over a real socket, and the ambiguous loss that is not one.

Arm "clean": the client leaves through -net-match-e2e-leave. The §7 exchange must run
(LeaveRequest -> LeaveAck), the host must close the seat and revoke the generation, and the CLIENT
must delete its recovery record. A later process presenting that old ticket must not reclaim.

Arm "ambiguous": the client's process is terminated mid-match instead. §7 is explicit that this must
NOT clear the record - it is exactly when the record is needed - so the ticket file must survive and
a relaunched client must still reclaim.

Expected verdict lines:
    executable_matches_manifest
    clean_leave_acked          - client reconnect.client_leave_acks >= 1
    clean_leave_cleared_ticket - the ticket file is gone after the clean leave
    clean_old_ticket_refused   - a third process with the pre-leave ticket copy does NOT commit
    host_closed_the_seat       - host admission seats_closed_by_leave >= 1
    ambiguous_kept_ticket      - the terminated client's ticket file still exists
    ambiguous_reclaim_worked   - the relaunched client used the stored ticket and committed
    hosts_survived             - both hosts exit 0 with no FATAL

Run:
    python .../clean_leave.py [--arm clean|ambiguous|both] [--dry-run]
"""

from __future__ import annotations

import argparse
import shutil
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
PORT_CLEAN = 46470
# LANE EDIT: +2800, out of the 43300-44300 band and clear of the matrix band.
PORT_AMBIGUOUS = 46472
TICKS = 900
LEAVE_AFTER_S = 20.0
TIMEOUT_S = 420.0


def clean_argvs(root: Path) -> dict:
    ticket = root / "clean_client.ticket"
    return {
        "clean_host": [
            *common_args(PORT_CLEAN, TICKS, root / "clean_host.ticket"),
            "-net-host",
            # LANE FIX: the host must NOT leave; it stays to ack the client's leave.
            "-net-match-report",
            str(root / "clean_host_report.json"),
        ],
        "clean_client": [
            *common_args(PORT_CLEAN, TICKS, ticket),
            "-net-join",
            "127.0.0.1",
            "-net-match-e2e-leave",
            "-net-match-report",
            str(root / "clean_client_report.json"),
        ],
        "clean_stale": [
            *common_args(PORT_CLEAN, TICKS, root / "clean_stale.ticket"),
            "-net-join",
            "127.0.0.1",
            "-net-match-report",
            str(root / "clean_stale_report.json"),
        ],
    }


def ambiguous_argvs(root: Path) -> dict:
    ticket = root / "ambiguous_client.ticket"
    return {
        "ambiguous_host": [
            *common_args(PORT_AMBIGUOUS, TICKS, root / "ambiguous_host.ticket"),
            "-net-host",
            "-net-match-e2e-resync",
            "-net-match-report",
            str(root / "ambiguous_host_report.json"),
        ],
        "ambiguous_client1": [
            *common_args(PORT_AMBIGUOUS, TICKS, ticket),
            "-net-join",
            "127.0.0.1",
            "-net-match-e2e-resync",
            "-net-match-report",
            str(root / "ambiguous_client1_report.json"),
        ],
        "ambiguous_client2": [
            *common_args(PORT_AMBIGUOUS, TICKS, ticket),
            "-net-join",
            "127.0.0.1",
            "-net-match-e2e-resync",
            "-net-match-report",
            str(root / "ambiguous_client2_report.json"),
        ],
    }


def run_clean(checks: Checks, root: Path) -> dict:
    argvs = clean_argvs(root)
    ticket = root / "clean_client.ticket"
    stale_copy = root / "clean_stale.ticket"
    host = peer_run(argvs["clean_host"], root / "clean_host", TIMEOUT_S)
    client = peer_run(argvs["clean_client"], root / "clean_client", TIMEOUT_S)
    records: dict = {}
    try:
        host.start()
        time.sleep(1.5)
        client.start()
        # Copy the live ticket aside BEFORE the leave clears it; that copy is the old-ticket control.
        # LANE FIX: the leave is at tick 300, ~6 s in - a 20 s sleep copies it after.
        wait_in_match(client, settle_s=1.0)
        if ticket.exists():
            shutil.copy2(ticket, stale_copy)
        records["client"] = client.finish()
        records["host"] = host.finish()
    finally:
        host.close()
        client.close()

    client_report = read_json(root / "clean_client_report.json")
    client_reconnect = (
        client_report.get("reconnect", {}) if isinstance(client_report, dict) else {}
    )
    host_report = read_json(root / "clean_host_report.json")
    host_session = (
        ((host_report.get("runner", {}) or {}).get("session", {}) or {})
        if isinstance(host_report, dict)
        else {}
    )
    admission = host_session.get("admission", {}) or {}
    checks.check(
        "clean_leave_acked",
        int(client_reconnect.get("client_leave_acks") or 0) >= 1,
        str(client_reconnect.get("client_leave_acks")),
        [root / "clean_client_report.json"],
    )
    checks.check(
        "clean_leave_cleared_ticket", not ticket.exists(), str(ticket), [ticket.parent]
    )
    checks.check(
        "host_closed_the_seat",
        int(admission.get("seats_closed_by_leave") or 0) >= 1,
        str(admission.get("seats_closed_by_leave")),
        [root / "clean_host_report.json"],
    )

    # The old ticket must not get back in. A fresh host round is needed because the first one ended.
    if stale_copy.exists():
        stale_host = peer_run(
            [
                *common_args(PORT_CLEAN + 1, TICKS, root / "clean_host2.ticket"),
                "-net-host",
                "-net-match-report",
                str(root / "clean_host2_report.json"),
            ],
            root / "clean_host2",
            TIMEOUT_S,
        )
        stale_client = peer_run(
            [
                *common_args(PORT_CLEAN + 1, TICKS, stale_copy),
                "-net-join",
                "127.0.0.1",
                "-net-match-report",
                str(root / "clean_stale_report.json"),
            ],
            root / "clean_stale",
            TIMEOUT_S,
        )
        try:
            stale_host.start()
            time.sleep(1.5)
            stale_client.start()
            records["stale_client"] = stale_client.finish()
            records["stale_host"] = stale_host.finish()
        finally:
            stale_host.close()
            stale_client.close()
        stale_report = read_json(root / "clean_stale_report.json")
        stale_reconnect = (
            stale_report.get("reconnect", {}) if isinstance(stale_report, dict) else {}
        )
        # The old ticket names a revoked generation of a closed seat: it must not reclaim. The client
        # may still join as a NEW player, which is a fresh join, not a reclaim - that is the check.
        checks.check(
            "clean_old_ticket_refused",
            stale_reconnect.get("client_used_stored_ticket") is not True
            or int(stale_reconnect.get("client_commits") or 0) == 0,
            f"used_stored_ticket={stale_reconnect.get('client_used_stored_ticket')} commits={stale_reconnect.get('client_commits')}",
            [root / "clean_stale_report.json"],
        )
    else:
        checks.check(
            "clean_old_ticket_refused",
            False,
            "no pre-leave ticket copy was captured",
            [root],
        )
    return records


def run_ambiguous(checks: Checks, root: Path) -> dict:
    argvs = ambiguous_argvs(root)
    ticket = root / "ambiguous_client.ticket"
    host = peer_run(argvs["ambiguous_host"], root / "ambiguous_host", TIMEOUT_S)
    first = peer_run(argvs["ambiguous_client1"], root / "ambiguous_client1", TIMEOUT_S)
    second = peer_run(argvs["ambiguous_client2"], root / "ambiguous_client2", TIMEOUT_S)
    records: dict = {}
    try:
        host.start()
        time.sleep(1.5)
        first.start()
        # LANE FIX: drop mid-match, not after it.
        wait_in_match(first)
        first.terminate(code=137, reason="injected ambiguous network loss")
        records["client1"] = first.finish()
        checks.check("ambiguous_kept_ticket", ticket.exists(), str(ticket), [ticket])
        time.sleep(1.0)
        second.start()
        records["client2"] = second.finish()
        records["host"] = host.finish()
    finally:
        host.close()
        first.close()
        second.close()
    second_report = read_json(root / "ambiguous_client2_report.json")
    second_reconnect = (
        second_report.get("reconnect", {}) if isinstance(second_report, dict) else {}
    )
    checks.check(
        "ambiguous_reclaim_worked",
        second_reconnect.get("client_used_stored_ticket") is True
        and int(second_reconnect.get("client_commits") or 0) >= 1,
        f"used_stored_ticket={second_reconnect.get('client_used_stored_ticket')} commits={second_reconnect.get('client_commits')}",
        [root / "ambiguous_client2_report.json"],
    )
    return records


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arm", choices=("clean", "ambiguous", "both"), default="both")
    parser.add_argument("--dry-run", action="store_true")
    options = parser.parse_args()

    root = out_root("clean_leave")
    if options.dry_run:
        argvs = {}
        argvs.update(clean_argvs(root))
        argvs.update(ambiguous_argvs(root))
        return dry_run_banner("clean_leave", argvs)

    manifest = verify_executable()
    checks = Checks("clean_leave", manifest, root)
    checks.check("executable_matches_manifest", True, manifest["verified_sha256"])
    records = {}
    if options.arm in ("clean", "both"):
        records["clean"] = run_clean(checks, root)
    if options.arm in ("ambiguous", "both"):
        records["ambiguous"] = run_ambiguous(checks, root)

    fatal = any(
        "FATAL" in log_text(root / name)
        for name in ("clean_host", "ambiguous_host")
        if (root / name).exists()
    )
    exits = [
        record.get("exit_code")
        for arm in records.values()
        for key, record in arm.items()
        if key.endswith("host")
    ]
    checks.check(
        "hosts_survived",
        not fatal and all(code == 0 for code in exits if code is not None),
        f"exits={exits} fatal={fatal}",
        [root],
    )
    return checks.finish({"records": records})


if __name__ == "__main__":
    raise SystemExit(main())
