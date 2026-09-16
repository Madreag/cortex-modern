"""Game-over observe freeze: look-around is held until the same sim tick on both peers.

The fixture ends the round, injects HOLD_RIGHT, and prints the observation target.
On the base (wall-clock freeze) the first move's elapsed sim time differs.
This arm is written, not run.
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
PRESET = "Determinism Game Over Observe"
TARGET = re.compile(r"\[game-over-freeze\] target=([-\d.]+),([-\d.]+) elapsed=([-\d.]+) player=(\d+)")
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


def parse_targets(text: str) -> list[tuple[float, float, float]]:
    rows = []
    for match in TARGET.finditer(text):
        rows.append((float(match.group(1)), float(match.group(2)), float(match.group(3))))
    return rows


def first_move_elapsed(rows: list[tuple[float, float, float]]) -> float | None:
    if not rows:
        return None
    origin = (rows[0][0], rows[0][1])
    for x, y, elapsed in rows:
        if (x, y) != origin:
            return elapsed
    return None


def held_until_sim(rows: list[tuple[float, float, float]]) -> bool:
    if not rows:
        return False
    origin = (rows[0][0], rows[0][1])
    return all((x, y) == origin for x, y, elapsed in rows if elapsed < 1000.0)


def stage_module(runtime: Path) -> None:
    module = Path(runtime) / "Userdata/UserScenes.rte"
    module.mkdir(parents=True, exist_ok=True)
    (module / "Index.ini").write_text(INDEX, encoding="utf-8")
    (module / "GameOverObserve.lua").write_bytes(FIXTURE.read_bytes())


def inspect(root: Path, peers: tuple[str, ...]) -> dict:
    checks = {}
    details = {"first_move": {}, "held": {}}
    moves = []
    for who in peers:
        rows = parse_targets(peer_log(root / who))
        move = first_move_elapsed(rows)
        held = held_until_sim(rows)
        details["first_move"][who] = move
        details["held"][who] = held
        checks[f"{who}_held"] = held
        checks[f"{who}_moved"] = move is not None
        moves.append(move)
    checks["same_tick"] = len(set(moves)) == 1 and None not in moves
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
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print("PASS" if result["pass"] else "FAIL", "game_over_observe", result["details"])
        return 0 if result["pass"] else 1
    finally:
        for run in runs.values():
            run.close()


if __name__ == "__main__":
    raise SystemExit(main())
