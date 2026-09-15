"""Run an unchanged-mod activity fixture in one offline process, on any retained executable.

The seat-facts pair driver needs a firewall rule per executable; this one launches no socket, so the
retained pre-change reference can answer the same fixture as the broken and the repaired builds.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

REPO = Path(__file__).resolve().parents[2]
FIXTURE = Path(__file__).parent / "fixtures/SeatFacts.lua"
ROW = re.compile(r"\[vessel-mod\] tick=(\d+) player=(\d+) uid=(\d+) screen=(-?\d+) (.*)$", re.M)
ERROR = re.compile(r"^.*(?:attempt to index|attempt to call|attempt to perform).*$", re.M)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def stamp():
    return subprocess.check_output(["date", "+%Y-%m-%d %H:%M:%S MST"], text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("out", type=Path)
    parser.add_argument("--exe", type=Path, help="the executable to run; the tree's own by default")
    parser.add_argument("--fixture", default="VesselModCalls")
    parser.add_argument("--scenario", default="VesselMod")
    parser.add_argument("--ticks", type=int, default=320)
    args = parser.parse_args()
    args.out = args.out.resolve()
    if args.out.exists():
        parser.error("evidence directory already exists")
    os.environ["CCCP_HEADLESS"] = "1"
    sys.path.insert(0, str(REPO / "tools"))
    from run_sim_test import make_run

    exe = (args.exe or REPO / "Cortex Command.exe").resolve()
    run = make_run(REPO, ["-scenario", args.scenario, "-max-ticks", args.ticks], args.out / "run", timeout=300)
    run.argv[0] = str(exe)
    module = Path(run.cwd) / "Userdata/UserScenes.rte"
    module.mkdir(exist_ok=True)
    (module / "SeatFacts.lua").write_bytes(FIXTURE.read_bytes())
    (module / (args.fixture + ".lua")).write_bytes(FIXTURE.with_name(args.fixture + ".lua").read_bytes())
    (module / "Index.ini").write_text(
        "DataModule\n\tModuleName = User Scenes\n\tScanFolderContents = 1\n"
        f"\tAddActivity = GAScripted\n\t\tPresetName = Determinism {args.scenario}\n"
        f"\t\tSceneName = Grasslands\n\t\tScriptPath = UserScenes.rte/{args.fixture}.lua\n"
        f"\t\tLuaClassName = {args.fixture}\n\t\tMinTeamsRequired = 2\n"
        "\t\tDefaultRequireClearPathToOrbit = 0\n\t\tDefaultFogOfWar = 0\n"
        "\t\tDefaultDeployUnits = 0\n", encoding="utf-8")
    record = run.start().finish()
    log = (args.out / "run/stdout.log").read_text(errors="replace")
    console = Path(run.cwd) / "LogConsole.txt"
    if console.exists():
        log += console.read_text(errors="replace")
    rows = ROW.findall(log)
    errors = ERROR.findall(log)
    result = {"stamp": stamp(), "exe": str(exe), "exe_sha256": sha(exe), "fixture": args.fixture,
              "fixture_sha256": sha(FIXTURE.with_name(args.fixture + ".lua")), "driver_sha256": sha(__file__),
              "exit_code": record.get("exit_code"), "timed_out": record.get("timed_out"),
              "rows": rows, "errors": errors, "pass": len(rows) == 4 and not errors}
    (args.out / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
