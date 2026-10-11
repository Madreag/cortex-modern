"""Check the complete engine-written history of a running AI fight."""

import json
from pathlib import Path

from compare_sim_traces import CORE
from feel.retained_resume import PER_PEER_SUBSYSTEMS


def compare_fight_hashes(root, spec):
    errors, histories, coverage = [], {}, {}
    required = CORE | {"controller", "scene", "terrain", "sim_rng", "lua_state"}
    try:
        for peer in spec["peers"]:
            path = Path(root) / f"{peer}-stage/probe/ticks.jsonl"
            history, rounds, longest, streak = {}, set(), 0, 0
            with path.open(encoding="utf-8") as stream:
                for line in stream:
                    if not line.strip():
                        continue
                    row = json.loads(line)
                    key = (row["round"], row["tick"])
                    if key in history:
                        errors.append(f"{peer}: duplicate round/tick {key}")
                    history[key] = row
                    rounds.add(row["round"])
                    if row.get("phase") != "live" or not isinstance(row.get("paused"), bool):
                        errors.append(f"{peer}: missing live state at tick {row['tick']}")
                    streak = streak + 1 if row.get("paused") is False else 0
                    longest = max(longest, streak)
                    if not required <= row.get("subsystems", {}).keys() or not row.get("sim_gated"):
                        errors.append(f"{peer}: incomplete state hash at tick {row['tick']}")
            ticks = {tick for _, tick in history}
            last = max(ticks, default=0)
            if len(rounds) != 1 or None in rounds:
                errors.append(f"{peer}: expected one uninterrupted round, found {sorted(map(str, rounds))}")
            if ticks != set(range(1, last + 1)) or not ticks:
                errors.append(f"{peer}: incomplete tick coverage")
            if longest < spec["running_ticks"]:
                errors.append(f"{peer}: only {longest} consecutive running ticks; need {spec['running_ticks']}")
            coverage[peer] = {"rows": len(history), "last_tick": last, "running_ticks": longest}
            histories[peer] = history

        reference = spec["peers"][0]
        first_difference = None
        for peer in spec["peers"][1:]:
            if histories[peer].keys() != histories[reference].keys():
                errors.append(f"{peer}: tick coverage differs from {reference}")
            for key in sorted(histories[peer].keys() & histories[reference].keys()):
                a, b = histories[reference][key], histories[peer][key]
                shared = lambda row: {name: value for name, value in row["subsystems"].items() if name not in PER_PEER_SUBSYSTEMS}
                a_shared, b_shared = shared(a), shared(b)
                if a_shared != b_shared or a["sim_gated"] != b["sim_gated"] or a["paused"] != b["paused"]:
                    components = [name for name in sorted(a_shared.keys() | b_shared.keys()) if a_shared.get(name) != b_shared.get(name)]
                    first_difference = {"peer": peer, "round": key[0], "tick": key[1], "components": components}
                    errors.append(f"{peer}: state differs at tick {key[1]}")
                    break
        return {"status": "FAIL" if errors else "PASS", "errors": errors, "coverage": coverage,
                "required_running_ticks": spec["running_ticks"], "first_difference": first_difference,
                "excluded_components": sorted(PER_PEER_SUBSYSTEMS)}
    except (OSError, ValueError, KeyError, TypeError, IndexError) as error:
        return {"status": "FAIL", "errors": [*errors, str(error)], "coverage": coverage,
                "required_running_ticks": spec["running_ticks"]}
