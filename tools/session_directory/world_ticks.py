"""Compare world tick receipts across live play and private replay."""

import json
from pathlib import Path


def read_world_ticks(directory: Path) -> dict[str, dict[int, str]]:
    phases: dict[str, dict[int, str]] = {"live": {}, "catchup": {}}
    recorded: dict[int, str] = {}
    for path in sorted(directory.glob("live.jsonl*")):
        for number, line in enumerate(path.open(encoding="utf-8"), 1):
            item = json.loads(line)
            if not isinstance(item, dict):
                raise ValueError(f"invalid world receipt at {path.name}:{number}")
            if "tick" not in item:
                continue
            tick, phase, value = item["tick"], item.get("phase"), item.get("sim_gated")
            if type(tick) is not int or tick < 0 or phase not in phases or not isinstance(value, str) or len(value) != 64:
                raise ValueError(f"invalid world tick at {path.name}:{number}")
            if tick in recorded and recorded[tick] != value:
                raise ValueError(f"contradictory world tick {tick}")
            recorded[tick] = value
            phases[phase][tick] = value
    return phases


def compare_world_ticks(host: dict[str, dict[int, str]], peer: dict[str, dict[int, str]]) -> dict:
    authority, live, replay = host["live"], peer["live"], peer["catchup"]
    recorded = {**replay, **live}
    first, last = (min(live), max(live)) if live else (0, 0)
    stats = {
        "compared": len(live),
        "unequal": sum(tick in authority and value != authority[tick] for tick, value in live.items()),
        "absent": sum(tick not in authority for tick in live),
        "conflicts": 0,
        "compared_catchup": len(replay),
        "catchup_unequal": sum(tick in authority and value != authority[tick] for tick, value in replay.items()),
        "catchup_absent": sum(tick not in authority for tick in replay),
        # A private return records these ticks as replay, while the host keeps playing.
        "live_only_holes": sum(tick not in live for tick in range(first, last + 1)) if live else 0,
        "missing_in_span": sum(tick not in recorded for tick in range(first, last + 1)) if live else 0,
        "host_missing_in_span": len(range(min(authority), max(authority) + 1)) - len(authority) if authority else 0,
    }
    stats["pass"] = bool(authority and live) and all(stats[key] == 0 for key in
        ("unequal", "absent", "conflicts", "catchup_unequal", "catchup_absent", "missing_in_span", "host_missing_in_span"))
    return stats
