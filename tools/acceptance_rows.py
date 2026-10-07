"""Fail closed on missing or contradictory world/mod acceptance receipts.

Inputs are reductions of native receipts, not scenario declarations. Runtime
adapters retain the source files and the reduction beside each verdict.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import re

ROWS = ("spectator", "mod-match", "mod-refusal", "world-join", "image-sizes", "world-soak")
BOXES = {"pc", "remote", "mac", "linux"}
MIB = 1024**2


def number(value):
    return type(value) in (int, float) and math.isfinite(value)


class Checks:
    def __init__(self):
        self.failures = []

    def require(self, condition, field, message):
        if not condition:
            self.failures.append(f"{field}: {message}")
        return bool(condition)

    def numeric(self, value, field, minimum=0):
        return self.require(number(value) and value >= minimum, field, "missing or invalid number")


def hash_gate(check, value, name, first=None, last=None, cadence=1):
    for key in ("first", "last", "expected", "compared", "missing", "unequal", "invalid", "duplicates"):
        check.require(type(value.get(key)) is int and value[key] >= 0, f"{name}.{key}", "missing or invalid count")
    if not all(type(value.get(k)) is int for k in ("first", "last", "expected", "compared")):
        return
    low, high = value["first"], value["last"]
    check.require(low > 0 and high >= low, name, "empty or reversed hash interval")
    if first is not None:
        check.require(low == first, name, "hash coverage does not begin at the required tick")
    if last is not None:
        check.require(high == last, name, "hash coverage ends at the wrong tick")
    owed = (high-low)//cadence+1
    check.require(value["expected"] == owed == value["compared"], name, "hash samples are missing")
    for key in ("missing", "unequal", "invalid", "duplicates"):
        check.require(value.get(key) == 0, name, f"{key} hash samples")


def peer_gate(check, peers, names, timing=False):
    check.require(bool(names) and len(names) == len(set(names)), "peers", "empty or duplicate peer set")
    for name in names:
        value = peers.get(name, {})
        prefix = f"peers.{name}"
        check.require(value.get("completed") is True, prefix, "native completion missing")
        check.require(value.get("holds") == [], prefix, "holds present or hold receipts missing")
        if not timing:
            continue
        windows = value.get("timing", [])
        check.require(bool(windows), prefix, "timing receipts missing")
        previous = None
        for window in windows:
            low, high, duration = window.get("first"), window.get("last"), window.get("elapsed_ms")
            valid = (type(low) is int and type(high) is int and high > low and
                     number(duration) and duration > 0)
            if not check.require(valid, prefix, "invalid timing window"):
                continue
            check.require(window.get("samples") == high-low+1, prefix, "timing samples are missing")
            check.require(previous is None or low == previous, prefix, "timing window gap or overlap")
            previous = high
            check.require((high-low)*1000/duration >= 59.5, prefix, "round rate below 59.5 tps")
            wait, longest = window.get("wait_ms"), window.get("max_wait_ms")
            check.require(number(wait) and 0 <= wait < duration/100, prefix, "waiting time missing or at least one percent")
            check.require(number(longest) and 0 <= longest <= 50, prefix, "frame wait missing or exceeds 50 ms")
        if windows:
            check.require(windows[0].get("first") == value.get("first") and
                          windows[-1].get("last") == value.get("last"), prefix, "timing omits part of the declared interval")


def tree_gate(check, facts):
    hashes = facts.get("tree_hashes", {})
    check.require(set(hashes) == BOXES, "tree_hashes", "four box hashes missing")
    check.require(all(isinstance(h, str) and re.fullmatch(r"[0-9a-f]{64}", h) for h in hashes.values()),
                  "tree_hashes", "invalid SHA-256")
    check.require(len(set(hashes.values())) == 1, "tree_hashes", "installed mod trees differ")
    check.require(facts.get("module") == "VoidWanderers.rte", "module", "wrong installed module")


def transfer_gate(check, value, prefix="transfer"):
    begin, end = value.get("start_ms"), value.get("end_ms")
    valid = number(begin) and number(end) and 0 <= begin < end
    check.require(valid, prefix, "transfer start/end receipts missing or reversed")
    check.require(type(value.get("received_bytes")) is int and value["received_bytes"] > 0,
                  prefix, "StateChunk byte total missing")
    labels = value.get("labels", [])
    check.require(bool(labels), prefix, "progress label receipts missing")
    if valid and labels:
        times = [r.get("at_ms") for r in labels]
        if check.require(all(number(t) for t in times), prefix, "invalid label time"):
            check.require(times[0] <= begin and times[-1] >= end, prefix, "labels do not cover the transfer")
            check.require(all(0 < b-a <= 5000 for a, b in zip(times, times[1:])), prefix, "progress labels have a gap over five seconds")
        check.require(all(isinstance(r.get("texts"), list) and all(isinstance(t, str) for t in r["texts"])
                          for r in labels), prefix, "label text dump missing")


def named_boxes(check, facts):
    host = facts.get('host_box', 'pc')
    if not check.require(host in ('pc', 'laptop'), 'host_box', 'host is outside the named four-box roster'):
        return set()
    return {host, 'remote', 'mac', 'linux'}


def join_gate(check, facts, boxes=True, progress=True):
    join = facts.get("join", {})
    check.require(join.get("peer") == "remote", "join", "internet late joiner is not REMOTE")
    check.require(number(join.get("host_tick")) and join["host_tick"] >= 1200, "join", "join starts before 1200 host frames")
    check.require(join.get("directory") in ("public-default", "loopback-fallback"), "join", "directory route missing")
    if join.get("directory") == "loopback-fallback":
        check.require(join.get("public_directory_down") is True and join.get("own_certificate") is True,
                      "join", "fallback lacks outage and lane-certificate receipts")
    check.require(join.get("nat_to_nat") is True and join.get("stun") is True,
                  "join", "NAT-to-NAT STUN receipt missing")
    check.require(join.get("route") == "direct", "join", "completed direct route missing")
    low, high = join.get("activation_tick"), join.get("last_tick")
    check.require(type(low) is int and type(high) is int and high > low, "join", "activation or play interval missing")
    hash_gate(check, facts.get("live", {}), "live", low, high)
    if type(low) is int and type(high) is int:
        hash_gate(check, facts.get("fullstate", {}), "fullstate", (low+59)//60*60, high//60*60, 60)
    peers = facts.get("peers", {})
    if boxes:
        check.require(set(peers) == BOXES, "peers", "four real boxes are required")
        check.require({p.get("box") for p in peers.values()} == named_boxes(check, facts), "peers", "machine set differs from the declared host roster")
    seated = [name for name in peers if name != join.get("peer")]
    peer_gate(check, peers, seated, timing=True)
    peer_gate(check, peers, ["remote"])
    if progress:
        transfer_gate(check, facts.get("transfer", {}))
    else:
        value = facts.get('transfer', {}).get('received_bytes')
        check.require(type(value) is int and value > 0, 'transfer', 'StateChunk byte total missing')


def census_gate(check, rows, prefix, duration):
    valid = bool(rows) and all(type(r.get("uptime_ms")) is int and r["uptime_ms"] >= 0 and
                               type(r.get("process_bytes")) is int and r["process_bytes"] > 0 for r in rows)
    if not check.require(valid, prefix, "memory census missing or invalid"):
        return {}
    times = [r["uptime_ms"] for r in rows]
    check.require(all(b > a for a, b in zip(times, times[1:])), prefix, "reversed or duplicate native clocks")
    from feel.report import reduce_memory
    # Use the merged oracle's minute slots, fitted slope and last-minus-first
    # retention. The separate fixed harness also requires its native census rule.
    bounded = reduce_memory([dict(elapsed_s=r['uptime_ms']/1000, private=r['process_bytes']) for r in rows],
                            warmup_s=120, slope_bytes_per_minute=8*MIB, retained_bytes=128*MIB,
                            sample_seconds=60, elapsed_s=duration)
    check.require(not bounded['missing_samples'], prefix, 'minute census samples are missing')
    check.require(bool(bounded['sizes']), prefix, 'post-warmup samples missing')
    for value in bounded['sizes'].values():
        check.require(number(value['slope_bytes_per_minute']) and value['slope_bytes_per_minute'] <= 8*MIB,
                      prefix, 'raw process slope exceeds 8 MiB/min')
        check.require(value['retained_bytes'] <= 128*MIB, prefix, 'raw retained growth exceeds 128 MiB')
    return {**bounded, 'instrument_subtraction': False}


def judge(row, facts):
    if row not in ROWS:
        raise ValueError(f"unknown acceptance row: {row}")
    check = Checks()
    if not isinstance(facts, dict):
        return dict(row=row, passed=False, failures=["facts: object missing"])
    details = {}
    peers = facts.get("peers", {})
    if row in ("mod-match", "mod-refusal"):
        tree_gate(check, facts)
        hash_gate(check, facts.get("live", {}), "live", 1, 1201)
        hash_gate(check, facts.get("fullstate", {}), "fullstate", 60, 1200, 60)
    if row == "mod-match":
        check.require(set(peers) == BOXES and {p.get("box") for p in peers.values()} == named_boxes(check, facts),
                      "peers", "four distinct boxes are required")
        peer_gate(check, peers, sorted(BOXES))
        check.require(bool(facts.get("activity")) and facts["activity"] == facts.get("installed_activity"),
                      "activity", "the installed mod activity was not used")
        for name in BOXES:
            value = peers.get(name, {})
            check.require(value.get("first") == 1 and value.get("last") == 1201,
                          f"peers.{name}", "1201 frames are not covered")
    elif row == "mod-refusal":
        refusal = facts.get("refusal", {})
        check.require(refusal.get("joined") is False, "refusal", "mismatched joiner was admitted or outcome missing")
        check.require(refusal.get("files_changed") == 1 and refusal.get("bytes_changed") == 1,
                      "refusal", "mutation is not exactly one byte of one file")
        before = refusal.get("before")
        check.require(before in facts.get("tree_hashes", {}).values() and before == refusal.get("restored") and
                      before != refusal.get("altered"), "refusal", "scratch-copy restoration hash missing or different")
        check.require(refusal.get("joiner") == refusal.get("altered_box") in BOXES,
                      "refusal", "altered tree is not the refused peer")
        for field in ("log_text", "landing_text"):
            text = refusal.get(field, "").casefold().replace("-", " ")
            named = "module manifest mismatch" in text or "modulemanifestmismatch" in text
            if field == "landing_text":
                named |= any(message in text for message in ("module content hash does not match", "module manifest hash does not match",
                                                              "this host's mods do not match yours."))
            check.require(named, f"refusal.{field}", "named module-manifest refusal missing")
        survivors = refusal.get("survivors", [])
        check.require(set(survivors) == BOXES-{refusal.get("joiner")}, "refusal", "three survivors missing")
        peer_gate(check, peers, survivors)
        for name in survivors:
            check.require(number(refusal.get("refusal_tick")) and number(peers.get(name, {}).get("last")) and
                          peers[name]["last"] > refusal["refusal_tick"], "refusal", "survivor stopped at the refusal")
    elif row == "spectator":
        config, watch = facts.get("configuration", {}), facts.get("watch", {})
        check.require(config.get("seats") == 3 and config.get("world_max_spectators") == 1 and config.get('persistent_world') is True,
                      "configuration", "three seats and one spectator were not configured")
        seated, spectator = facts.get("seated", []), facts.get("spectator")
        check.require(len(seated) == 3 and spectator in peers and spectator not in seated,
                      "spectator", "fourth peer is not an unseated spectator")
        peer_gate(check, peers, seated, timing=True)
        check.require(watch.get("role") == "Spectator" and watch.get("image_received") is True,
                      "watch", "spectator image/role receipts missing")
        hash_gate(check, watch.get("hashes", {}), "watch.hashes", watch.get("first"), watch.get("last"))
        cost = facts.get("throttle", {})
        clock_box = facts.get('clock_box', 'pc')
        if 'clock_brackets' in facts:
            from acceptance_clock_brackets import errors as clock_errors
            check.failures.extend(clock_errors(facts))
        else:
            check.require(clock_box in ('pc','remote') and all(peers.get(name,{}).get('box') == clock_box for name in [*seated,spectator]),
                          'throttle', 'crawl and seated timing must share their actual native clock domain')
        check.require(cost.get("process") == spectator and number(cost.get("sim_cost_us")) and cost["sim_cost_us"] >= 100000,
                      "throttle", "crawl cost was not applied to the spectator")
        check.require(number(cost.get("start_ms")) and number(cost.get("end_ms")) and cost["end_ms"]-cost["start_ms"] >= 30000,
                      "throttle", "spectator crawl lasted less than 30 seconds")
        if 'clock_brackets' not in facts:
            for name in seated:
                value = peers.get(name, {})
                check.require(number(value.get("first_wall_ms")) and number(value.get("last_wall_ms")) and
                              number(cost.get("start_ms")) and number(cost.get("end_ms")) and
                              value["first_wall_ms"] <= cost["start_ms"] < cost["end_ms"] <= value["last_wall_ms"],
                              "throttle", f"{name} timing does not cover the crawl in the native steady-clock domain")
        check.require(all(type(value) is int for value in (watch.get("first"), watch.get("last"), cost.get("first_tick"), cost.get("last_tick"))) and
                      watch["first"] <= cost["first_tick"] <= cost["last_tick"] <= watch["last"],
                      "watch", "spectator hash comparison does not cover its crawl ticks")
        promotion = facts.get("promotion", {})
        check.require(promotion.get("host_authorized") is True, "promotion", "host release/reassignment missing")
        for field in ("seat", "actor", "ticket_incarnation", "activation_tick", "input_tick", "input_created_tick"):
            check.require(type(promotion.get(field)) is int and promotion[field] > 0,
                          f"promotion.{field}", "ownership/input receipt missing")
        for field, source in (("seat", "freed_seat"), ("applied_seat", "seat"), ("applied_actor", "actor"),
                              ("applied_incarnation", "ticket_incarnation")):
            check.require(promotion.get(field) is not None and promotion.get(field) == promotion.get(source),
                          f"promotion.{field}", "promoted ownership and applied input disagree")
        activation = promotion.get("activation_tick")
        check.require(type(activation) is int and type(cost.get("last_tick")) is int and activation > cost["last_tick"],
                      "promotion", "spectator was promoted before its crawl ended")
        check.require(type(activation) is int and type(promotion.get("input_tick")) is int and
                      type(promotion.get("input_created_tick")) is int and
                      activation < promotion["input_created_tick"] <= promotion["input_tick"] and
                      bool(promotion.get("applied_input")), "promotion", "no fresh controllable input after promotion")
    elif row in ("world-join", "world-soak"):
        join_gate(check, facts, boxes=row == "world-join", progress=row == "world-join")
        if row == "world-soak":
            soak = facts.get("soak", {})
            duration, late = soak.get("elapsed_s"), soak.get("late_join_elapsed_s")
            check.require(number(duration) and duration >= 3600, "soak", "run is shorter than 60 minutes")
            check.require(number(late) and 3000 <= late < 3060, "soak", "late join was not at minute 50")
            check.require(soak.get("autosave_seconds") == 60 and soak.get("fullstate_every") == 60 and
                          soak.get("census_every_s") == 60, "soak", "autosave, hash or census cadence missing")
            check.require(set(peers) == {"pc", "remote-first", "remote"}, "peers", "world host and the two REMOTE seats are not covered")
            activation = facts.get('join', {}).get('activation_tick')
            if type(activation) is int:
                hash_gate(check, facts.get('initial_live', {}), 'initial_live', 1, activation-1)
                hash_gate(check, facts.get('initial_fullstate', {}), 'initial_fullstate', 60, (activation-1)//60*60, 60)
            details["memory"] = {}
            if number(duration) and number(late):
                for name, seconds in (("pc", duration), ("remote-first", duration), ("remote", duration-late)):
                    details["memory"][name] = census_gate(check, facts.get("census", {}).get(name, []), f"census.{name}", seconds)
            journal = soak.get("journal", [])
            check.require([r.get("minute") for r in journal] == [10, 30, 50, 60] and
                          all(type(r.get("bytes")) is int and r["bytes"] >= 0 for r in journal),
                          "journal", "10/30/50/60 minute sizes missing")
            details["journal"] = journal
            details["journal_verdict"] = "RED L02: pruning proof outstanding"
            if soak.get("journal_pruning") == "landed":
                bound = soak.get("journal_bound_bytes")
                if check.numeric(bound, "journal_bound_bytes", 1):
                    check.require(all(r.get("bytes", math.inf) <= bound for r in journal), "journal", "pruning bound exceeded")
                    details["journal_verdict"] = "PASS" if all(r.get("bytes", math.inf) <= bound for r in journal) else "FAIL"
    elif row == "image-sizes":
        build = facts.get("build", {})
        check.require(build.get("configuration") == "Final" and build.get("sanitizer") is False,
                      "build", "release Final build receipt missing")
        offered, rows = facts.get("offered_scenes", []), facts.get("scenes", [])
        names = [r.get("name") for r in rows]
        check.require(bool(offered) and len(offered) == len(set(offered)) and
                      sorted(names) == sorted(offered), "scenes", "table differs from world lobby scene list")
        details["sizes"] = []
        for scene in rows:
            prefix = f"scenes.{scene.get('name')}"
            archive, stream = scene.get("archive_bytes"), scene.get("received_bytes")
            check.require(type(archive) is int and archive > 0 and type(stream) is int and stream > 0,
                          prefix, "archive or received byte total missing")
            check.require(scene.get("capture_count") == 1, prefix, "not exactly one checkpoint capture")
            transfer_gate(check, scene.get("transfer", {}), prefix)
            if type(archive) is int and archive > 0 and type(stream) is int and stream > 0:
                details["sizes"].append(dict(name=scene["name"], archive_bytes=archive, received_bytes=stream,
                                              ratio=stream/archive, compresses=stream < archive))
    return dict(row=row, passed=not check.failures, failures=check.failures, **details)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("row", choices=ROWS)
    parser.add_argument("receipts", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    result = judge(args.row, json.loads(args.receipts.read_text(encoding="utf-8-sig")))
    args.out.write_text(json.dumps(result, indent=2, allow_nan=False)+"\n", encoding="utf-8")
    print(f"[{args.row}] {'PASS' if result['passed'] else 'FAIL'}")
    for failure in result["failures"]:
        print(failure)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
