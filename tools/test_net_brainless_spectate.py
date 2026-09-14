"""Prove the brainless-humans spectate rule: the two-peer match arms and the single-player arms.

Arms
  net_rule_on        two peers, stock Skirmish Defense, every human brain destroyed by the fixture,
                     the host rule ON: both peers stay in Gameplay, both humans sit in Observe and
                     the round ends only when fewer than two sides stand. Host and client traces and
                     the recorded replay must stay identical.
  net_rule_off       the same row with the rule OFF: the round ends at the last human brain's death.
                     This is the RED line on a base executable and the negative control on the tip.
  sp_keep_playing    one process, a three-team skirmish on the stock script, the Gameplay setting ON:
                     the human's brain is destroyed while two CPU sides stand and the round runs on.
  sp_end_match       the same row with the setting OFF: the round ends at the human brain's death.
  sp_view_is_local   sp_keep_playing with and without the spectator cycling input: the per-tick
                     hashes must be identical, so the spectator view writes no simulation state.

Every engine launch goes through tools/win32_test_runner.py. Nothing runs while the lead's family
lock exists.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
FIXTURE = REPO / "tools/fixtures/brainless_spectate_probe.lua"
SCRATCH = Path("D:/mx/opus-l33-brainless-spectate-20260914")
FAMILY_LOCK = Path("D:/mx/LEAD_FAMILY.lock")
BATTERY_LOCK = Path("D:/mx/LEAD_BATTERY.lock")
EXCLUSIVE_LOCK = Path("D:/mx/LEAD_EXCLUSIVE.lock")
HARNESS = Path("D:/Projects/stage2_p4/recovery_e2e.py")
PORT_RANGE = range(48400, 48420)

PROBE = re.compile(
    r"\[spectate-probe\] simms=(\d+) state=(-?\d+) views=([-\d,]+) brained_teams=(\d+) winner=(-?\d+)")
KILL = re.compile(r"\[spectate-probe\] brain-kill simms=(\d+) team=(\d+) uid=(\d+)")
ACTIVITY_OVER = 6  # Activity::ActivityState::Over
VIEW_OBSERVE = 1  # Activity::ViewState::Observe
# GetViewState answers Observe for a seat this machine does not own, so only a local seat's row is
# evidence; the per-peer local seat is read from the match report in PHASE B.

# A three-team skirmish on the STOCK script: one human team and two CPU sides. The preset only
# names the stock activity file, so the rule under test is the shipped one, not a fixture's copy.
SP_INDEX = (
    "DataModule\n\tModuleName = User Scenes\n\tScanFolderContents = 1\n\tIgnoreMissingItems = 1\n"
    "\tAddActivity = GAScripted\n\t\tPresetName = Spectate Skirmish\n"
    "\t\tSceneName = Grasslands\n\t\tScriptPath = Base.rte/Activities/SkirmishDefense.lua\n"
    "\t\tLuaClassName = SkirmishDefense\n\t\tMinTeamsRequired = 2\n\t\tMaxPlayerSupport = 1\n"
    "\t\tTeamOfPlayer1 = 0\n\t\tPlayer1IsHuman = 1\n"
    "\t\tTeamOfPlayer2 = 1\n\t\tPlayer2IsHuman = 0\n"
    "\t\tTeamOfPlayer3 = 2\n\t\tPlayer3IsHuman = 0\n"
    "\t\tDefaultFogOfWar = 0\n\t\tDefaultRequireClearPathToOrbit = 0\n\t\tDefaultDeployUnits = 0\n"
)
# The spectator's own input: cycle forward twice and back once, well after the brain is gone.
CYCLE_INPUT = "# spectator cycling for the brainless seat\n420 421 NEXT\n460 461 NEXT\n500 501 PREV\n"


def sha256(path: Path) -> str:
    with Path(path).open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def stamp() -> str:
    return subprocess.check_output(["date", "+%Y-%m-%d %H:%M:%S MST"], text=True).strip()


def refuse_on_locks() -> None:
    for lock in (FAMILY_LOCK, BATTERY_LOCK, EXCLUSIVE_LOCK):
        if lock.exists():
            raise SystemExit(f"refusing to run: {lock} exists")


def probe_rows(text: str) -> list[dict]:
    rows = []
    for simms, state, views, brained, winner in PROBE.findall(text):
        rows.append({"simms": int(simms), "state": int(state),
                     "views": [int(value) for value in views.split(",")],
                     "brained_teams": int(brained), "winner": int(winner)})
    return rows


def peer_log(run_dir: Path) -> str:
    text = ""
    for name in ("stdout.log", "runtime/LogConsole.txt"):
        path = run_dir / name
        if path.exists():
            text += path.read_text(errors="replace")
    return text


def check(checks: list[dict], name: str, ok: bool, detail: str, evidence: list) -> None:
    checks.append({"name": name, "status": "pass" if ok else "fail", "detail": detail,
                   "evidence": [str(item) for item in evidence]})


def load_harness(out: Path, port: int):
    sys.path[:0] = [str(REPO / "tools"), str(HARNESS.parent)]
    spec = importlib.util.spec_from_file_location("brainless_spectate_harness", HARNESS)
    harness = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(harness)
    harness.REPO, harness.EXE = REPO, REPO / "Cortex Command.exe"
    harness.ROOT, harness.OUT = out, out / "e2e"
    return harness


def net_arm(out: Path, port: int, rule_on: bool, ticks: int, timeout: float) -> dict:
    """Two peers on the stock activity; the fixture destroys every human brain at sim second 5."""
    harness = load_harness(out, port)
    lane = {
        "port": port,
        "delay": 0,
        "ticks": ticks,
        "timeout": timeout,
        "mode": "normal",
        "what": "every human brain is destroyed mid-match; the host rule decides whether the round ends",
        "host": [],
        "client": [],
    }
    common = [
        "-net-match-service-preset", "Spectate Skirmish",
        "-net-match-mode", "pvpve",
        "-net-match-brainless-spectate", "1" if rule_on else "0",
        "-net-autosave-seconds", "0",
        "-test-script", "UserScenes.rte/brainless_spectate_probe.lua",
    ]
    lane["host"] += common
    lane["client"] += common
    original_run = harness.run_isolated

    def prepare(*positional, **keywords):
        run = original_run(*positional, **keywords)
        module = Path(run.cwd) / "Userdata/UserScenes.rte"
        module.mkdir(parents=True, exist_ok=True)
        (module / "Index.ini").write_text(SP_INDEX, encoding="utf-8")
        (module / "brainless_spectate_probe.lua").write_bytes(FIXTURE.read_bytes())
        return run

    harness.run_isolated = prepare
    result = harness.lane("brainless_spectate", lane)
    checks: list[dict] = []
    logs = {peer: peer_log(out / "e2e/brainless_spectate" / peer) for peer in ("host", "client")}
    rows = {peer: probe_rows(text) for peer, text in logs.items()}
    for peer, text in logs.items():
        check(checks, f"{peer}_brains_destroyed", len(KILL.findall(text)) >= 2,
              f"brain-kill lines={len(KILL.findall(text))}", [out / "e2e/brainless_spectate" / peer / "stdout.log"])
        seat_rows = [row for row in rows[peer] if row["simms"] > 6000]
        check(checks, f"{peer}_seats_observe",
              bool(seat_rows) and all(all(view == VIEW_OBSERVE for view in row["views"][:2]) for row in seat_rows),
              f"views after the deaths={[row['views'] for row in seat_rows][:4]}",
              [out / "e2e/brainless_spectate" / peer / "stdout.log"])
        last = rows[peer][-1] if rows[peer] else None
        over = bool(last) and last["state"] == ACTIVITY_OVER
        check(checks, f"{peer}_round_{'ran_on' if rule_on else 'ended'}", over != rule_on,
              f"last probe row={last}", [out / "e2e/brainless_spectate" / peer / "stdout.log"])
    check(checks, "peers_agree", rows.get("host") == rows.get("client"),
          "host and client probe rows are identical", [out / "e2e/brainless_spectate"])
    result["spectate_checks"] = checks
    result["spectate_pass"] = all(row["status"] == "pass" for row in checks)
    return result


def sp_arm(out: Path, setting_on: bool, cycle_input: bool, ticks: int, timeout: float) -> dict:
    """One process on the same stock script, the Gameplay setting deciding the end rule."""
    sys.path.insert(0, str(REPO / "tools"))
    from run_sim_test import prepare_runtime  # noqa: E402
    from win32_test_runner import IsolatedRun  # noqa: E402

    out.mkdir(parents=True, exist_ok=False)
    runtime = prepare_runtime(REPO, out)
    module = runtime / "Userdata/UserScenes.rte"
    module.mkdir(parents=True, exist_ok=True)
    (module / "Index.ini").write_text(SP_INDEX, encoding="utf-8")
    (module / "brainless_spectate_probe.lua").write_bytes(FIXTURE.read_bytes())

    settings_path = runtime / "Userdata/Settings.ini"
    settings = settings_path.read_text(encoding="utf-8-sig")
    value = "1" if setting_on else "0"
    settings, count = re.subn(r"(?m)^(\s*BrainlessHumansSpectate\s*=\s*)[^\r\n]*",
                              lambda match: match[1] + value, settings)
    if count == 0:
        settings += f"\n\tBrainlessHumansSpectate = {value}\n"
    settings_path.write_text(settings, encoding="utf-8")

    flags = ["-scenario", "Spectate Skirmish", "-seed", "42", "-max-ticks", str(ticks),
             "-tick-hashes", "-out", str(out / "trace.json"),
             "-test-script", "UserScenes.rte/brainless_spectate_probe.lua"]
    if cycle_input:
        input_path = out / "spectator_input.txt"
        input_path.write_text(CYCLE_INPUT, encoding="utf-8")
        flags += ["-input-script", str(input_path)]
    exe = REPO / "Cortex Command.exe"
    argv = [str(exe), "-headless", *flags]
    env = {"TEMP": str(runtime / "Temp"), "TMP": str(runtime / "Temp"), "CCCP_HEADLESS": "1",
           "PATH": str(exe.parent) + os.pathsep + os.environ.get("PATH", "")}
    process = IsolatedRun(argv, runtime, out, timeout=timeout, env=env)
    try:
        record = process.start().finish()
    finally:
        process.close()

    text = peer_log(out)
    rows = probe_rows(text)
    last = rows[-1] if rows else None
    checks: list[dict] = []
    check(checks, "brain_destroyed", bool(KILL.findall(text)), f"kill lines={len(KILL.findall(text))}",
          [out / "stdout.log"])
    seat_rows = [row for row in rows if row["simms"] > 6000]
    check(checks, "seat_observes", bool(seat_rows) and all(row["views"][0] == VIEW_OBSERVE for row in seat_rows),
          f"seat 0 view after the death={[row['views'][0] for row in seat_rows][:8]}", [out / "stdout.log"])
    over = bool(last) and last["state"] == ACTIVITY_OVER
    check(checks, "round_ran_on" if setting_on else "round_ended", over != setting_on,
          f"last probe row={last}", [out / "stdout.log"])
    if not setting_on:
        check(checks, "winner_named", bool(last) and last["winner"] >= 0,
              f"winner={last['winner'] if last else None}", [out / "stdout.log"])
    return {"exe": str(exe), "exe_sha256": sha256(exe), "argv": argv, "out": str(out),
            "exit_code": record.get("exit_code"), "timed_out": record.get("timed_out"),
            "trace": str(out / "trace.json"), "probe_rows": rows,
            "checks": checks, "pass": all(row["status"] == "pass" for row in checks)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("arm", choices=("net_rule_on", "net_rule_off", "sp_keep_playing",
                                        "sp_end_match", "sp_view_is_local"))
    parser.add_argument("--out", type=Path, default=None)
    parser.add_argument("--port", type=int, default=PORT_RANGE.start)
    parser.add_argument("--ticks", type=int, default=900)
    parser.add_argument("--timeout", type=float, default=420)
    args = parser.parse_args()
    if args.port not in PORT_RANGE:
        parser.error(f"port is outside {PORT_RANGE.start}..{PORT_RANGE.stop - 1}")
    refuse_on_locks()
    out = (args.out or SCRATCH / (args.arm + "-" + time.strftime("%Y%m%d-%H%M%S"))).resolve()
    out.mkdir(parents=True, exist_ok=True)
    os.environ["CCCP_HEADLESS"] = "1"
    os.chdir(REPO)

    manifest = {"stamp": stamp(), "arm": args.arm, "port": args.port, "ticks": args.ticks,
                "repo": str(REPO), "exe_sha256": sha256(REPO / "Cortex Command.exe"),
                "driver_sha256": sha256(__file__), "fixture_sha256": sha256(FIXTURE)}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")

    if args.arm.startswith("net_"):
        result = net_arm(out, args.port, args.arm == "net_rule_on", args.ticks, args.timeout)
        passed = bool(result.get("pass", True)) and result.get("spectate_pass", False)
    elif args.arm == "sp_view_is_local":
        plain = sp_arm(out / "plain", True, False, args.ticks, args.timeout)
        cycled = sp_arm(out / "cycled", True, True, args.ticks, args.timeout)
        compare = subprocess.run(
            [sys.executable, str(REPO / "tools/compare_sim_traces.py"), plain["trace"], cycled["trace"],
             "--expected-ticks", str(args.ticks)],
            capture_output=True, text=True)
        (out / "compare.log").write_text(compare.stdout + compare.stderr, encoding="utf-8")
        result = {"plain": plain, "cycled": cycled, "compare_exit": compare.returncode,
                  "compare_log": str(out / "compare.log")}
        passed = plain["pass"] and cycled["pass"] and compare.returncode == 0
    else:
        result = sp_arm(out / "run", args.arm == "sp_keep_playing", args.arm == "sp_keep_playing",
                        args.ticks, args.timeout)
        passed = result["pass"]

    result["manifest"] = manifest
    result["pass"] = passed
    (out / "result.json").write_text(json.dumps(result, indent=2, default=str), encoding="utf-8")
    print(json.dumps({"arm": args.arm, "pass": passed, "out": str(out)}, indent=2))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
