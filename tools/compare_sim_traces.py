"""Validate and compare complete controller-sync trace ranges."""

import argparse
import json
from pathlib import Path
import re
import sys

CORE = frozenset({"actors", "terrain", "sim_rng", "scene", "funds", "lua_state", "rot_angle", "rot_angvel", "tick"})
# What a held tick must hash: the state a paused sim can still move. The per-object subsystems join it
# whenever the world holds objects, and the comparison covers every subsystem both peers reported.
PAUSED_CORE = frozenset({"tick", "terrain", "sim_rng", "scene", "funds", "lua_state"})
HASH = re.compile(r"[0-9a-f]{64}\Z")
# The full-state oracle (-net-fullstate-hash-every): a hash per shared section of the capture, in the capture's order.
# The engine leaves the per-peer sections out of the line by the capture's own section table, so every listed one is compared.
FULLSTATE = re.compile(r"^\[fullstate\] tick=(\d+) hash=([0-9a-f]{16}) sections=(\S+) round=(\d+)\s*$", re.M)


def load_fullstate(paths):
    """Every oracle sample in one peer's logs, keyed by (round, tick). A tick sampled twice (a rewind) keeps both."""
    samples = {}
    for path in [paths] if isinstance(paths, (str, Path)) else paths:
        path = Path(path)
        text = path.read_text(encoding="utf-8-sig", errors="replace") if path.is_file() else ""
        for tick, total, sections, round_id in FULLSTATE.findall(text):
            parsed = [tuple(item.rsplit(":", 1)) for item in sections.split(",")]
            samples.setdefault((int(round_id), int(tick)), []).append((total, parsed))
    return samples


def compare_fullstate(host_logs, client_logs):
    """The oracle's verdict over every (round, tick) both peers sampled: each client sample must equal each host sample
    of that tick section for section. The first differing tick and the first differing section in the capture's order
    are named; a pair with no sample in common is not a pass."""
    host, client = load_fullstate(host_logs), load_fullstate(client_logs)
    common = sorted(host.keys() & client.keys())
    result = {"host_samples": sum(map(len, host.values())), "client_samples": sum(map(len, client.values())),
              "compared_samples": 0, "first_divergence": None, "divergent_ticks": 0, "reasons": []}
    for key in common:
        differing = []
        for client_total, client_sections in client[key]:
            for host_total, host_sections in host[key]:
                result["compared_samples"] += 1
                if client_total == host_total and client_sections == host_sections:
                    continue
                client_map = dict(client_sections)
                order = [name for name, _ in host_sections] + [name for name, _ in client_sections if name not in dict(host_sections)]
                differing += [name for name in order if dict(host_sections).get(name) != client_map.get(name) and name not in differing]
        if differing:
            result["divergent_ticks"] += 1
            if result["first_divergence"] is None:
                result["first_divergence"] = {"round": key[0], "tick": key[1], "section": differing[0], "sections": differing}
    if not common:
        result["reasons"].append("the peers share no full-state sample")
    elif result["first_divergence"]:
        first = result["first_divergence"]
        result["reasons"].append(f"full state differs at round {first['round']} tick {first['tick']}: first section {first['section']} "
                                 f"of {len(first['sections'])} {first['sections']}")
    result["sampled_ticks"] = [tick for _, tick in common]
    result["passed"] = not result["reasons"]
    return result


