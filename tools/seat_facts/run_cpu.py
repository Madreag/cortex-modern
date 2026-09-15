"""Observe roster CPU facts and stock-script CPU assignment on two peers and replay."""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import sys

from run_pair import REPO, sha, stamp

HERE = Path(__file__).resolve().parent
FACTS = re.compile(r"\[cpu-facts\] phase=(\w+) tick=(\d+) state=(-?\d+) running=([01]) humans=(\d+) legacy=(-?\d+) aiTeams=([\d,]+|none) directorTeams=([\d,]+|none) flags=([\d:,]+|none)")
NATIVE = re.compile(r"\[e2e\] TeamIsCPU team=(\d+) value=([01])")
WRITE = re.compile(r"\[cpu-write\] derived=(-?\d+) actual=(-?\d+)")
ARMS = {"p4": ("CPU Facts", "CpuFacts", "P4 Alpha Duel", "coop-pve", 1),
        "stock": ("CPU Stock Facts", "CpuStockFacts", "Skirmish Defense", "coop-pve", 1),
        "script": ("CPU Write Facts", "CpuWriteFacts", "P4 Alpha Duel", "pvpve", 2)}
FATAL = re.compile(r"ERROR:|RTE Assert|RTE Abort|stack traceback|Lua Error", re.I)


def parse(log):
    result = []
    for phase, tick, state, running, humans, legacy, ai, director, flags in FACTS.findall(log):
        result.append({"phase": phase, "tick": int(tick), "state": int(state), "running": int(running), "humans": int(humans), "legacy": int(legacy),
            "aiTeams": [] if ai == "none" else list(map(int, ai.split(","))),
            "directorTeams": [] if director == "none" else list(map(int, director.split(","))),
            "flags": [] if flags == "none" else [list(map(int, value.split(":"))) for value in flags.split(",")]})
    return result


def score(logs, arm):
    cpu = ARMS[arm][4]
    rows = {peer: parse(log) for peer, log in logs.items()}
    expected_flags = [[team, int(team == cpu)] for team in range(cpu + 1)]
    checks = {"both_peers": set(logs) == {"host", "client"}}
    launchable = set(logs) == {"host", "client"}
    for peer, values in rows.items():
        phases = {phase: [row for row in values if row["phase"] == phase] for phase in ("roster", "started", "first_tick")}
        checks[peer + "_complete_observations"] = all(len(rows) == 1 for rows in phases.values()) and len(values) == 3
        started = phases["started"]
        launchable = launchable and len(started) == 1 and started[0]["running"] == 1
        checks[peer + "_native_roster_prints"] = [list(map(int, row)) for row in NATIVE.findall(logs[peer])] == expected_flags
        for phase in ("roster", "first_tick"):
            selected = phases[phase]
            checks[peer + "_" + phase + "_cpu"] = len(selected) == 1 and selected[0]["flags"] == expected_flags and selected[0]["aiTeams"] == [cpu] and selected[0]["legacy"] == cpu
        first = phases["first_tick"]
        checks[peer + "_first_running_tick"] = len(first) == 1 and first[0]["running"] == 1 and first[0]["tick"] == 1
        if arm == "stock":
            checks[peer + "_stock_director_nonempty"] = len(first) == 1 and first[0]["directorTeams"] == [cpu]
        if arm == "script":
            checks[peer + "_lua_property_write"] = [list(map(int, row)) for row in WRITE.findall(logs[peer])] == [[cpu, cpu]]
    checks["shared_cpu_facts"] = "host" in rows and "client" in rows and rows["host"] == rows["client"]
    needs_placement = arm == "stock" and set(rows) == {"host", "client"} and not any(FATAL.search(log) for log in logs.values()) and all(
        len(started := [row for row in values if row["phase"] == "started"]) == 1 and started[0]["state"] == 2 for values in rows.values())
    return {"pass": all(checks.values()), "launchable": bool(launchable), "needs_brain_placement": needs_placement, "checks": checks, "rows": rows}


