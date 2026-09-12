"""Validate and compare complete controller-sync trace ranges."""

import argparse
import json
from pathlib import Path
import re
import sys

CORE = frozenset({"actors", "terrain", "sim_rng", "scene", "funds", "lua_state", "rot_angle", "rot_angvel", "tick"})
HASH = re.compile(r"[0-9a-f]{64}\Z")


def load_trace(path):
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
        if previous is not None and tick != previous + 1:
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
            if set(subs) != {"tick", "terrain"}:
                raise ValueError(f"tick {tick}: unexpected paused-tick schema")
        elif not CORE <= subs.keys():
            raise ValueError(f"tick {tick}: core subsystems missing {sorted(CORE - subs.keys())}")
        ticks[tick] = (subs, paused)
    return ticks, {k: v for k, v in runs[0].items() if k != "tick_hashes"}


def strict_compare(host, client, expected_ticks=None, *, first_tick=1, min_ticks=1, prefix=False):
    result = {"compared_ticks": 0, "first_divergence": None, "divergent_subsystems": [], "reasons": [], "paused_ticks": 0}
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
            ticks, info = load_trace(path)
        except (OSError, ValueError, TypeError) as exc:
            result["reasons"].append(f"{label}: {exc}")
            continue
        traces.append(ticks)
        result[f"{label}_info"] = {"run": info}
        if next(iter(ticks)) != first_tick:
            result["reasons"].append(f"{label}: first tick is {next(iter(ticks))}, expected {first_tick}")
        if expected_ticks is not None and (len(ticks) < expected_ticks if prefix else len(ticks) != expected_ticks):
            kind = "at least" if prefix else "exactly"
            if len(ticks) < expected_ticks:
                result["reasons"].append(f"{label}: too few ticks ({len(ticks)}, expected {kind} {expected_ticks})")
            else:
                result["reasons"].append(f"{label}: has {len(ticks)} ticks, expected {kind} {expected_ticks}")
        if len(ticks) < min_ticks:
            result["reasons"].append(f"{label}: too few ticks ({len(ticks)}, minimum is {min_ticks})")
    if result["reasons"]:
        return False, result
    ht, ct = traces
    if prefix:
        ht = {t: v for t, v in ht.items() if t < first_tick + expected_ticks}
        ct = {t: v for t, v in ct.items() if t < first_tick + expected_ticks}
    if ht.keys() != ct.keys():
        result["reasons"].append("host and client tick ranges differ")
        return False, result
    schemas = set()
    for tick in ht:
        hs, hp = ht[tick]
        cs, cp = ct[tick]
        schemas.update((tuple(sorted(hs)), tuple(sorted(cs))))
        diff = sorted(k for k in hs.keys() | cs.keys() if k != "controller" and (k not in hs or k not in cs or hs[k] != cs[k]))
        if diff or hp != cp:
            result["first_divergence"] = tick
            result["divergent_subsystems"] = diff
            result["reasons"].append(f"tick {tick}: on-wire divergence in {diff}" if diff else f"tick {tick}: pause state differs")
            break
        result["compared_ticks"] += 1
        result["paused_ticks"] += int(hp)
    result["subsystem_sets_seen"] = [list(s) for s in sorted(schemas)]
    result["coverage"] = "active core hashes; paused ticks cover tick and terrain only"
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
    args = parser.parse_args()
    passed, result = strict_compare(args.host, args.client, args.prefix_ticks or args.expected_ticks, first_tick=args.first_tick, min_ticks=args.min_ticks, prefix=args.prefix_ticks is not None)
    if args.json:
        args.json.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if passed:
        print(f"PASS sim-gated: {result['compared_ticks']} overlapping ticks identical (controller excluded); paused_ticks={result['paused_ticks']}")
    else:
        print("FAIL: " + "; ".join(result["reasons"]), file=sys.stderr)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
