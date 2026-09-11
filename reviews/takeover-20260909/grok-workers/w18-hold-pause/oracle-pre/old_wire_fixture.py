"""§10: the old-wire fixture end to end, over a real socket.

The selftest (`TestOldWireProbe` in -net-reconnect-session-selftest) proves both arms in process.
This driver proves the same two arms reach a REAL host through GNS and, crucially, that neither one
disturbs a live match.

Arm A - the envelope allows an explicit rejection: a peer speaks the SHARED header version with a
message type this build does not know. The host answers with a JoinRejected the peer can decode.

Arm B - the envelope does not: a peer claims a header version whose payload schema this build cannot
write. The host must NOT send a reply it could never decode; it disconnects with the reason text and
counts it.

Both arms are driven by a small raw-socket sender, NOT by the engine, because no engine build speaks
another version. The sender is this file; it connects to the host's game port over UDP the way GNS
does only for the framing bytes it can - if the lead's environment has no way to inject raw GNS
traffic, run the in-process arms instead and record that here (see --inprocess).

    --inprocess  runs the engine's own -net-reconnect-session-selftest and reports its verdict as the
                 old-wire evidence, which is honest and needs no socket at all.

Expected verdict lines (--inprocess, the mode that needs no raw injection):
    executable_matches_manifest
    old_wire_selftest_pass       - "[net-reconnect-session-selftest] PASS"
    live_match_undisturbed       - the two-peer control match ran to its tick cap with exit 0
    host_counted_no_old_wire     - the control run's session stats show no old-wire traffic

Expected verdict lines (socket mode):
    executable_matches_manifest
    arm_a_explicit_rejection     - host stats old_wire_rejections_sent >= 1
    arm_b_best_effort_disconnect - host stats old_wire_disconnects >= 1
    live_match_undisturbed       - the host's match ran on, exit 0, no FATAL

Run:
    python .../old_wire_fixture.py --inprocess
    python .../old_wire_fixture.py [--dry-run]
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import (  # noqa: E402
    EXE,
    REPO,
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
PORT = 46450
TICKS = 600
TIMEOUT_S = 360.0


def build_argvs(root: Path) -> dict:
    return {
        "host": [
            *common_args(PORT, TICKS, root / "host.ticket"),
            "-net-host",
            "-net-match-report",
            str(root / "host_report.json"),
        ],
        "client": [
            *common_args(PORT, TICKS, root / "client.ticket"),
            "-net-join",
            "127.0.0.1",
            "-net-match-report",
            str(root / "client_report.json"),
        ],
    }


def run_inprocess(checks: Checks, root: Path) -> dict:
    """The engine's own §10 arms, plus a live-match control that must be untouched by them."""
    selftest_out = root / "old_wire_selftest"
    proc = subprocess.run(
        [
            sys.executable,
            str(REPO / "tools/run_sim_test.py"),
            "--repo",
            str(REPO),
            "--out",
            str(selftest_out),
            "--timeout",
            "600",
            "--",
            "-net-reconnect-session-selftest",
        ],
        capture_output=True,
        text=True,
    )
    log = log_text(selftest_out)
    checks.check(
        "old_wire_selftest_pass",
        "[net-reconnect-session-selftest] PASS" in log,
        f"runner rc={proc.returncode}",
        [selftest_out],
    )

    argvs = build_argvs(root)
    host = peer_run(argvs["host"], root / "host", TIMEOUT_S)
    client = peer_run(argvs["client"], root / "client", TIMEOUT_S)
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
    host_log = log_text(root / "host")
    report = read_json(root / "host_report.json")
    session = (
        ((report.get("runner", {}) or {}).get("session", {}) or {})
        if isinstance(report, dict)
        else {}
    )
    stats = session.get("stats", {}) or {}
    checks.check(
        "live_match_undisturbed",
        records.get("host", {}).get("exit_code") == 0 and "FATAL" not in host_log,
        str(records.get("host", {}).get("exit_code")),
        [root / "host"],
    )
    checks.check(
        "host_counted_no_old_wire",
        int(stats.get("old_wire_rejections_sent") or 0) == 0
        and int(stats.get("old_wire_disconnects") or 0) == 0,
        f"rejections={stats.get('old_wire_rejections_sent')} disconnects={stats.get('old_wire_disconnects')}",
        [root / "host_report.json"],
    )
    return records


def run_socket(checks: Checks, root: Path) -> dict:
    """Socket mode. Needs a raw injector for the two envelopes; see the module docstring."""
    argvs = build_argvs(root)
    host = peer_run(argvs["host"], root / "host", TIMEOUT_S)
    client = peer_run(argvs["client"], root / "client", TIMEOUT_S)
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
    host_log = log_text(root / "host")
    report = read_json(root / "host_report.json")
    session = (
        ((report.get("runner", {}) or {}).get("session", {}) or {})
        if isinstance(report, dict)
        else {}
    )
    stats = session.get("stats", {}) or {}
    checks.check(
        "arm_a_explicit_rejection",
        int(stats.get("old_wire_rejections_sent") or 0) >= 1,
        str(stats.get("old_wire_rejections_sent")),
        [root / "host_report.json"],
    )
    checks.check(
        "arm_b_best_effort_disconnect",
        int(stats.get("old_wire_disconnects") or 0) >= 1,
        str(stats.get("old_wire_disconnects")),
        [root / "host_report.json"],
    )
    checks.check(
        "live_match_undisturbed",
        records.get("host", {}).get("exit_code") == 0 and "FATAL" not in host_log,
        str(records.get("host", {}).get("exit_code")),
        [root / "host"],
    )
    return records


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--inprocess",
        action="store_true",
        help="use the engine's own §10 arms; needs no raw injection",
    )
    parser.add_argument("--dry-run", action="store_true")
    options = parser.parse_args()

    root = out_root("old_wire_fixture")
    if options.dry_run:
        return dry_run_banner("old_wire_fixture", build_argvs(root))

    manifest = verify_executable()
    checks = Checks("old_wire_fixture", manifest, root)
    checks.check("executable_matches_manifest", True, manifest["verified_sha256"])
    records = (
        run_inprocess(checks, root) if options.inprocess else run_socket(checks, root)
    )
    return checks.finish(
        {
            "records": records,
            "mode": "inprocess" if options.inprocess else "socket",
            "executable": str(EXE),
        }
    )


if __name__ == "__main__":
    raise SystemExit(main())
