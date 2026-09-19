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
FIXTURE = REPO / "tools/fixtures/spectate_skirmish_activity.lua"
SCRATCH = Path("D:/mx/opus-l33-brainless-spectate-20260914")
FAMILY_LOCK = Path("D:/mx/LEAD_FAMILY.lock")
BATTERY_LOCK = Path("D:/mx/LEAD_BATTERY.lock")
EXCLUSIVE_LOCK = Path("D:/mx/LEAD_EXCLUSIVE.lock")
HARNESS = Path("D:/Projects/stage2_p4/recovery_e2e.py")
PORT_RANGE = range(48400, 48420)

PROBE = re.compile(
    r"\[spectate-probe\] simms=(\d+) state=(-?\d+) views=([-\d,]+) brained_teams=(\d+) winner=(-?\d+)"
    r" rule=(1|0|na) scroll=(-?\d+),(-?\d+)")
KILL = re.compile(r"\[spectate-probe\] brain-kill simms=(\d+) team=(\d+) uid=(\d+)")
ACTIVITY_OVER = 6  # Activity::ActivityState::Over
VIEW_OBSERVE = 1  # Activity::ViewState::Observe
# GetViewState answers Observe for a seat this machine does not own, so only a local seat's row is
# evidence; the per-peer local seat is read from the match report in PHASE B.

# A three-team skirmish on the STOCK script: one human team and two CPU sides. The activity subclasses
# the shipped SkirmishDefense and only seats the brain the scenario menu would place, so the end rule
# under test is the shipped one, not a fixture's copy. ScenarioRunner::ResolvePresetName pads
# "Determinism " onto a -scenario name, so the preset carries that prefix and the net service (which
# takes the name verbatim) gets the same string. The two AI sides are marked by the activity script,
# not here: the launched activity is a clone of the preset and only the legacy CPUTeam scalar
# survives that copy, so a preset row would seat one attacker instead of two.
PRESET = "Determinism Spectate Skirmish"
SP_INDEX = (
    "DataModule\n\tModuleName = User Scenes\n\tScanFolderContents = 1\n\tIgnoreMissingItems = 1\n"
    "\tAddActivity = GAScripted\n\t\tPresetName = " + PRESET + "\n"
    "\t\tSceneName = Grasslands\n\t\tScriptPath = UserScenes.rte/SpectateSkirmish.lua\n"
    "\t\tLuaClassName = SpectateSkirmish\n\t\tMinTeamsRequired = 2\n\t\tIsTestActivity = 1\n"
    "\t\tTeamOfPlayer1 = 0\n\t\tPlayer1IsHuman = 1\n"
    "\t\tDefaultFogOfWar = 0\n\t\tDefaultRequireClearPathToOrbit = 0\n\t\tDefaultDeployUnits = 0\n"
)
# The spectator's own input: cycle forward twice and back once, well after the brain is gone.
CYCLE_INPUT = "# spectator cycling for the brainless seat\n420 421 NEXT\n460 461 NEXT\n500 501 PREV\n"
# Calibrated: the AI's first wave lands around sim second 4, so the brain dies with both CPU sides
# already fighting, and the cycling presses land at ticks 422/462/502 - after the death at tick ~301.
KILL_MS = 5000


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
    for simms, state, views, brained, winner, rule, scroll_x, scroll_y in PROBE.findall(text):
        rows.append({"simms": int(simms), "state": int(state),
                     "views": [int(value) for value in views.split(",")],
                     "brained_teams": int(brained), "winner": int(winner), "rule": rule,
                     "scroll": [int(scroll_x), int(scroll_y)]})
    return rows


