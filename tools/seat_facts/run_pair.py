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
    parser.add_argument("arm", choices=("snapshot", "damage", "reseat", "lua"))
    parser.add_argument("out", type=Path)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--harness", type=Path, default=Path("D:/Projects/stage2_p4/recovery_e2e.py"))
    args = parser.parse_args()
    if not 43570 <= args.port <= 43590:
        parser.error("port is outside 43570..43590")
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
    harness.REPO, harness.EXE = REPO, REPO / "Cortex Command.exe"
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
    elif args.arm == "lua":
        for peer in ("host", "client"):
            lane[peer] += ["-net-match-service-preset", "Seat Facts"]
    manifest = {"stamp": stamp(), "arm": args.arm, "port": args.port,
                "exe": str(harness.EXE), "exe_sha256": sha(harness.EXE),
                "driver_sha256": sha(__file__), "fixture_sha256": sha(FIXTURE),
                "harness_sha256": sha(args.harness), "lane": lane}
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    original_run = harness.run_isolated

    def prepare(*positional, **keywords):
        run = original_run(*positional, **keywords)
        if args.arm == "lua":
            module = Path(run.cwd) / "Userdata/UserScenes.rte"
            module.mkdir(exist_ok=True)
            (module / "SeatFacts.lua").write_bytes(FIXTURE.read_bytes())
            (module / "Index.ini").write_text(
                "DataModule\n\tModuleName = User Scenes\n\tScanFolderContents = 1\n"
                "\tAddActivity = GAScripted\n\t\tPresetName = Seat Facts\n"
                "\t\tSceneName = Grasslands\n\t\tScriptPath = UserScenes.rte/SeatFacts.lua\n"
                "\t\tLuaClassName = SeatFacts\n\t\tMinTeamsRequired = 2\n"
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
    if args.arm == "lua":
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
    (args.out / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2))
    return 0 if result.get("pass") is True and result.get("seat_facts", {"pass": True})["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
