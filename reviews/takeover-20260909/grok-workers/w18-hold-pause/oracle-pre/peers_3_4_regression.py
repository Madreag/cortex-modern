"""§9a: the 3- and 4-player regression with the admission plane live.

A1's post-commit review closed this line for the isolation work (3-peer `273549b3…`, 4-peer
`5a9f0007…`, plus a 3-peer hard-DROP adjudication). Enabling live admission traffic changes the
handshake for every peer - Ready now waits for JoinCommitted - so the line has to be re-run.

Arms: 3 peers, 4 peers, and a 3-peer arm where one client is dropped mid-match and reclaims, which
is the case where the peer-id reuse (P4) and the reseat both have to be right with a third peer
watching.

Expected verdict lines:
    executable_matches_manifest
    peers3_all_exit_zero
    peers3_sim_gated_identical    - every peer's sim-gated tick hash agrees at the cap
    peers4_all_exit_zero
    peers4_sim_gated_identical
    drop3_returner_reclaimed      - the dropped peer's returner used its stored ticket
    drop3_survivors_identical     - the two survivors agree at the cap
    drop3_distinct_seats          - no two peers ended on the same session peer id
    no_census_refusals            - the host walked the world only from the sim tick

Note: the sim-hash comparison uses tools/compare_sim_traces.strict_compare, the same comparator the
retained interp gates use; every peer is launched with -tick-hashes -out.

Run:
    python .../peers_3_4_regression.py [--arm peers3|peers4|drop3|all] [--dry-run]
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import (  # noqa: E402
    REPO,
    Checks,
    common_args,
    dry_run_banner,
    out_root,
    peer_run,
    read_json,
    verify_executable,
    wait_in_match,
)

sys.path.insert(0, str(REPO / "tools"))
try:
    from compare_sim_traces import strict_compare
except Exception:  # the comparator is optional; the driver reports why if it is missing
    strict_compare = None

# LANE EDIT: +2800, out of the 43300-44300 band and clear of the matrix band.
PORT_3 = 46460
# LANE EDIT: +2800, out of the 43300-44300 band and clear of the matrix band.
PORT_4 = 46462
# LANE EDIT: +2800, out of the 43300-44300 band and clear of the matrix band.
PORT_DROP = 46464
TICKS = 600
TIMEOUT_S = 480.0
DROP_AFTER_S = 25.0


def peer_argvs(
    root: Path, port: int, peers: int, tag: str, drop_arm: bool = False
) -> dict:
    argvs: dict = {}
    common = lambda who: [  # noqa: E731 - a local alias keeps the table readable
        *common_args(port, TICKS, root / f"{tag}_{who}.ticket"),
        "-net-match-peers",
        str(peers),
        "-tick-hashes",
        "-max-ticks",
        str(TICKS),
        "-out",
        str(root / f"{tag}_{who}_trace.json"),
        "-net-match-report",
        str(root / f"{tag}_{who}_report.json"),
    ]
    argvs[f"{tag}_host"] = [*common("host"), "-net-host"] + (
        ["-net-match-e2e-resync"] if drop_arm else []
    )
    for index in range(1, peers):
        who = f"client{index}"
        argvs[f"{tag}_{who}"] = [*common(who), "-net-join", "127.0.0.1"] + (
            ["-net-match-e2e-resync"] if drop_arm else []
        )
    if drop_arm:
        argvs[f"{tag}_returner"] = [
            *common_args(port, TICKS, root / f"{tag}_client1.ticket"),
            "-net-match-peers",
            str(peers),
            "-net-join",
            "127.0.0.1",
            "-net-match-e2e-resync",
            "-net-match-report",
            str(root / f"{tag}_returner_report.json"),
        ]
    return argvs


def compare_traces(paths: list[Path]) -> tuple[bool, str]:
    if strict_compare is None:
        return False, "compare_sim_traces is unavailable"
    missing = [str(path) for path in paths if not path.exists()]
    if missing:
        return False, "missing traces: " + ", ".join(missing)
    try:
        first = paths[0]
        for other in paths[1:]:
            result = strict_compare(str(first), str(other))
            if isinstance(result, tuple) and len(result) == 2:
                # LANE FIX: strict_compare's real return shape is (ok, info).
                ok = bool(result[0])
            elif isinstance(result, bool):
                ok = result
            else:
                ok = bool(
                    getattr(result, "identical", False)
                    or (isinstance(result, dict) and result.get("identical"))
                )
            if not ok:
                return False, f"{first.name} vs {other.name}: {result}"
        return True, f"{len(paths)} traces identical"
    except Exception as exc:
        return False, repr(exc)


def run_plain_arm(checks: Checks, root: Path, tag: str, port: int, peers: int) -> dict:
    argvs = peer_argvs(root, port, peers, tag)
    runs = {key: peer_run(argv, root / key, TIMEOUT_S) for key, argv in argvs.items()}
    records: dict = {}
    try:
        order = [f"{tag}_host"] + [f"{tag}_client{index}" for index in range(1, peers)]
        for index, key in enumerate(order):
            runs[key].start()
            if index == 0:
                time.sleep(1.5)
            else:
                time.sleep(0.5)
        for key in order:
            records[key] = runs[key].finish()
    finally:
        for run in runs.values():
            run.close()
    exits = {key: record.get("exit_code") for key, record in records.items()}
    checks.check(
        f"{tag}_all_exit_zero",
        all(code == 0 for code in exits.values()),
        str(exits),
        [root],
    )
    traces = [root / f"{tag}_host_trace.json"] + [
        root / f"{tag}_client{index}_trace.json" for index in range(1, peers)
    ]
    ok, detail = compare_traces(traces)
    checks.check(f"{tag}_sim_gated_identical", ok, detail, traces)
    return records


def run_drop_arm(checks: Checks, root: Path, tag: str, port: int, peers: int) -> dict:
    argvs = peer_argvs(root, port, peers, tag, drop_arm=True)
    runs = {key: peer_run(argv, root / key, TIMEOUT_S) for key, argv in argvs.items()}
    records: dict = {}
    order = [f"{tag}_host"] + [f"{tag}_client{index}" for index in range(1, peers)]
    try:
        for index, key in enumerate(order):
            runs[key].start()
            time.sleep(1.5 if index == 0 else 0.5)
        # LANE FIX: drop mid-match, not after it.
        dropped = f"{tag}_client1"
        wait_in_match(runs[dropped])
        runs[dropped].terminate(code=137, reason="injected mid-match client drop")
        records[dropped] = runs[dropped].finish()
        time.sleep(1.0)
        runs[f"{tag}_returner"].start()
        records[f"{tag}_returner"] = runs[f"{tag}_returner"].finish()
        for key in order:
            if key not in records:
                records[key] = runs[key].finish()
    finally:
        for run in runs.values():
            run.close()

    returner = read_json(root / f"{tag}_returner_report.json")
    returner_reconnect = (
        returner.get("reconnect", {}) if isinstance(returner, dict) else {}
    )
    checks.check(
        f"{tag}_returner_reclaimed",
        returner_reconnect.get("client_used_stored_ticket") is True,
        "",
        [root / f"{tag}_returner_report.json"],
    )
    survivors = [root / f"{tag}_host_trace.json"] + [
        root / f"{tag}_client{index}_trace.json" for index in range(2, peers)
    ]
    ok, detail = compare_traces(survivors)
    checks.check(f"{tag}_survivors_identical", ok, detail, survivors)
    seat_ids = []
    for who in (
        ["host"] + [f"client{index}" for index in range(1, peers)] + ["returner"]
    ):
        report = (
            read_json(root / f"{tag}_{who}_report.json")
            if (root / f"{tag}_{who}_report.json").exists()
            else {}
        )
        if (
            isinstance(report, dict)
            and report.get("local_peer_id") is not None
            and who != "client1"
        ):
            seat_ids.append(report.get("local_peer_id"))
    checks.check(
        f"{tag}_distinct_seats",
        len(seat_ids) == len(set(seat_ids)),
        str(seat_ids),
        [root],
    )
    return records


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--arm", choices=("peers3", "peers4", "drop3", "all"), default="all"
    )
    parser.add_argument("--dry-run", action="store_true")
    options = parser.parse_args()

    root = out_root("peers_3_4_regression")
    if options.dry_run:
        argvs = {}
        argvs.update(peer_argvs(root, PORT_3, 3, "peers3"))
        argvs.update(peer_argvs(root, PORT_4, 4, "peers4"))
        argvs.update(peer_argvs(root, PORT_DROP, 3, "drop3", drop_arm=True))
        return dry_run_banner("peers_3_4_regression", argvs)

    manifest = verify_executable()
    checks = Checks("peers_3_4_regression", manifest, root)
    checks.check("executable_matches_manifest", True, manifest["verified_sha256"])
    records = {}
    if options.arm in ("peers3", "all"):
        records["peers3"] = run_plain_arm(checks, root, "peers3", PORT_3, 3)
    if options.arm in ("peers4", "all"):
        records["peers4"] = run_plain_arm(checks, root, "peers4", PORT_4, 4)
    if options.arm in ("drop3", "all"):
        records["drop3"] = run_drop_arm(checks, root, "drop3", PORT_DROP, 3)

    refusals = 0
    for report in root.glob("*_host_report.json"):
        payload = read_json(report)
        refusals += (
            int(((payload.get("reconnect", {}) or {}).get("census_refusals") or 0))
            if isinstance(payload, dict)
            else 0
        )
    checks.check(
        "no_census_refusals", refusals == 0, f"census_refusals={refusals}", [root]
    )
    return checks.finish({"records": records})


if __name__ == "__main__":
    raise SystemExit(main())
