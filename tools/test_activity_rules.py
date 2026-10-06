"""A match hosted from the menus with nothing changed plays by the activity's own rules on both peers.

Two engines take the newcomer's path through the multiplayer menus: the host presses Host Game and Create Lobby, the
other joins by address and presses Ready, the host presses Start Match. Neither visits the options. At the round's first
tick each peer prints the rules it plays by; the run passes when, on both peers, the starting gold, fog of war, clear
path to orbit and unit deployment are the activity's own defaults - the values the single-player setup seeds from the
same activity - and every seated team's funds equal that gold.

  python tools/test_activity_rules.py --out D:/mx/<lane>/activity-rules [--port 47350] [--runs 1] [--path address|newcomer]

The newcomer path joins through the game list - the host's row, Join Game - instead of by address; the
host's port is the one setup word, so two runs on one machine never meet.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
from run_sim_test import make_run, engine_executable, file_sha256  # noqa: E402

RULES = re.compile(r"\[e2e\] rules tick=1 difficulty=(\d+) gold=(-?\d+) fog=(\d) orbit=(\d) deploy=(\d)(.*?) cpu_team=(-?\d+) activity=\"([^\"]*)\"")
FUNDS = re.compile(r"team(\d)\.funds=(-?[\d.]+)")
# The single-player setup's gold bands (ScenarioActivityConfigGUI::UpdateStartingGoldSliderAndLabel): the first band at or
# above the difficulty that names a gold, then the Max band's, then 2,000.
BANDS = ((20, "DefaultGoldCakeDifficulty"), (40, "DefaultGoldEasyDifficulty"), (60, "DefaultGoldMediumDifficulty"),
         (80, "DefaultGoldHardDifficulty"), (95, "DefaultGoldNutsDifficulty"), (100, "DefaultGoldMaxDifficulty"))


def activity_defaults(repo: Path, module_and_preset: str) -> dict:
    """An activity's own defaults, read from its module's definition the way the engine reads them."""
    module, _, preset = module_and_preset.partition("/")
    for ini in sorted((repo / "Data" / module).rglob("*.ini")):
        text = ini.read_text(encoding="utf-8", errors="replace")
        for block in re.split(r"(?m)^(?=AddActivity\s*=)", text):
            if re.search(rf"(?m)^\s*PresetName\s*=\s*{re.escape(preset)}\s*$", block):
                values = {key: int(value) for key, value in re.findall(r"(?m)^\s*(Default\w+)\s*=\s*(-?\d+)", block)}
                return {"file": str(ini), "values": values}
    raise SystemExit(f"no definition of {module_and_preset} under Data/{module}")


def expected_rules(defaults: dict, difficulty: int) -> dict:
    values = defaults["values"]
    gold = next((values[key] for ceiling, key in BANDS if difficulty <= ceiling and values.get(key, -1) > -1), None)
    if gold is None:
        gold = values["DefaultGoldMaxDifficulty"] if values.get("DefaultGoldMaxDifficulty", -1) > -1 else 2000
    return {"gold": gold, "fog": int(values.get("DefaultFogOfWar", -1) > 0), "orbit": int(values.get("DefaultRequireClearPathToOrbit", -1) > 0),
            "deploy": int(values.get("DefaultDeployUnits", -1) > 0)}


def newcomer_scripts(port: int) -> dict:
    host = ("wait 40\nactivate ButtonMainToMultiplayer\nwait 12\nassert_substate Landing\nactivate ButtonMultiplayerHostGame\nwait 6\n"
            f"assert_substate HostSetup\nsetup_host_port {port}\nactivate ButtonMultiplayerCreate\nwait 15\nassert_substate Lobby\n"
            "wait_remote_ready 120\nwait 10\nactivate ButtonMultiplayerStart\nwait_state Running 60\nwait_ms 600000\nexit\n")
    client = ("wait 40\nactivate ButtonMainToMultiplayer\nwait 12\nassert_substate Landing\nactivate ButtonMultiplayerJoinGame\nwait 6\n"
              f"assert_substate JoinSetup\nwait_row GameRowPort{port} 90\nclick_row GameRowPort{port}\nwait 4\nactivate ButtonMultiplayerConnect\n"
              "wait_connected 2 90\nwait_substate Lobby 30\nwait 5\nactivate ButtonMultiplayerReady\nwait_ms 600000\nexit\n")
    return {"host": host, "client": client}


def scripts(port: int) -> dict:
    host = ("wait 40\nactivate ButtonMainToMultiplayer\nwait 12\nassert_substate Landing\nsettext TextMultiplayerName Host\n"
            "activate ButtonMultiplayerHostGame\nwait 6\nassert_substate HostSetup\n"
            # The run's own port, so two runs on one machine never meet; nothing else on the screen is touched.
            f"setup_host_port {port}\n"
            "activate ButtonMultiplayerCreate\nwait 15\nassert_substate Lobby\nwait_remote_ready 120\nwait 10\n"
            "activate ButtonMultiplayerStart\nwait_state Running 60\nwait_ms 600000\nexit\n")
    client = ("wait 40\nactivate ButtonMainToMultiplayer\nwait 12\nassert_substate Landing\nsettext TextMultiplayerName Joiner\n"
              "activate ButtonMultiplayerJoinGame\nwait 6\nassert_substate JoinSetup\n"
              f"activate ButtonJoinByAddress\nwait 4\nsettext TextJoinAddress 127.0.0.1\nsettext TextJoinPort {port}\n"
              "activate ButtonJoinAddressGo\nwait_connected 2 90\nwait_substate Lobby 30\nwait 5\nactivate ButtonMultiplayerReady\n"
              "wait_ms 600000\nexit\n")
    return {"host": host, "client": client}


def run_once(options, root: Path) -> dict:
    root.mkdir(parents=True, exist_ok=False)
    runs, logs, result = {}, {}, {"peers": {}}
    started = set()
    try:
        for who, text in (newcomer_scripts if options.path == "newcomer" else scripts)(options.port).items():
            script = root / f"{who}-menu.txt"
            script.write_text(text, encoding="utf-8")
            runs[who] = make_run(options.repo, ["-menu-script", str(script)], root / who, options.timeout, env={"CCCP_HEADLESS": "1"})
            runs[who].start()
            started.add(who)
            if who == "host":
                time.sleep(2.0)
        deadline = time.monotonic() + options.timeout
        while time.monotonic() < deadline:
            for who, run in runs.items():
                log = run.out / "stdout.log"
                logs[who] = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
            if all(RULES.search(text) for text in logs.values()):
                break
            if any(run.poll() is not None for run in runs.values()):
                break
            time.sleep(1.0)
    finally:
        # A run whose start was refused never ran: its refusal is what propagates, so only started runs are stopped.
        for who, run in runs.items():
            if who in started and run.poll() is None:
                run.terminate(0, "the rules were read")
            try:
                run.close()
            except Exception:
                pass
    for who in runs:
        log = runs[who].out / "stdout.log"
        logs[who] = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
    passed = True
    for who, text in logs.items():
        match = RULES.search(text)
        failed = re.findall(r"\[menu-script\] FAILED: [^\r\n]*", text)
        if not match:
            result["peers"][who] = {"pass": False, "reason": "no rules line at the round's first tick", "menu_failures": failed}
            passed = False
            continue
        difficulty, gold, fog, orbit, deploy, tail, cpu_team, activity = match.groups()
        defaults = activity_defaults(options.repo, activity)
        expected = expected_rules(defaults, int(difficulty))
        funds = {int(team): float(value) for team, value in FUNDS.findall(tail)}
        seated = [0, 1]
        observed = {"gold": int(gold), "fog": int(fog), "orbit": int(orbit), "deploy": int(deploy)}
        peer_pass = observed == expected and all(funds.get(team) == float(expected["gold"]) for team in seated)
        passed &= peer_pass
        result["peers"][who] = {"pass": peer_pass, "line": match.group(0)[:400], "activity": activity, "difficulty": int(difficulty),
                                "observed": observed, "expected": expected, "seated_team_funds": {team: funds.get(team) for team in seated},
                                "defaults_from": defaults["file"], "menu_failures": failed}
    result["pass"] = passed
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=REPO)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=47350)
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--path", choices=("address", "newcomer"), default="address", help="join by address, or the newcomer's way through the game list")
    options = parser.parse_args()
    options.repo = options.repo.resolve()
    options.out.mkdir(parents=True, exist_ok=False)
    rows = [run_once(options, options.out / f"run{index + 1}") for index in range(options.runs)]
    summary = {"pass": all(row["pass"] for row in rows), "passed_runs": sum(row["pass"] for row in rows), "runs": len(rows),
               "exe_sha256": file_sha256(engine_executable(options.repo)), "rows": rows}
    (options.out / "result.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"[activity-rules] {'PASS' if summary['pass'] else 'FAIL'} {summary['passed_runs']} of {summary['runs']} {options.out / 'result.json'}")
    for row in rows:
        for who, peer in row["peers"].items():
            print(f"[activity-rules] {who}: observed={peer.get('observed')} expected={peer.get('expected')} funds={peer.get('seated_team_funds')} {peer.get('reason', '')}")
    return 0 if summary["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