def stage_user_module(runtime: Path, settings_value=None, arm_trace: bool = False, kill_follow_ms: int = 0) -> Path:
    """The staged UserScenes module: the three-team preset, its activity script and the arm options."""
    module = Path(runtime) / "Userdata/UserScenes.rte"
    module.mkdir(parents=True, exist_ok=True)
    (module / "Index.ini").write_text(SP_INDEX, encoding="utf-8")
    (module / "SpectateSkirmish.lua").write_bytes(FIXTURE.read_bytes())
    # armTrace only in single player: on a match peer, opening a metrics run would disarm the
    # per-tick recording the match harness already armed (BeginRun re-reads the scenario runner).
    (module / "SpectateOptions.lua").write_text(
        "return { armTrace = " + ("true" if arm_trace else "false") +
        ", killAtMs = " + str(KILL_MS) +
        ", killFollowAtMs = " + str(kill_follow_ms) + " }\n", encoding="utf-8")
    if settings_value is not None:
        set_setting(Path(runtime) / "Userdata/Settings.ini", settings_value)
    return module


def set_setting(settings_path: Path, value: bool) -> str:
    """Write this machine's Gameplay setting; a net peer must follow the host's rule instead of it."""
    settings = settings_path.read_text(encoding="utf-8-sig")
    text = "1" if value else "0"
    settings, count = re.subn(r"(?m)^(\s*BrainlessHumansSpectate\s*=\s*)[^\r\n]*",
                              lambda match: match[1] + text, settings)
    if count == 0:
        settings += f"\n\tBrainlessHumansSpectate = {text}\n"
    settings_path.write_text(settings, encoding="utf-8")
    return hashlib.sha256(settings.encode("utf-8")).hexdigest()


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


