"""Game-over observe freeze: look-around lifts at the same sim tick on both peers.

The fixture ends the round. InputScript drives the observe view after OVER. The C++
gate prints the lift tick at the existing freeze site. Written, not run.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import sys

REPO = Path(__file__).resolve().parents[1]
FIXTURE = REPO / "tools/fixtures/game_over_observe.lua"
INPUTS = REPO / "tools/fixtures/game_over_observe.txt"
PRESET = "Determinism Game Over Observe"
LIFT = re.compile(r"\[game-over-freeze\] lift tick=(\d+) elapsed=([-\d.]+) player=(\d+)")
INDEX = (
    "DataModule\n\tModuleName = User Scenes\n\tScanFolderContents = 1\n\tIgnoreMissingItems = 1\n"
    "\tAddActivity = GAScripted\n\t\tPresetName = " + PRESET + "\n"
    "\t\tSceneName = Grasslands\n\t\tScriptPath = UserScenes.rte/GameOverObserve.lua\n"
    "\t\tLuaClassName = GameOverObserve\n\t\tMinTeamsRequired = 2\n\t\tIsTestActivity = 1\n"
    "\t\tTeamOfPlayer1 = 0\n\t\tPlayer1IsHuman = 1\n"
    "\t\tDefaultFogOfWar = 0\n\t\tDefaultRequireClearPathToOrbit = 0\n\t\tDefaultDeployUnits = 0\n"
)


def sha(path: Path) -> str:
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def peer_log(run_dir: Path) -> str:
    text = ""
    for name in ("stdout.log", "runtime/LogConsole.txt"):
        path = run_dir / name
        if path.exists():
            text += path.read_text(encoding="utf-8", errors="replace")
    return text


def first_lift(text: str) -> tuple[int, float] | None:
    match = LIFT.search(text)
    if not match:
        return None
    return int(match.group(1)), float(match.group(2))


def stage_module(runtime: Path) -> None:
    module = Path(runtime) / "Userdata/UserScenes.rte"
    module.mkdir(parents=True, exist_ok=True)
    (module / "Index.ini").write_text(INDEX, encoding="utf-8")
    (module / "GameOverObserve.lua").write_bytes(FIXTURE.read_bytes())


def inspect(root: Path, peers: tuple[str, ...]) -> dict:
    checks = {}
    details = {"lift": {}}
    ticks = []
    for who in peers:
        lift = first_lift(peer_log(root / who))
        details["lift"][who] = None if lift is None else {"tick": lift[0], "elapsed": lift[1]}
        checks[f"{who}_lifted"] = lift is not None
        checks[f"{who}_held"] = bool(lift and lift[1] >= 1000.0)
        ticks.append(None if lift is None else lift[0])
    checks["same_tick"] = len(set(ticks)) == 1 and None not in ticks
    return {"pass": all(checks.values()), "checks": checks, "details": details}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=REPO)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=48531)
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--ticks", type=int, default=240)
    options = parser.parse_args()
    sys.path.insert(0, str(options.repo / "tools"))
    from run_sim_test import make_run

    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    os.environ["CCCP_HEADLESS"] = "1"
    peers = ("host", "client")
    runs = {}
    try:
        for who in peers:
            flags = ["-seed", "42", "-free-run-sim", "-net-match-service-e2e",
                     "-net-port", str(options.port), "-net-match-peers", "2",
                     "-net-match-ticks", str(options.ticks), "-net-match-input-delay", "3",
                     "-net-match-service-preset", PRESET, "-net-match-service-module", "UserScenes.rte",
                     "-net-autosave-seconds", "0",
                     "-input-script", str(INPUTS),
                     "-net-match-report", str(root / f"{who}_report.json")]
            flags += ["-net-host"] if who == "host" else ["-net-join", "127.0.0.1"]
            env = {"CCCP_HEADLESS": "1"}
            run = make_run(options.repo, flags, root / who, timeout=options.timeout, env=env)
            stage_module(Path(run.cwd))
            runs[who] = run
        for run in runs.values():
            run.start()
        for run in runs.values():
            run.wait()
        result = inspect(root, peers)
        result["driver_sha256"] = sha(Path(__file__))
        result["fixture_sha256"] = sha(FIXTURE)
        result["input_sha256"] = sha(INPUTS)
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print("PASS" if result["pass"] else "FAIL", "game_over_observe", result["details"])
        return 0 if result["pass"] else 1
    finally:
        for run in runs.values():
            run.close()


if __name__ == "__main__":
    raise SystemExit(main())
