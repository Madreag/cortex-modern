"""Compare unpaused sim ticks only. Free-run rows keep compare_e2e_simgated.py."""
import argparse
import json
import re
from pathlib import Path

HASH = re.compile(r"[0-9a-f]{64}\Z")
CORE = frozenset({"actors", "terrain", "sim_rng", "scene", "funds", "lua_state", "rot_angle", "rot_angvel", "tick"})


def load_active(path):
    data = json.loads(Path(path).read_text(encoding="utf-8-sig"))
    runs = data.get("runs") if isinstance(data, dict) else None
    if not isinstance(runs, list) or len(runs) != 1 or not isinstance(runs[0], dict):
        raise ValueError("expected exactly one run")
    entries = runs[0].get("tick_hashes")
    if not isinstance(entries, list) or not entries:
        raise ValueError("missing or empty tick_hashes")
    active = {}
    paused_n = 0
    for entry in entries:
        if not isinstance(entry, dict):
            raise ValueError("tick record must be an object")
        tick = entry.get("tick")
        if type(tick) is not int or tick < 1:
            raise ValueError(f"invalid tick {tick!r}")
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
            paused_n += 1
            continue
        if not CORE <= subs.keys():
            raise ValueError(f"tick {tick}: core subsystems missing {sorted(CORE - subs.keys())}")
        active[tick] = subs
    return active, paused_n


def compare_active(host, client, min_ticks=1):
    reasons = []
    try:
        ht, hp = load_active(host)
    except (OSError, ValueError, TypeError) as exc:
        reasons.append(f"host: {exc}")
        ht, hp = {}, 0
    try:
        ct, cp = load_active(client)
    except (OSError, ValueError, TypeError) as exc:
        reasons.append(f"client: {exc}")
        ct, cp = {}, 0
    if reasons:
        return False, {"compared_ticks": 0, "paused_ticks": 0, "reasons": reasons}
    if set(ht) != set(ct):
        reasons.append(
            f"unpaused tick sets differ host={len(ht)} client={len(ct)} "
            f"only_host={sorted(set(ht) - set(ct))[:8]} only_client={sorted(set(ct) - set(ht))[:8]}"
        )
        return False, {"compared_ticks": 0, "paused_ticks": hp + cp, "first_divergence": None, "reasons": reasons}
    overlap = sorted(ht)
    compared = 0
    first = None
    for tick in overlap:
        diff = sorted(
            k for k in ht[tick].keys() | ct[tick].keys()
            if k != "controller" and (k not in ht[tick] or k not in ct[tick] or ht[tick][k] != ct[tick][k])
        )
        if diff:
            first = tick
            reasons.append(f"tick {tick}: on-wire divergence in {diff}")
            break
        compared += 1
    if compared < min_ticks:
        reasons.append(f"overlapping unpaused ticks {compared} < {min_ticks}")
    return not reasons, {
        "compared_ticks": compared,
        "paused_ticks": hp + cp,
        "first_divergence": first,
        "reasons": reasons,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", type=Path)
    parser.add_argument("client", type=Path)
    parser.add_argument("--min-ticks", type=int, default=1)
    parser.add_argument("--expected-ticks", type=int, default=None)
    args = parser.parse_args()
    need = args.min_ticks if args.expected_ticks is None else args.expected_ticks
    passed, result = compare_active(args.host, args.client, need)
    if passed:
        print(
            f"PASS sim-gated: {result['compared_ticks']} overlapping ticks identical "
            f"(controller excluded); paused_ticks={result['paused_ticks']}"
        )
    else:
        print("FAIL: " + "; ".join(result["reasons"]))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
