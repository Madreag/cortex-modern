#!/usr/bin/env python
"""Oracle for expire3: after the claim the actor returns to the host in CIM_AI.

usage: check_claimed_actor_expiry.py <host_report> <client2_report> <host_log> <client2_log>
       [<host_trace> <client2_trace>]
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

CIM_AI = 2
RETURNED = re.compile(
    r"\[net-match\] claim of actor (\d+) returned to peer (\d+) after seat (\d+) expired"
)

sys.path.insert(0, str(Path(__file__).resolve().parent))
try:
    from compare_sim_traces import strict_compare
except Exception:
    strict_compare = None


def load_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def owner_rows(report) -> list[dict]:
    log = report.get("owner_log") if isinstance(report, dict) else None
    if not isinstance(log, list):
        return []
    rows = []
    for entry in log:
        if not isinstance(entry, dict) or "tick" not in entry or "uid" not in entry:
            continue
        rows.append(
            {
                "tick": int(entry["tick"]),
                "uid": int(entry["uid"]),
                "owner": int(entry.get("owner", -1)),
                "mode": int(entry.get("mode", -1)),
            }
        )
    return rows


def after_expiry(rows: list[dict]) -> tuple[list[dict], str]:
    claimed = [row for row in rows if row["owner"] == 2]
    if not claimed:
        return [], "no owner=2 claim rows"
    last_claim = max(row["tick"] for row in claimed)
    later = [row for row in rows if row["tick"] > last_claim]
    expiry = next((row["tick"] for row in later if row["mode"] == CIM_AI), None)
    if expiry is None:
        return later, f"last_claim_tick={last_claim} no CIM_AI after claim later={len(later)}"
    suffix = [row for row in later if row["tick"] >= expiry]
    return suffix, f"last_claim_tick={last_claim} expiry_tick={expiry} later={len(suffix)}"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("host_report", type=Path)
    ap.add_argument("client2_report", type=Path)
    ap.add_argument("host_log", type=Path)
    ap.add_argument("client2_log", type=Path)
    ap.add_argument("host_trace", type=Path, nargs="?")
    ap.add_argument("client2_trace", type=Path, nargs="?")
    ap.add_argument("--expected-ticks", type=int, default=600)
    args = ap.parse_args()
    failures = []

    host_later, host_detail = after_expiry(owner_rows(load_json(args.host_report)))
    client_later, client_detail = after_expiry(owner_rows(load_json(args.client2_report)))
    host_ok = bool(host_later) and all(row["owner"] == 1 and row["mode"] == CIM_AI for row in host_later)
    client_ok = bool(client_later) and all(row["owner"] == 1 and row["mode"] == CIM_AI for row in client_later)
    if not host_ok or not client_ok or host_later != client_later:
        failures.append(
            f"after claim: host_ok={host_ok} ({host_detail}) client_ok={client_ok} ({client_detail}) "
            f"identical={host_later == client_later} host_sample={host_later[:3]}"
        )
        if any(row["owner"] == 0 for row in host_later):
            failures.append("owner=0 rows after the claim")
        if any(row["mode"] == 0 for row in host_later):
            failures.append("actor disabled (mode=0) after the claim")

    host_line = RETURNED.search(args.host_log.read_text(encoding="utf-8-sig", errors="replace"))
    client_line = RETURNED.search(args.client2_log.read_text(encoding="utf-8-sig", errors="replace"))
    if not host_line or not client_line or host_line.group(0) != client_line.group(0):
        failures.append(
            f"returned line host={host_line.group(0) if host_line else ''} "
            f"client2={client_line.group(0) if client_line else ''}"
        )

    if args.host_trace and args.client2_trace:
        if strict_compare is None:
            failures.append("compare_sim_traces unavailable")
        else:
            try:
                ok, info = strict_compare(str(args.host_trace), str(args.client2_trace), args.expected_ticks)
                if not ok:
                    failures.append(f"traces: {info}")
            except Exception as exc:
                failures.append(f"compare_sim_traces: {exc!r}")

    if failures:
        for line in failures:
            print("FAIL: " + line, file=sys.stderr)
        return 1
    print(
        f"PASS claimed_actor_expiry: later={len(host_later)} returned={host_line.group(0) if host_line else ''}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
