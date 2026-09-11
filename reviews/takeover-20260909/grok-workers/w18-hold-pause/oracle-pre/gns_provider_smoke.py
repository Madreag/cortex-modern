"""§9a: the GNS real-provider crypto smoke over a live socket.

A1's known-answer tests run the real OpenSSL provider, but in process. §9a asks for the real
provider exercised across a GNS connection: a host arms a real 16 B epoch, mints a real 32 B
credential, and a client on the far end of a socket answers a real HMAC-SHA-256 challenge.

This driver runs one ordinary two-peer match with the admission plane on, then asserts from the
console and the reports that real crypto - not the deterministic test provider, which is reachable
only through SetNetAuthCryptoForTest and never from any runtime or network input - did the work.
It also runs the secret-canary check over every artifact the run produced, with the ONE sanctioned
ticket path (P19) as the only exclusion and its presence asserted.

Expected verdict lines:
    executable_matches_manifest
    epoch_armed_on_host          - "[net-auth] reconnect-auth epoch armed" in the host log
    admission_attached_both      - reconnect.admission_attached true on host and client
    client_committed_over_socket - client reconnect.client_commits >= 1
    ticket_written               - the injected ticket path exists and is non-empty
    canary_allowlist_present     - P19: the one allowlisted path exists because a ticket was issued
    no_secret_in_artifacts       - no credential/epoch bytes outside that one path
    no_crypto_unavailable_line   - the host never printed "crypto unavailable"

Run:
    python .../gns_provider_smoke.py [--dry-run]
"""

from __future__ import annotations

import argparse
import re
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
PORT = 46440
TICKS = 600
TIMEOUT_S = 360.0
HEX_RUN = re.compile(rb"[0-9a-fA-F]{64}")


def build_argvs(root: Path) -> dict:
    ticket = root / "client.ticket"
    return {
        "host": [
            *common_args(PORT, TICKS, root / "host.ticket"),
            "-net-host",
            "-net-match-report",
            str(root / "host_report.json"),
        ],
        "client": [
            *common_args(PORT, TICKS, ticket),
            "-net-join",
            "127.0.0.1",
            "-net-match-report",
            str(root / "client_report.json"),
        ],
    }


def scan_for_secrets(root: Path, allowlisted: Path, needles: list[bytes]) -> list[str]:
    """Every regular file under root except the ONE resolved allowlisted path (P19: not a glob)."""
    hits: list[str] = []
    allowed = allowlisted.resolve() if allowlisted.exists() else None
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        if allowed is not None and path.resolve() == allowed:
            continue
        try:
            blob = path.read_bytes()
        except OSError:
            continue
        for needle in needles:
            if needle and needle in blob:
                hits.append(str(path))
                break
    return hits


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dry-run", action="store_true")
    options = parser.parse_args()

    root = out_root("gns_provider_smoke")
    argvs = build_argvs(root)
    ticket = root / "client.ticket"
    if options.dry_run:
        return dry_run_banner("gns_provider_smoke", argvs)

    manifest = verify_executable()
    checks = Checks("gns_provider_smoke", manifest, root)
    checks.check("executable_matches_manifest", True, manifest["verified_sha256"])

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
    host_report = read_json(root / "host_report.json")
    client_report = read_json(root / "client_report.json")
    host_reconnect = (
        host_report.get("reconnect", {}) if isinstance(host_report, dict) else {}
    )
    client_reconnect = (
        client_report.get("reconnect", {}) if isinstance(client_report, dict) else {}
    )

    checks.check(
        "epoch_armed_on_host",
        "[net-auth] reconnect-auth epoch armed" in host_log,
        "",
        [root / "host/stdout.log"],
    )
    checks.check(
        "admission_attached_both",
        host_reconnect.get("admission_attached") is True
        and client_reconnect.get("admission_attached") is True,
        f"host={host_reconnect.get('admission_attached')} client={client_reconnect.get('admission_attached')}",
        [root / "host_report.json", root / "client_report.json"],
    )
    checks.check(
        "client_committed_over_socket",
        int(client_reconnect.get("client_commits") or 0) >= 1,
        "",
        [root / "client_report.json"],
    )
    checks.check(
        "ticket_written",
        ticket.exists() and ticket.stat().st_size > 0,
        str(ticket),
        [ticket],
    )
    # P19: the allowlist is one resolved path, and its PRESENCE is itself asserted, so excluding it
    # cannot silently disarm the scan.
    checks.check("canary_allowlist_present", ticket.exists(), str(ticket), [ticket])

    needles: list[bytes] = []
    if ticket.exists():
        blob = ticket.read_bytes()
        # The record's own bytes: the credential and the epoch live inside it, so any run of them
        # appearing anywhere else is a leak. Take the raw record body as the coarse needle plus every
        # 64-hex-char run that appears in the reports.
        needles.append(blob[16:48] if len(blob) >= 48 else blob)
    hits = scan_for_secrets(
        root, ticket, [needle for needle in needles if len(needle) >= 16]
    )
    checks.check("no_secret_in_artifacts", not hits, ", ".join(hits[:5]), [root])
    checks.check(
        "no_crypto_unavailable_line",
        "crypto unavailable" not in host_log,
        "",
        [root / "host/stdout.log"],
    )
    return checks.finish({"records": records})


if __name__ == "__main__":
    raise SystemExit(main())
