"""Reduce native traces into acceptance receipts without filling missing evidence."""
from __future__ import annotations

from collections import defaultdict
import json
from pathlib import Path
import re

from feel.records import open_record
from feel.report import parse_fullstate, compare_fullstate_histories
from compare_sim_traces import CORE

HISTORY = ("session", "match", "history_branch", "source_round")


def rows(path):
    with open_record(Path(path), encoding="utf-8-sig") as stream:
        for number, line in enumerate(stream, 1):
            if line.strip():
                value = json.loads(line)
                if not isinstance(value, dict):
                    raise ValueError(f"non-object native record at line {number}")
                yield value


def live_hashes(paths, first, last):
    if len(paths) < 2 or first < 1 or last < first:
        raise ValueError("live comparison needs peers and a nonempty declared interval")
    seen, values, invalid, duplicates = set(), defaultdict(dict), 0, 0
    for peer, path in paths.items():
        for row in rows(path):
            tick = row.get("tick")
            if type(tick) is not int:
                invalid += 1
                continue
            if not first <= tick <= last:
                continue
            subsystems = row.get("subsystems")
            required = CORE | {"controller"}
            if (not all(field in row for field in (*HISTORY, "instance", "execution", "incarnation")) or
                    row.get("instance") != ("erol" if peer == "pc" else peer) or
                    not isinstance(subsystems, dict) or not set(required) <= set(subsystems) or
                    not re.fullmatch(r"[0-9a-f]{64}", str(row.get("sim_gated", ""))) or
                    any(not re.fullmatch(r"[0-9a-f]{64}", str(h)) for h in (subsystems or {}).values())):
                invalid += 1
                continue
            identity = (peer, row["execution"], row["incarnation"], *(row[k] for k in HISTORY), tick)
            duplicates += identity in seen
            seen.add(identity)
            signature = ((tuple(row[k] for k in HISTORY)), row["sim_gated"],
                         tuple(sorted((k, v) for k, v in subsystems.items() if k != "controller_route")))
            values[tick].setdefault(peer, []).append(signature)
    missing, unequal, compared = 0, 0, 0
    for tick in range(first, last+1):
        found = values[tick]
        missing += len(set(paths)-set(found))
        signatures = [signature for observations in found.values() for signature in observations]
        unequal += bool(signatures) and any(value != signatures[0] for value in signatures[1:])
        compared += set(found) == set(paths)
    return dict(first=first, last=last, expected=last-first+1, compared=compared,
                missing=missing, unequal=unequal, invalid=invalid, duplicates=duplicates)


def fullstate_hashes(logs, first, last):
    documents = {peer: parse_fullstate([path]) for peer, path in logs.items()}
    low, high = (first+59)//60*60, last//60*60
    rounds = {sample["key"][0] for document in documents.values() for sample in document["samples"]
              if low <= sample["key"][1] <= high and sample["key"][2] == "sample"}
    if len(rounds) != 1 or high < low:
        return dict(first=low, last=high, expected=0, compared=0, missing=1, unequal=0, invalid=1, duplicates=0)
    expected = [(next(iter(rounds)), tick, "sample") for tick in range(low, high+1, 60)]
    result = compare_fullstate_histories(documents, expected)
    duplicates = sum(len([s for s in doc["samples"] if tuple(s["key"]) == key])-1
                     for doc in documents.values() for key in expected if any(tuple(s["key"]) == key for s in doc["samples"]))
    return dict(first=low, last=high, expected=len(expected), compared=result["compared_samples"],
                missing=len(result["missing"])+sum(result["held"].values())+sum(result["coalesced"].values()),
                unequal=len(result["differences"])+sum(not r["equal"] for r in result["restores"]),
                invalid=len(result["scope_failures"])+len(result["refusals"]), duplicates=duplicates,
                native_comparison=result)


def peer_receipt(box, live_path, log, record, first, last):
    observed = defaultdict(list)
    for row in rows(live_path):
        if row.get("phase") == "live" and first <= row.get("tick", -1) <= last:
            observed[row["tick"]].append(row.get("wall_ms"))
    waits = [(int(t), float(ms)) for t, ms in re.findall(r"\[net-frame-wait\] frame=(\d+) wait_ms=(\d+(?:\.\d+)?)", log)]
    holds = [int(t) for t in re.findall(r"\[net-match\] hold peer=\d+ frame=(\d+)", log) if first <= int(t) <= last]
    windows = []
    for low in range(first, last, 60):
        high = min(low+60, last)
        times = [observed.get(tick, []) for tick in range(low, high+1)]
        complete = all(len(value) == 1 and type(value[0]) in (int, float) for value in times)
        interval_waits = [ms for tick, ms in waits if low < tick <= high]
        windows.append(dict(first=low, last=high, samples=sum(bool(value) for value in times),
                            elapsed_ms=times[-1][0]-times[0][0] if complete else None,
                            wait_ms=sum(interval_waits) if log else None,
                            max_wait_ms=max(interval_waits, default=0) if log else None))
    bounds = {label: observed[tick][0] if len(observed[tick]) == 1 else None for label, tick in (("first_wall_ms", first), ("last_wall_ms", last))}
    return dict(box=box, first=first, last=last, **bounds, completed=record.get("exit_code") == 0 and not record.get("timed_out", False),
                holds=holds if log else None, timing=windows)


def tagged_receipts(log, tag):
    values = []
    for line in log.splitlines():
        if line.startswith(f"[{tag}] "):
            value = json.loads(line[len(tag)+3:])
            if not isinstance(value, dict):
                raise ValueError("tagged receipt is not an object")
            values.append(value)
    return values