def load_trace(path, away=frozenset(), rewinds=frozenset(), repeats=None):
    """away: ticks a held client never simulated, so its trace may jump over them. rewinds: the images a held client
    reloaded that were older than its stop, so its trace may jump back once to each image's next tick; the readings
    of ticks it simulates a second time go to `repeats` as (tick, subsystems, paused)."""
    unused_rewinds = set(rewinds)
    data = json.loads(Path(path).read_text(encoding="utf-8-sig"))
    runs = data.get("runs") if isinstance(data, dict) else None
    if not isinstance(runs, list) or len(runs) != 1 or not isinstance(runs[0], dict):
        raise ValueError("expected exactly one run")
    entries = runs[0].get("tick_hashes")
    if not isinstance(entries, list) or not entries:
        raise ValueError("missing or empty tick_hashes")
    ticks = {}
    previous = None
    for entry in entries:
        if not isinstance(entry, dict):
            raise ValueError("tick record must be an object")
        tick = entry.get("tick")
        if type(tick) is not int or tick < 1:
            raise ValueError(f"invalid tick {tick!r}")
        rewound = previous is not None and tick <= previous and tick - 1 in unused_rewinds
        if rewound:
            unused_rewinds.discard(tick - 1)
        if previous is not None and tick != previous + 1 and not rewound and not (tick > previous + 1 and set(range(previous + 1, tick)) <= away):
            raise ValueError(f"tick {tick} does not follow {previous} (gap, duplicate or reordering)")
        previous = tick
        paused = entry.get("paused", False)
        if type(paused) is not bool:
            raise ValueError(f"tick {tick}: paused must be a boolean")
        subs = entry.get("subsystems")
        if not isinstance(subs, dict) or not subs:
            raise ValueError(f"tick {tick}: missing subsystems")
        if any(not isinstance(value, str) or not HASH.fullmatch(value) for value in subs.values()):
            raise ValueError(f"tick {tick}: invalid subsystem hash")
        if not isinstance(entry.get("total"), str) or not HASH.fullmatch(entry["total"]):
            raise ValueError(f"tick {tick}: invalid total hash")
        if paused:
            # A held tick applies commands and runs the activity, so it hashes the same end-of-tick census a
            # simulated one does; the per-object subsystems are in it only when the world holds objects.
            if not PAUSED_CORE <= subs.keys():
                raise ValueError(f"tick {tick}: paused tick is missing {sorted(PAUSED_CORE - subs.keys())}")
        elif not CORE <= subs.keys():
            raise ValueError(f"tick {tick}: core subsystems missing {sorted(CORE - subs.keys())}")
        if tick in ticks:
            if repeats is not None:
                repeats.append((tick, subs, paused))
            continue
        ticks[tick] = (subs, paused)
    return ticks, {k: v for k, v in runs[0].items() if k != "tick_hashes"}


