"""What a shared script's path query sees of terrain it just dug.

Scene:CalculatePath from a script that is not the running AI actor reads the committed horizon under
lockstep, so the path asked on the tick the hole was dug answers against the terrain of T - H. The
same fixture runs on a pre-change reference executable, where there is no horizon and the hole is
there at once, so the two logs are the before-and-after evidence. The driver records the three
lengths and asserts only that the fixture ran: the lag is the result, not a failure.

    python tools/test_dig_then_path.py --out <dir> [--repo <tree>]
"""

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from run_sim_test import make_run, engine_executable
from test_window_size_sim import sha256_file

REPO = Path(__file__).resolve().parents[1]
FIXTURE = Path(__file__).resolve().parent / "fixtures/dig_then_path.lua"
PRESET = "Determinism Dig Then Path"
INDEX = (
    "DataModule\n\tModuleName = User Scenes\n\tScanFolderContents = 1\n\tIgnoreMissingItems = 1\n"
    "\tAddActivity = GAScripted\n\t\tPresetName = " + PRESET + "\n"
    "\t\tSceneName = Grasslands\n\t\tScriptPath = UserScenes.rte/DigThenPath.lua\n"
    "\t\tLuaClassName = DigThenPath\n\t\tMinTeamsRequired = 1\n\t\tIsTestActivity = 1\n"
    "\t\tTeamOfPlayer1 = 0\n\t\tPlayer1IsHuman = 1\n\t\tDefaultFogOfWar = 0\n"
)
PHASE = re.compile(r"\[digpath\] tick=(\d+) phase=(\w+) len=(-?\d+)")
DUG = re.compile(r"\[digpath\] tick=(\d+) dug=(\d+)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=REPO)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, help="read the activity script from here instead of the tree")
    parser.add_argument("--ticks", type=int, default=240)
    parser.add_argument("--timeout", type=float, default=300)
    options = parser.parse_args()
    fixture = options.fixture.resolve() if options.fixture else FIXTURE

    options.out.mkdir(parents=True, exist_ok=False)
    run = make_run(options.repo,
                   ["-scenario", PRESET, "-seed", "42", "-max-ticks", str(options.ticks), "-scenario-run-past-end"],
                   options.out / "run", options.timeout, env={"CCCP_HEADLESS": "1"})
    module = Path(run.cwd) / "Userdata/UserScenes.rte"
    module.mkdir(parents=True, exist_ok=True)
    (module / "Index.ini").write_text(INDEX, encoding="utf-8")
    (module / "DigThenPath.lua").write_bytes(fixture.read_bytes())
    try:
        record = run.start().finish()
    finally:
        run.close()

    text = ""
    for name in ("stdout.log", "runtime/LogConsole.txt"):
        path = options.out / "run" / name
        if path.exists():
            text += path.read_text(errors="replace")
    phases = {phase: {"tick": int(tick), "len": int(length)} for tick, phase, length in PHASE.findall(text)}
    dug = [{"tick": int(tick), "pixels": int(pixels)} for tick, pixels in DUG.findall(text)]
    result = {"exe_sha256": sha256_file(engine_executable(options.repo)), "fixture": str(fixture),
              "exit_code": record.get("exit_code"), "timed_out": record.get("timed_out"),
              "dug": dug, "phases": phases,
              # True where the tick that dug the hole already pathed through it; a horizon answers False.
              "same_tick_sees_the_hole": bool(phases) and phases.get("same", {}).get("len", -1) > 0}
    (options.out / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    return 0 if all(name in phases for name in ("before", "same", "later")) and dug else 1


if __name__ == "__main__":
    raise SystemExit(main())
