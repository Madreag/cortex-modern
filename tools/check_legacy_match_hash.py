"""Compare a baseline report's config hash with the pinned v2 source algorithm."""

import argparse
import hashlib
import json
from pathlib import Path
import struct


def fields(config):
    result = [(key, str(config[key])) for key in (
        "version", "session_id", "host_peer_id", "peer_count", "input_delay_frames",
        "mode", "ownership_policy", "activity_type", "activity_preset", "scene_name", "mode_preset",
    )]
    result.append(("peer_input_delays", ",".join(map(str, config["peer_input_delays"]))))
    for player in config["players"]:
        prefix = f"player.{player['peer_id']}."
        result.extend(((prefix + "team", str(player["team"])),
                       (prefix + "cpu", "1" if player["cpu"] else "0"),
                       (prefix + "display_name", player["display_name"])))
    if config["dedicated"]:
        result.append(("dedicated", "true"))
    return sorted(result)


def source_hash(config):
    if config["version"] != 2:
        raise ValueError("expected baseline config version 2")
    def line(key, value):
        return f"S {key} {len(value.encode('utf-8'))}:{value}\n"
    canonical = "NetIdentityCanonicalText/v1\n" + line("domain", "NetMatchConfig/v2")
    canonical += "".join(line(key, value) for key, value in fields(config))
    mask = (1 << 64) - 1
    state = 0xCBF29CE484222325
    for byte in canonical.encode("utf-8"):
        state = ((state ^ byte) * 0x100000001B3) & mask
    result = b""
    for _ in range(4):
        state = (state + 0x9E3779B97F4A7C15) & mask
        value = ((state ^ (state >> 30)) * 0xBF58476D1CE4E5B9) & mask
        value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & mask
        result += struct.pack("<Q", value ^ (value >> 31))
    return result.hex()


def configs(value):
    if isinstance(value, dict):
        if "match_config_hash" in value and "players" in value and "version" in value:
            yield value
        for child in value.values():
            yield from configs(child)
    elif isinstance(value, list):
        for child in value:
            yield from configs(child)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--exe-sha256", required=True)
    options = parser.parse_args()
    report_bytes = options.report.read_bytes()
    fixture = dict(version=2, session_id=1, host_peer_id=1, peer_count=2, dedicated=False,
        input_delay_frames=0, peer_input_delays=[], mode="pvp-skirmish", ownership_policy="team-owner",
        activity_type="GAScripted", activity_preset="Skirmish Defense", scene_name="Grasslands", mode_preset="PvP",
        players=[dict(peer_id=1, team=0, cpu=False, display_name="Host"), dict(peer_id=2, team=1, cpu=False, display_name="Client")])
    records = []
    for config in configs(json.loads(report_bytes)):
        computed = source_hash(config)
        observed = config["match_config_hash"]
        exact = fields(config) == fields(fixture)
        records.append(dict(baseline_print=observed, source_algorithm=computed, exact_fixture=exact, equal=observed == computed))
        print(f"BASELINE_HASH printed={observed} source_algorithm={computed} exact_fixture={str(exact).lower()}")
    result = dict(report=str(options.report), report_sha256=hashlib.sha256(report_bytes).hexdigest(),
        exe_sha256=options.exe_sha256, records=records, fixture_source_hash=source_hash(fixture),
        exact_fixture_observed=any(row["exact_fixture"] for row in records))
    result["pass"] = bool(records) and all(row["equal"] for row in records)
    options.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