def net_arm(out: Path, port: int, rule_on: bool, ticks: int, timeout: float, sim_dump: str | None = None,
            dedicated: bool = True, delay: int = 0) -> dict:
    """Two peers on the stock activity; the fixture destroys every human brain at sim second 5.

    The roster decides what the arm can prove. BuildMatchConfig seats one CPU slot, so a two-human
    PvPvE row leaves exactly one side standing once both brains die and the round ends under either
    rule. A dedicated host seats no player, which puts the single human on team 0, the roster CPU on
    team 1 and leaves team 2 for the activity's second AI attacker: two sides stand after the only
    human brain dies, so the end rule itself is what the two arms differ by.
    """
    harness = load_harness(out, port)
    lane = {
        "port": port,
        "delay": delay,
        "ticks": ticks,
        "timeout": timeout,
        "mode": "normal",
        "what": "every human brain is destroyed mid-match; the host rule decides whether the round ends",
        "host": [],
        "client": [],
    }
    common = [
        "-net-match-service-preset", PRESET,
        # The activity is staged into the run's own UserScenes module, so the match names it.
        "-net-match-service-module", "UserScenes.rte",
        "-net-match-mode", "pvpve",
        "-net-match-brainless-spectate", "1" if rule_on else "0",
        "-net-autosave-seconds", "0",
    ]
    lane["host"] += common
    lane["client"] += common
    original_run = harness.run_isolated

    # Both machines' own Gameplay setting is the OPPOSITE of the host's flag, so a peer that read its
    # local setting instead of the agreed match rule prints the opposite value.
    local_setting = not rule_on

    def prepare(*positional, **keywords):
        if dedicated and positional and "-net-host" in positional[0]:
            # The harness lane always hosts with a seat; this arm needs the seatless host instead.
            argv = ["-net-dedicated" if item == "-net-host" else item for item in positional[0]]
            positional = (argv,) + positional[1:]
        run = original_run(*positional, **keywords)
        stage_user_module(Path(run.cwd), local_setting, arm_trace=False)
        if sim_dump:
            # Per-MO forensics for both peers; recorded in launch.json so the env difference is visible.
            run.env["CC_SIM_DUMP"] = sim_dump
            run.record["env_set"]["CC_SIM_DUMP"] = sim_dump
            run._save()
        return run

    harness.run_isolated = prepare
    result = harness.lane("brainless_spectate", lane)
    checks: list[dict] = []
    logs = {peer: peer_log(out / "e2e/brainless_spectate" / peer) for peer in ("host", "client")}
    rows = {peer: probe_rows(text) for peer, text in logs.items()}
    expected_rule = "1" if rule_on else "0"
    # The roster's human seats: a dedicated host seats only the joining peer, a hosted row seats both.
    human_seats = 1 if dedicated else 2
    for peer, text in logs.items():
        kills = KILL.findall(text)
        check(checks, f"{peer}_brains_destroyed", len(kills) == human_seats,
              f"brain-kill lines={len(kills)} human seats={human_seats}",
              [out / "e2e/brainless_spectate" / peer / "stdout.log"])
        seat_rows = [row for row in rows[peer] if row["simms"] > 6000]
        check(checks, f"{peer}_seats_observe",
              bool(seat_rows) and all(all(view == VIEW_OBSERVE for view in row["views"][:human_seats]) for row in seat_rows),
              f"views after the deaths={[row['views'] for row in seat_rows][:4]}",
              [out / "e2e/brainless_spectate" / peer / "stdout.log"])
        # The end rule itself, on both peers: the dead human seat either watches the AI sides fight on
        # or takes the round down with it. Only a roster that leaves two sides standing can tell them
        # apart, so the check runs on the dedicated row and says why when it cannot.
        kill_ms = int(kills[0][0]) if kills else 0
        after = [row for row in rows[peer] if row["simms"] >= kill_ms]
        first_after = after[0] if after else None
        running = [row for row in after if row["state"] != ACTIVITY_OVER]
        if not dedicated:
            check(checks, f"{peer}_round_end_rule_not_provable", True,
                  "a two-human PvPvE roster leaves one side standing, so both rules end the round here",
                  [out / "e2e/brainless_spectate" / peer / "stdout.log"])
        elif rule_on:
            check(checks, f"{peer}_round_ran_on",
                  bool(first_after) and first_after["state"] != ACTIVITY_OVER and len(running) >= 5,
                  f"first row after the death={first_after} running rows={len(running)}",
                  [out / "e2e/brainless_spectate" / peer / "stdout.log"])
        else:
            check(checks, f"{peer}_round_ended",
                  bool(first_after) and first_after["state"] == ACTIVITY_OVER and first_after["winner"] >= 0,
                  f"first row after the death={first_after}",
                  [out / "e2e/brainless_spectate" / peer / "stdout.log"])
        # The host's flag is the match rule on BOTH machines, against both local settings. Only the
        # rows of a RUNNING match say that: once the round is over the lockstep sync is gone and the
        # query answers this machine's own setting again, which is the designed behaviour.
        running_rows = [row for row in rows[peer] if row["state"] != ACTIVITY_OVER]
        check(checks, f"{peer}_follows_host_rule",
              bool(running_rows) and all(row["rule"] == expected_rule for row in running_rows),
              f"local setting={'1' if local_setting else '0'} rule rows in the running match="
              f"{sorted({row['rule'] for row in running_rows})} after the end="
              f"{sorted({row['rule'] for row in rows[peer] if row['state'] == ACTIVITY_OVER})}",
              [out / "e2e/brainless_spectate" / peer / "stdout.log"])
    # The view fields are deliberately per-machine (each peer watches its own seat), so the peers
    # agree on the SIM fields; the local view is evidence for the sp_view_is_local arm instead.
    sim_rows = {peer: [{key: value for key, value in row.items() if key not in ("scroll", "views")}
                       for row in peer_rows] for peer, peer_rows in rows.items()}
    check(checks, "peers_agree", bool(sim_rows.get("host")) and sim_rows.get("host") == sim_rows.get("client"),
          f"host rows={len(sim_rows.get('host', []))} client rows={len(sim_rows.get('client', []))}",
          [out / "e2e/brainless_spectate"])
    result["spectate_checks"] = checks
    result["spectate_pass"] = all(row["status"] == "pass" for row in checks)
    return result