def strict_compare(host, client, expected_ticks=None, *, first_tick=1, min_ticks=1, prefix=False, per_peer=frozenset(), client_away=(),
                   client_rewinds=()):
    """per_peer names the subsystems that are per-machine by construction; only a cross-peer caller passes them
    (tools/feel/retained_resume.py PER_PEER_SUBSYSTEMS), and the comparison of two runs of one peer keeps them.
    client_away names inclusive tick ranges a held client never simulated: its coverage starts again at the image it
    loaded, and every tick it did simulate is compared. client_rewinds names the images older than its stop that it
    replayed from: each tick it simulated twice is compared twice."""
    away = {tick for low, high in client_away for tick in range(low, high + 1)}
    replayed = []
    result = {"compared_ticks": 0, "first_divergence": None, "divergent_subsystems": [], "reasons": [], "paused_ticks": 0,
              "per_peer_excluded": sorted(per_peer)}
    if type(first_tick) is not int or first_tick < 1 or type(min_ticks) is not int or min_ticks < 1:
        result["reasons"].append("first_tick and min_ticks must be positive integers")
        return False, result
    if expected_ticks is not None and (type(expected_ticks) is not int or expected_ticks < 1):
        result["reasons"].append("expected_ticks must be a positive integer")
        return False, result
    if prefix and expected_ticks is None:
        result["reasons"].append("prefix comparison requires an explicit tick count")
        return False, result
    traces = []
    for label, path in (("host", host), ("client", client)):
        try:
            if label == "client":
                ticks, info = load_trace(path, away, frozenset(client_rewinds), replayed)
            else:
                ticks, info = load_trace(path)
        except (OSError, ValueError, TypeError) as exc:
            result["reasons"].append(f"{label}: {exc}")
            continue
        traces.append(ticks)
        result[f"{label}_info"] = {"run": info}
        if next(iter(ticks)) != first_tick:
            result["reasons"].append(f"{label}: first tick is {next(iter(ticks))}, expected {first_tick}")
        expected_here = expected_ticks
        if expected_ticks is not None and label == "client":
            expected_here = expected_ticks - len(away & set(range(first_tick, first_tick + expected_ticks)))
        if expected_ticks is not None and (len(ticks) < expected_here if prefix else len(ticks) != expected_here):
            kind = "at least" if prefix else "exactly"
            if len(ticks) < expected_here:
                result["reasons"].append(f"{label}: too few ticks ({len(ticks)}, expected {kind} {expected_here})")
            else:
                result["reasons"].append(f"{label}: has {len(ticks)} ticks, expected {kind} {expected_here}")
        if len(ticks) < min_ticks:
            result["reasons"].append(f"{label}: too few ticks ({len(ticks)}, minimum is {min_ticks})")
    if result["reasons"]:
        return False, result
    ht, ct = traces
    if prefix:
        ht = {t: v for t, v in ht.items() if t < first_tick + expected_ticks}
        ct = {t: v for t, v in ct.items() if t < first_tick + expected_ticks}
    if ht.keys() - away != ct.keys():
        result["reasons"].append("host and client tick ranges differ")
        return False, result
    result["client_away_ticks"] = len(away & ht.keys())
    schemas = set()
    for tick in ct:
        hs, hp = ht[tick]
        cs, cp = ct[tick]
        schemas.update((tuple(sorted(hs)), tuple(sorted(cs))))
        diff = sorted(k for k in hs.keys() | cs.keys() if k != "controller" and k not in per_peer and (k not in hs or k not in cs or hs[k] != cs[k]))
        if diff or hp != cp:
            result["first_divergence"] = tick
            result["divergent_subsystems"] = diff
            result["reasons"].append(f"tick {tick}: on-wire divergence in {diff}" if diff else f"tick {tick}: pause state differs")
            break
        result["compared_ticks"] += 1
        result["paused_ticks"] += int(hp)
    for tick, cs, cp in replayed:
        if result["reasons"] or tick not in ht:
            continue
        hs, hp = ht[tick]
        diff = sorted(k for k in hs.keys() | cs.keys() if k != "controller" and k not in per_peer and (k not in hs or k not in cs or hs[k] != cs[k]))
        if diff or hp != cp:
            result["first_divergence"] = tick
            result["divergent_subsystems"] = diff
            result["reasons"].append(f"replayed tick {tick}: on-wire divergence in {diff}" if diff else f"replayed tick {tick}: pause state differs")
            break
    result["client_replayed_ticks"] = sum(1 for tick, _, _ in replayed if tick in ht)
    result["subsystem_sets_seen"] = [list(s) for s in sorted(schemas)]
    result["coverage"] = "active core hashes; paused ticks cover the same census, per-object subsystems only when the world holds objects"
    return not result["reasons"], result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", type=Path)
    parser.add_argument("client", type=Path)
    scope = parser.add_mutually_exclusive_group()
    scope.add_argument("--expected-ticks", type=int)
    scope.add_argument("--prefix-ticks", type=int, help="compare an explicit complete prefix for a departing peer")
    parser.add_argument("--first-tick", type=int, default=1)
    parser.add_argument("--min-ticks", type=int, default=1)
    parser.add_argument("--json", type=Path)
    parser.add_argument("--fullstate", nargs=2, type=Path, metavar=("HOST_LOG", "CLIENT_LOG"),
                        help="also compare the [fullstate] samples of the two peers' stdout logs")
    args = parser.parse_args()
    passed, result = strict_compare(args.host, args.client, args.prefix_ticks or args.expected_ticks, first_tick=args.first_tick, min_ticks=args.min_ticks, prefix=args.prefix_ticks is not None)
    if args.fullstate:
        result["fullstate"] = compare_fullstate(*args.fullstate)
        passed = passed and result["fullstate"]["passed"]
    if args.json:
        args.json.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if passed:
        print(f"PASS sim-gated: {result['compared_ticks']} overlapping ticks identical (controller excluded); paused_ticks={result['paused_ticks']}")
        if args.fullstate:
            print(f"PASS full state: {result['fullstate']['compared_samples']} samples identical at ticks {result['fullstate']['sampled_ticks']}")
    else:
        print("FAIL: " + "; ".join(result["reasons"] + result.get("fullstate", {}).get("reasons", [])), file=sys.stderr)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