def read_log(directory):
    stdout = directory / "stdout.log"
    console = directory / "runtime/LogConsole.txt"
    value = stdout.read_text(errors="replace") if stdout.is_file() else ""
    if not FACTS.search(value) and console.is_file():
        value += "\n" + console.read_text(errors="replace")
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("arm", choices=ARMS)
    parser.add_argument("out", type=Path)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--harness", type=Path, default=Path("D:/Projects/stage2_p4/recovery_e2e.py"))
    args = parser.parse_args()
    if not 43570 <= args.port <= 43590:
        parser.error("port is outside the assigned lane")
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    os.environ["CCCP_HEADLESS"] = "1"
    os.chdir(REPO)
    sys.path[:0] = [str(REPO / "tools"), str(args.harness.parent)]
    spec = importlib.util.spec_from_file_location("cpu_pair_harness", args.harness)
    harness = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(harness)
    harness.REPO, harness.EXE = REPO, REPO / "Cortex Command.exe"
    harness.ROOT, harness.OUT = args.out, args.out / "e2e"
    preset, lua_class, original_preset, mode, cpu = ARMS[args.arm]
    # The arm's activity is staged into the run's own UserScenes module, so the match names it.
    flags = ["-net-match-service-preset", preset, "-net-match-service-module", "UserScenes.rte",
             "-net-match-mode", mode, "-seed", "42", "-num-lua-states", "4"]
    lane = {"port": args.port, "ticks": 600, "delay": 3, "mode": "normal", "host": flags[:], "client": flags[:],
            "what": "shared roster CPU facts at start and first tick, and exact replay"}
    manifest = {"stamp": stamp(), "arm": args.arm, "cpu_team": cpu, "lane": lane,
        "exe_sha256": sha(harness.EXE), "driver_sha256": sha(__file__), "harness_sha256": sha(args.harness),
        "fixtures": {path.name: sha(path) for path in sorted((HERE / "fixtures").glob("Cpu*.lua"))},
        "stock_scripts": {path.name: sha(path) for path in (REPO / "Data/Base.rte/Activities/P4AlphaDuel.lua", REPO / "Data/Base.rte/Activities/SkirmishDefense.lua")}}
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    original = harness.run_isolated

    def prepare(*positional, **keywords):
        if "-net-replay" in positional[0]:
            positional = ([*positional[0], "-seed", "42", "-num-lua-states", "4"], *positional[1:])
        run = original(*positional, **keywords)
        module = Path(run.cwd) / "Userdata/UserScenes.rte"
        module.mkdir(exist_ok=True)
        for path in (HERE / "fixtures").glob("Cpu*.lua"):
            (module / path.name).write_bytes(path.read_bytes())
        (module / "Index.ini").write_text("DataModule\n\tModuleName = User Scenes\n\tAddActivity = GAScripted\n"
            f"\t\tCopyOf = {original_preset}\n\t\tPresetName = {preset}\n"
            f"\t\tScriptPath = UserScenes.rte/{lua_class}.lua\n\t\tLuaClassName = {lua_class}\n", encoding="utf-8")
        run.env["CC_SIM_DUMP"] = "1:600"
        start, finish = run.start, run.finish

        def ledger(event):
            actual = sha(run.argv[0])
            with (args.out / "exe-ledger.jsonl").open("a", encoding="utf-8") as stream:
                stream.write(json.dumps({"stamp": stamp(), "event": event, "cwd": str(run.cwd), "exe": str(run.argv[0]), "sha256": actual}) + "\n")
            if actual != manifest["exe_sha256"]:
                raise RuntimeError("executable changed during CPU arm")

        def begin():
            ledger("before_instance")
            return start()

        def end():
            try:
                return finish()
            finally:
                ledger("after_instance")

        run.start, run.finish = begin, end
        return run

    harness.run_isolated = prepare
    result = harness.lane("humans_vs_cpu", lane)
    directory = args.out / "e2e/humans_vs_cpu"
    logs = {peer: read_log(directory / peer) for peer in ("host", "client")}
    facts = score(logs, args.arm)
    battery_path = Path("D:/Projects/reviews/takeover-20260909/grok-workers/fg6d-battery/check_fixture.py")
    spec = importlib.util.spec_from_file_location("cpu_battery_oracle", battery_path)
    battery = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(battery)
    facts["battery_oracle_sha256"] = sha(battery_path)
    facts["battery_team_is_cpu_visible"] = {peer: battery.team_is_cpu_visible({}, log, cpu) for peer, log in logs.items()}
    for peer, value in facts["battery_team_is_cpu_visible"].items():
        facts["checks"][peer + "_battery_team_is_cpu_visible"] = value[0]
    facts["replay_observations"] = parse(read_log(directory / "replay_playback"))
    facts["checks"]["replay_cpu_facts_exact"] = bool(facts["rows"]["host"]) and facts["replay_observations"] == facts["rows"]["host"]
    facts["pass"] = all(facts["checks"].values())
    result["cpu_facts"] = facts
    result["pass"] = result["pass"] and facts["pass"]
    (args.out / "cpu-result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    for peer, log in logs.items():
        for line in log.splitlines():
            if "[cpu-facts]" in line or "[cpu-write]" in line or "[e2e] TeamIsCPU" in line:
                print(peer + " " + line)
    print(json.dumps(result, indent=2))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