def sp_arm(out: Path, setting_on: bool, cycle_input: bool, ticks: int, timeout: float, kill_follow_ms: int = 0) -> dict:
    """One process on the same stock script, the Gameplay setting deciding the end rule."""
    sys.path.insert(0, str(REPO / "tools"))
    from run_sim_test import prepare_runtime  # noqa: E402
    from win32_test_runner import IsolatedRun  # noqa: E402

    out.mkdir(parents=True, exist_ok=False)
    runtime = prepare_runtime(REPO, out)
    stage_user_module(runtime, setting_on, arm_trace=True, kill_follow_ms=kill_follow_ms)

    # Run past the end so BOTH arms report the same number of rows: the round that ends prints its
    # Over rows instead of simply stopping, which is what tells the two arms apart.
    flags = ["-scenario", PRESET, "-seed", "42", "-max-ticks", str(ticks),
             "-scenario-run-past-end", "-tick-hashes", "-out", str(out / "trace.json")]
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
    kills = KILL.findall(text)
    checks: list[dict] = []
    check(checks, "brain_destroyed", bool(kills), f"kill lines={len(kills)}", [out / "stdout.log"])
    # The run's tick cap ends the activity itself, so the last row is Over in both arms. What tells
    # them apart is the FIRST row after the brain died and how many rows the round then survived.
    kill_ms = int(kills[0][0]) if kills else 0
    after = [row for row in rows if row["simms"] >= kill_ms]
    first_after = after[0] if after else None
    check(checks, "rows_after_the_death", bool(after), f"rows past the kill={len(after)}",
          [out / "stdout.log"])
    if setting_on:
        # Only the spectating arm proves anything here: a round that is Over puts every seat in
        # Observe anyway, so the check would pass on the ended round without meaning it.
        check(checks, "seat_observes", bool(after) and all(row["views"][0] == VIEW_OBSERVE for row in after),
              f"seat 0 view after the death={[row['views'][0] for row in after][:8]}", [out / "stdout.log"])
        running = [row for row in after if row["state"] != ACTIVITY_OVER]
        check(checks, "round_ran_on",
              bool(first_after) and first_after["state"] != ACTIVITY_OVER and len(running) >= 5,
              f"first row after the death={first_after} running rows={len(running)}", [out / "stdout.log"])
    else:
        check(checks, "round_ended", bool(first_after) and first_after["state"] == ACTIVITY_OVER,
              f"first row after the death={first_after}", [out / "stdout.log"])
        check(checks, "winner_named", bool(first_after) and first_after["winner"] >= 0,
              f"winner={first_after['winner'] if first_after else None}", [out / "stdout.log"])
    if kill_follow_ms:
        # The follow name is gone once the spectated unit dies; the engine prints the clear.
        check(checks, "follow_label_empty_after_death",
              "[spectate-follow] kill " in text and "[spectate-follow] cleared player=" in text,
              "follow kill and clear lines", [out / "stdout.log"])
    exit_ok = record.get("exit_code") == 0 and not record.get("timed_out")
    check(checks, "process_completed", exit_ok,
          f"exit_code={record.get('exit_code')} timed_out={record.get('timed_out')}", [out / "stdout.log"])
    return {"exe": str(exe), "exe_sha256": sha256(exe), "argv": argv, "out": str(out),
            "exit_code": record.get("exit_code"), "timed_out": record.get("timed_out"),
            "trace": str(out / "trace.json"), "probe_rows": rows,
            "checks": checks, "pass": all(row["status"] == "pass" for row in checks)}


