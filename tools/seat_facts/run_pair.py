"""Run the shared-seat regression arms through the retained two-peer harness."""
from __future__ import annotations

import argparse
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys

REPO = Path(__file__).resolve().parents[2]
FIXTURE = Path(__file__).parent / "fixtures/SeatFacts.lua"
ROW = re.compile(r"\[seat-facts\] player=(\d+) active=(\d+) human=(\d+) team=(-?\d+) brain=(\d+) mark=(\d+) screen=(-?\d+)")
CONTROL_ROW = re.compile(r"\[control-facts\] tick=(\d+) player=(\d+) uid=(\d+) screen=(-?\d+)")
VESSEL_ROW = re.compile(r"\[vessel-facts\] tick=(\d+) player=(\d+) uid=(\d+) screen=(-?\d+) (.*)$", re.M)
# arm -> (Lua class, activity preset name); every scripted arm also carries the seat-facts fixture.
SCRIPTED = {"lua": ("SeatFacts", "Seat Facts"), "screens": ("ScreenFacts", "Screen Facts"), "control": ("ControlFacts", "Control Facts"),
            "vessel": ("VesselBannerFacts", "Vessel Banner Facts")}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def stamp():
    return subprocess.check_output(["date", "+%Y-%m-%d %H:%M:%S MST"], text=True).strip()


