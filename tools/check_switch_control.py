#!/usr/bin/env python
"""Oracle for the two-process switch-control e2e.

Reads both peers' match reports (owner_log) and -tick-hashes traces. A script line at tick T
rides that tick's frame and lands at T+D. Exit 0 = pass, 1 = fail (reasons printed).

usage: check_switch_control.py <host_report> <client_report> <host_trace> <client_trace>
       --delay D --switch-tick T [--host-peer 1] [--client-peer 2] [--expected-ticks 600]
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

CIM_PLAYER = 1
CIM_AI = 2

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare_sim_traces import strict_compare  # noqa: E402


def load_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def owner_log_of(report) -> list:
    log = report.get("owner_log") if isinstance(report, dict) else None
    return log if isinstance(log, list) else []


def canon(entries: list) -> list:
    out = []
    for entry in entries:
        if not isinstance(entry, dict):
            continue
        out.append(
            {
                "tick": int(entry["tick"]),
                "uid": int(entry["uid"]),
                "owner": int(entry["owner"]),
                "mode": int(entry.get("mode", -1)),
            }
        )
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("host_report", type=Path)
    ap.add_argument("client_report", type=Path)
    ap.add_argument("host_trace", type=Path)
    ap.add_argument("client_trace", type=Path)
    ap.add_argument("--delay", type=int, required=True)
    ap.add_argument("--switch-tick", type=int, required=True)
    ap.add_argument("--host-peer", type=int, default=1)
    ap.add_argument("--client-peer", type=int, default=2)
    ap.add_argument("--expected-ticks", type=int, default=600)
    args = ap.parse_args()
    failures = []

    host_report = load_json(args.host_report)
    client_report = load_json(args.client_report)
    host_log = canon(owner_log_of(host_report))
    client_log = canon(owner_log_of(client_report))
    if host_log != client_log:
        failures.append(
            f"owner_log differs: host n={len(host_log)} client n={len(client_log)}"
        )
        for h, c in zip(host_log, client_log):
            if h != c:
                failures.append(f"owner_log first mismatch host={h} client={c}")
                break
        if len(host_log) != len(client_log):
            failures.append(
                f"owner_log length host={len(host_log)} client={len(client_log)}"
            )
    if not host_log:
        failures.append("owner_log empty on both peers")

    apply_tick = args.switch_tick + args.delay
    handback_apply = args.switch_tick + 10 + args.delay
    by_tick = {e["tick"]: e for e in host_log}
    uids = {e["uid"] for e in host_log}
    if len(uids) != 1:
        failures.append(f"owner_log uids={sorted(uids)}, expected one switched actor")
    takeover = [e for e in host_log if apply_tick <= e["tick"] < handback_apply]
    after = [e for e in host_log if e["tick"] >= handback_apply]
    if not takeover:
        failures.append(
            f"no owner_log rows in takeover window [{apply_tick}, {handback_apply})"
        )
    for e in takeover:
        if e["owner"] != args.client_peer:
            failures.append(
                f"tick {e['tick']}: owner={e['owner']}, expected client peer {args.client_peer}"
            )
            break
        if e["mode"] != CIM_PLAYER:
            failures.append(
                f"tick {e['tick']}: mode={e['mode']}, expected CIM_PLAYER ({CIM_PLAYER})"
            )
            break
    if not after:
        failures.append(f"no owner_log rows after hand-back apply {handback_apply}")
    for e in after:
        if e["owner"] != args.host_peer:
            failures.append(
                f"tick {e['tick']}: owner={e['owner']} after hand-back, expected host {args.host_peer}"
            )
            break
        if e["mode"] != CIM_AI:
            failures.append(
                f"tick {e['tick']}: mode={e['mode']} after hand-back, expected CIM_AI ({CIM_AI})"
            )
            break
    if apply_tick not in by_tick:
        failures.append(f"owner_log missing apply tick {apply_tick}")
    if handback_apply not in by_tick:
        failures.append(f"owner_log missing hand-back apply tick {handback_apply}")

    try:
        ok, info = strict_compare(
            str(args.host_trace), str(args.client_trace), args.expected_ticks
        )
        if not ok:
            failures.append(f"traces: {info}")
    except Exception as exc:
        failures.append(f"compare_sim_traces: {exc!r}")

    if failures:
        for line in failures:
            print("FAIL: " + line, file=sys.stderr)
        return 1
    uid = next(iter(uids)) if uids else 0
    print(
        f"PASS switch_control: uid={uid} apply={apply_tick} handback={handback_apply} "
        f"owner_log={len(host_log)} traces={args.expected_ticks}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
