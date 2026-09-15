"""What a mod sees of the screen, at two window sizes, on any build.

FrameMan.PlayerScreenWidth/Height are mod-visible and must keep answering this machine's window
whatever the engine does about simulation; the pinned pair must answer the default window at every
size. The same fixture runs on a pre-change reference executable, where the pinned pair is absent,
so the two logs are the before-and-after evidence.
"""

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from run_sim_test import make_run
from test_telemetry_bundle import set_visual_resolution
from test_window_size_sim import parse_size, sha256_file

REPO = Path(__file__).resolve().parents[1]
FIXTURE = Path(__file__).resolve().parent / "fixtures/screen_property_facts.lua"
PRESET = "Determinism Screen Property Facts"
INDEX = (
    "DataModule\n\tModuleName = User Scenes\n\tScanFolderContents = 1\n\tIgnoreMissingItems = 1\n"
    "\tAddActivity = GAScripted\n\t\tPresetName = " + PRESET + "\n"
    "\t\tSceneName = Grasslands\n\t\tScriptPath = UserScenes.rte/ScreenPropertyFacts.lua\n"
    "\t\tLuaClassName = ScreenPropertyFacts\n\t\tMinTeamsRequired = 1\n\t\tIsTestActivity = 1\n"
    "\t\tTeamOfPlayer1 = 0\n\t\tPlayer1IsHuman = 1\n\t\tDefaultFogOfWar = 0\n"
)
FACTS = re.compile(r"\[screenprops\] player=(\S+) sim=(\S+) screens=(\S+) resmult=(\S+)")


def run_one(repo: Path, out: Path, size, ticks: int, timeout: float) -> dict:
    run = make_run(repo, ["-scenario", PRESET, "-seed", "42", "-max-ticks", str(ticks), "-scenario-run-past-end"],
                   out, timeout, env={"CCCP_HEADLESS": "1"})
    set_visual_resolution(run, *size)
    module = Path(run.cwd) / "Userdata/UserScenes.rte"
    module.mkdir(parents=True, exist_ok=True)
    (module / "Index.ini").write_text(INDEX, encoding="utf-8")
    (module / "ScreenPropertyFacts.lua").write_bytes(FIXTURE.read_bytes())
    try:
        record = run.start().finish()
    finally:
        run.close()
    text = ""
    for name in ("stdout.log", "runtime/LogConsole.txt"):
        path = out / name
        if path.exists():
            text += path.read_text(errors="replace")
    facts = FACTS.findall(text)
    return {"size": list(size), "exit_code": record.get("exit_code"), "timed_out": record.get("timed_out"),
            "player": facts[0][0] if facts else None, "sim": facts[0][1] if facts else None,
            "screens": facts[0][2] if facts else None, "resmult": facts[0][3] if facts else None}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=REPO)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, help="read the activity script from here instead of the tree")
    parser.add_argument("--sizes", nargs="+", default=["960x540", "640x360"])
    parser.add_argument("--ticks", type=int, default=60)
    parser.add_argument("--timeout", type=float, default=300)
    options = parser.parse_args()
    global FIXTURE
    if options.fixture:
        FIXTURE = options.fixture.resolve()

    options.out.mkdir(parents=True, exist_ok=False)
    result = {"exe_sha256": sha256_file(options.repo / "Cortex Command.exe"), "fixture": str(FIXTURE), "runs": {}}
    for text in options.sizes:
        result["runs"][text] = run_one(options.repo, options.out / text, parse_size(text), options.ticks, options.timeout)
    (options.out / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    for text, row in result["runs"].items():
        print(f"{text}: player={row['player']} sim={row['sim']} screens={row['screens']} resmult={row['resmult']}")
    return 0 if all(row["player"] for row in result["runs"].values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