def main() -> int:
    global KILL_MS
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("arm", choices=("net_rule_on", "net_rule_off", "sp_keep_playing",
                                        "sp_end_match", "sp_view_is_local"))
    parser.add_argument("--out", type=Path, default=None)
    parser.add_argument("--port", type=int, default=PORT_RANGE.start)
    parser.add_argument("--ticks", type=int, default=900)
    parser.add_argument("--timeout", type=float, default=420)
    parser.add_argument("--kill-ms", type=int, default=KILL_MS,
                        help="sim time of the brain kill; 0 is the control arm where nobody dies")
    parser.add_argument("--sim-dump", default=None,
                        help="CC_SIM_DUMP=<from>:<to> for both net peers; forensics only, no oracle effect")
    parser.add_argument("--two-humans", action="store_true",
                        help="net arms: seat both peers instead of hosting seatlessly; the end rule is then unprovable")
    parser.add_argument("--delay", type=int, default=0, help="net arms: the match input delay both peers launch with")
    args = parser.parse_args()
    if args.port not in PORT_RANGE:
        parser.error(f"port is outside {PORT_RANGE.start}..{PORT_RANGE.stop - 1}")
    KILL_MS = args.kill_ms
    refuse_on_locks()
    out = (args.out or SCRATCH / (args.arm + "-" + time.strftime("%Y%m%d-%H%M%S"))).resolve()
    out.mkdir(parents=True, exist_ok=True)
    os.environ["CCCP_HEADLESS"] = "1"
    os.chdir(REPO)

    manifest = {"stamp": stamp(), "arm": args.arm, "port": args.port, "ticks": args.ticks,
                "repo": str(REPO), "exe_sha256": sha256(REPO / "Cortex Command.exe"),
                "driver_sha256": sha256(__file__), "fixture_sha256": sha256(FIXTURE),
                "kill_ms": KILL_MS, "sim_dump": args.sim_dump, "dedicated": not args.two_humans,
                "input_delay": args.delay}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")

    if args.arm.startswith("net_"):
        result = net_arm(out, args.port, args.arm == "net_rule_on", args.ticks, args.timeout, args.sim_dump,
                         dedicated=not args.two_humans, delay=args.delay)
        passed = bool(result.get("pass", True)) and result.get("spectate_pass", False)
    elif args.arm == "sp_view_is_local":
        plain = sp_arm(out / "plain", True, False, args.ticks, args.timeout)
        # Cycle onto a unit, then gib it past 9 s so the follow label's empty-after-death line can fire.
        cycled = sp_arm(out / "cycled", True, True, args.ticks, args.timeout, kill_follow_ms=9000)
        compare = subprocess.run(
            [sys.executable, str(REPO / "tools/compare_sim_traces.py"), plain["trace"], cycled["trace"],
             "--expected-ticks", str(args.ticks)],
            capture_output=True, text=True)
        (out / "compare.log").write_text(compare.stdout + compare.stderr, encoding="utf-8")
        # Identical hashes only prove "no sim writes" if the cycling input actually reached the
        # spectator view: the followed unit moves the scroll target away from the plain run's.
        plain_scroll = [row["scroll"] for row in plain["probe_rows"]]
        cycled_scroll = [row["scroll"] for row in cycled["probe_rows"]]
        view_checks: list[dict] = []
        check(view_checks, "cycling_moved_the_view", plain_scroll != cycled_scroll and bool(cycled_scroll),
              f"plain scroll={plain_scroll[-4:]} cycled scroll={cycled_scroll[-4:]}",
              [plain["out"], cycled["out"]])
        check(view_checks, "sim_rows_identical",
              [{k: v for k, v in row.items() if k != "scroll"} for row in plain["probe_rows"]] ==
              [{k: v for k, v in row.items() if k != "scroll"} for row in cycled["probe_rows"]],
              "every probe field except the local view is identical", [plain["out"], cycled["out"]])
        result = {"plain": plain, "cycled": cycled, "compare_exit": compare.returncode,
                  "compare_log": str(out / "compare.log"), "view_checks": view_checks}
        passed = (plain["pass"] and cycled["pass"] and compare.returncode == 0
                  and all(row["status"] == "pass" for row in view_checks))
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