def score_seats(logs):
    rows = {peer: [tuple(map(int, row)) for row in ROW.findall(log)] for peer, log in logs.items()}
    complete = set(rows) == {"host", "client"} and all(len(values) == 4 and [row[0] for row in values] == list(range(4)) for values in rows.values())
    checks = {"four_seat_rows_per_peer": complete}
    if complete:
        checks["shared_seats_and_brains"] = [row[:-1] for row in rows["host"]] == [row[:-1] for row in rows["client"]]
        for peer, values in rows.items():
            checks[peer + "_both_human_branches"] = all(
                row[1:4] == (1, 1, player) and row[4] > 0 and row[5] == 101 + player
                for player, row in enumerate(values[:2]))
            checks[peer + "_distinct_brains"] = values[0][4] != values[1][4]
            checks[peer + "_inactive_seats"] = all(row[1:3] == (0, 0) and row[4:6] == (0, 0) for row in values[2:])
            checks[peer + "_local_screen"] = [row[6] for row in values] == ([0, -1, -1, -1] if peer == "host" else [-1, 0, -1, -1])
    return {"pass": all(checks.values()), "checks": checks, "rows": rows}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("arm", choices=("snapshot", "damage", "reseat", "lua", "screens", "control", "vessel"))
    parser.add_argument("out", type=Path)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--harness", type=Path, default=Path("D:/Projects/stage2_p4/recovery_e2e.py"))
    parser.add_argument("--exe", type=Path, help="run the arm on a retained executable instead of the tree's own")
    args = parser.parse_args()
    if not (43570 <= args.port <= 43590 or 48470 <= args.port <= 48479):
        parser.error("port is outside 43570..43590 and 48470..48479")
    args.out = args.out.resolve()
    if args.out.exists():
        parser.error("evidence directory already exists")
    args.out.mkdir(parents=True)
    os.environ["CCCP_HEADLESS"] = "1"
    os.chdir(REPO)
    sys.path[:0] = [str(REPO / "tools"), str(args.harness.parent)]
    spec = importlib.util.spec_from_file_location("seat_pair_harness", args.harness)
    harness = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(harness)
    harness.REPO, harness.EXE = REPO, (args.exe.resolve() if args.exe else REPO / "Cortex Command.exe")
    harness.ROOT, harness.OUT = args.out, args.out / "e2e"
    lane = copy.deepcopy(harness.LANES["snapshot_p5"])
    lane["port"] = args.port
    if args.arm == "damage":
        for peer in ("host", "client"):
            lane[peer] += ["-net-match-e2e-brain-damage", "299"]
    elif args.arm == "reseat":
        for peer in ("host", "client"):
            lane[peer] += ["-net-match-e2e-brain-reseat", "100"]
        lane["client"] += ["-net-match-e2e-brain-spawn-command"]
    elif args.arm in SCRIPTED:
        for peer in ("host", "client"):
            lane[peer] += ["-net-match-service-preset", SCRIPTED[args.arm][1]]
    manifest = {"stamp": stamp(), "arm": args.arm, "port": args.port,
                "exe": str(harness.EXE), "exe_sha256": sha(harness.EXE),
                "driver_sha256": sha(__file__), "fixture_sha256": sha(FIXTURE),
                "harness_sha256": sha(args.harness), "lane": lane}
    if args.arm in SCRIPTED and args.arm != "lua":
        manifest["script_fixture_sha256"] = sha(FIXTURE.with_name(SCRIPTED[args.arm][0] + ".lua"))
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    original_run = harness.run_isolated

    def prepare(*positional, **keywords):
        run = original_run(*positional, **keywords)
        if args.arm in SCRIPTED:
            module = Path(run.cwd) / "Userdata/UserScenes.rte"
            module.mkdir(exist_ok=True)
            (module / "SeatFacts.lua").write_bytes(FIXTURE.read_bytes())
            class_name = SCRIPTED[args.arm][0]
            if args.arm != "lua":
                (module / (class_name + ".lua")).write_bytes(FIXTURE.with_name(class_name + ".lua").read_bytes())
            (module / "Index.ini").write_text(
                "DataModule\n\tModuleName = User Scenes\n\tScanFolderContents = 1\n"
                "\tAddActivity = GAScripted\n\t\tPresetName = " + SCRIPTED[args.arm][1] + "\n"
                f"\t\tSceneName = Grasslands\n\t\tScriptPath = UserScenes.rte/{class_name}.lua\n"
                f"\t\tLuaClassName = {class_name}\n\t\tMinTeamsRequired = 2\n"
                "\t\tDefaultRequireClearPathToOrbit = 0\n\t\tDefaultFogOfWar = 0\n"
                "\t\tDefaultDeployUnits = 0\n", encoding="utf-8")
        start, finish = run.start, run.finish

        def ledger(event):
            entry = {"stamp": stamp(), "event": event, "cwd": str(run.cwd),
                     "exe": str(run.argv[0]), "sha256": sha(run.argv[0])}
            with (args.out / "exe-ledger.jsonl").open("a", encoding="utf-8") as stream:
                stream.write(json.dumps(entry) + "\n")
            if entry["sha256"] != manifest["exe_sha256"]:
                raise RuntimeError("executable changed during the arm")

        def checked_start():
            ledger("before_instance")
            return start()

        def checked_finish():
            try:
                return finish()
            finally:
                ledger("after_instance")

        run.start, run.finish = checked_start, checked_finish
        return run

    harness.run_isolated = prepare
    result = harness.lane("snapshot_p5", lane)
    if args.arm in SCRIPTED:
        logs = {}
        for peer in ("host", "client"):
            run_dir = args.out / "e2e/snapshot_p5" / peer
            logs[peer] = (run_dir / "stdout.log").read_text(errors="replace")
            console = run_dir / "runtime/LogConsole.txt"
            if console.exists() and not ROW.search(logs[peer]):
                logs[peer] = console.read_text(errors="replace")
        seat_result = score_seats(logs)
        (args.out / "seat-result.json").write_text(json.dumps(seat_result, indent=2), encoding="utf-8")
        result["seat_facts"] = seat_result
        if args.arm == "screens":
            result["screen_facts"] = {peer: log.count("[screen-facts] pass remote=1") == 1 for peer, log in logs.items()}
        if args.arm == "control":
            rows = {peer: CONTROL_ROW.findall(log) for peer, log in logs.items()}
            # Every peer must name the same controlled actor for every seat; the screen column stays local.
            seats = {peer: [row[:3] for row in peer_rows] for peer, peer_rows in rows.items()}
            result["control_facts"] = {"rows": rows, "pass": len(seats["host"]) == 4 and seats["host"] == seats["client"]}
        if args.arm == "vessel":
            rows = {peer: VESSEL_ROW.findall(log) for peer, log in logs.items()}
            # The unchanged mod fixture must reach every seat on both peers with the same answers; only the screen is local.
            seats = {peer: [row[:3] for row in peer_rows] for peer, peer_rows in rows.items()}
            neutral = {peer: [row[4] for row in peer_rows if row[3] == "-1"] for peer, peer_rows in rows.items()}
            errors = {peer: [line for line in log.splitlines() if "attempt to index" in line or "attempt to call" in line]
                      for peer, log in logs.items()}
            result["vessel_facts"] = {"rows": rows, "errors": errors, "remote_neutral": neutral,
                                      "pass": len(seats["host"]) == 4 and seats["host"] == seats["client"] and not any(errors.values()) and
                                              len(neutral["host"]) + len(neutral["client"]) == 6 and
                                              all(value == "text= anim=0 kerning=0 visible=0 menu=1 editor=1 buyvisible=0"
                                                  for values in neutral.values() for value in values)}
    (args.out / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2))
    return 0 if (result.get("pass") is True and result.get("seat_facts", {"pass": True})["pass"] and
                 all(result.get("screen_facts", {}).values()) and result.get("control_facts", {"pass": True})["pass"] and
                 result.get("vessel_facts", {"pass": True})["pass"]) else 1


if __name__ == "__main__":
    raise SystemExit(main())
