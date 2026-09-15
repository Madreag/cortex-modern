"""The in-match pause menu over a running network session.

Every arm launches real peers through run_sim_test's private runtimes and drives the unchanged
network UI probe. The lifecycle case asserts that escape opens the pause menu on each peer while
the match keeps running (no activity pause, no leave), that settings and resume round-trip, that
the synchronized pause and the explicit leave still produce their existing lines, and that a
single-player pause is unchanged.

The desync arms drive no menu: they are the end-to-end gate for the runtime desync check itself.
`desync` perturbs the client's sim once and requires both peers to stop with the named reason
within one sample interval; `desync_clean` requires the same wire to run silently and be counted.
"""

import argparse
import hashlib
import json
import os
import re
import sys
import threading
from pathlib import Path

SCRATCH = Path("D:/mx/opus-l03-pause-session-20260914")
PORT_BASE = 48350
PORT_SPAN = 20
# The match must outlast the probe script on a loaded machine: a step costs a rendered frame, which a
# contended peer produces far more slowly than the fixed timestep produces ticks.
TICKS = 1800
# The desync arms drive no menu, so they need only enough ticks to clear a few sample intervals.
ARM_TICKS = {"desync": 300, "desync_clean": 300}
# The runtime desync check's sample interval, Main.cpp c_DesyncCheckIntervalTicks.
DESYNC_INTERVAL = 30
PERTURB_TICK = 50
SIZES = ((640, 360), (960, 540))
FIRST_SIZE_ONLY = ("peers4", "resync", "pad", "desync", "desync_clean")
MATCH_ROWS = ("ButtonLeaveMatch", "ButtonPauseMatch", "ButtonSettings", "ButtonSaveDiagnostics", "ButtonResume")
SINGLE_PLAYER_ROWS = ("ButtonBackToMain", "ButtonSaveOrLoadGame", "ButtonModManager")
LEFT_LOCAL = "NETWORK: Match left"
LEFT_REMOTE = re.compile(r"^\[net-match\] .* left the match at frame (\d+) ")
PAUSED = re.compile(r"^\[net-match\] match paused at tick (\d+) sim ms \d+$")
RESUMED = re.compile(r"^\[net-match\] match resumed at tick (\d+) sim ms \d+$")
ACTIVITY_PAUSED = re.compile(r'^SYSTEM: Activity "[^"]*" was paused$')
DESYNC_STOP = re.compile(r"Desync:sim state diverged at tick (\d+)")


def sha(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def read_log(out):
    # The pause and leave notices are console lines, so the engine's own console log is part of the log.
    sources = (out / "stdout.log", out / "stderr.log", out / "runtime" / "LogConsole.txt")
    return "\n".join(path.read_text(encoding="utf-8", errors="replace") for path in sources if path.exists())


def menu_step(command, accepted=True):
    return {"op": "menu", "command": command, "accepted": accepted}


def escape():
    return [{"op": "key_down", "key": "Escape"}, {"op": "key_up", "key": "Escape"}]


def live_menu_assertions(tag):
    """The match rows, their metrics inside the panel and the viewport, and the rows that are gone."""
    steps = []
    for row in MATCH_ROWS:
        steps += [menu_step(f"assert_visible {row} 1"), menu_step(f"assert_rect_inside {row} PauseScreen"),
                  menu_step(f"assert_rect_inside {row} viewport"), menu_step(f"assert_text_fits {row}")]
    for row in SINGLE_PLAYER_ROWS:
        steps += [menu_step(f"assert_visible {row} 0")]
    return steps + [menu_step("dump_host_options"), {"op": "screenshot", "name": tag}]


def pad(edge):
    return {"op": f"pad_{edge}", "button": "start"}


def pad_steps(tag):
    """The pad's start button on the same menu: it opens it, closes it and cancels the confirmation."""
    # The pad joins on its first step; the press itself is the next step's one-frame edge.
    steps = [{"op": "wait", "service": "Running"}, {"op": "wait", "sim_at_least": 200},
             pad("up"), {"op": "wait", "renders": 4},
             pad("down"), {"op": "wait", "screen": "Pause"}, pad("up"),
             {"op": "assert", "equals": {"screen": "Pause", "service": "Running", "paused": False}, "sim_at_least": 200}]
    steps += live_menu_assertions(f"{tag}_open")
    steps += [pad("down"), {"op": "wait", "screen": "Gameplay"}, pad("up"),
              {"op": "assert", "equals": {"screen": "Gameplay", "service": "Running", "paused": False}},
              {"op": "wait", "sim_at_least": 360},
              pad("down"), {"op": "wait", "screen": "Pause"}, pad("up"),
              menu_step("post_command ButtonLeaveMatch"), {"op": "wait", "screen": "PauseLeaveConfirm"},
              {"op": "screenshot", "name": f"{tag}_confirm"},
              # The pad cancels the confirmation: the seat stays in the match it was asked about.
              pad("down"), {"op": "wait", "screen": "Pause"}, pad("up"),
              {"op": "assert", "equals": {"screen": "Pause", "service": "Running", "paused": False}},
              menu_step("assert_visible ButtonResume 1"),
              menu_step("post_command ButtonResume"), {"op": "wait", "screen": "Gameplay"},
              {"op": "assert", "equals": {"screen": "Gameplay", "service": "Running", "paused": False}, "sim_at_least": 360},
              {"op": "wait", "sim_at_least": 480}, {"op": "finish"}]
    return {"schema": 1, "timeout_ms": 180000, "steps": steps}


def lifecycle_steps(arm, who, tag):
    """The probe script of one peer. Every arm opens the local menu the same way."""
    if arm == "pad":
        return pad_steps(tag)
    steps = [{"op": "wait", "service": "Running"}, {"op": "wait", "sim_at_least": 200}]
    steps += escape()
    steps += [{"op": "wait", "screen": "Pause"},
              {"op": "assert", "equals": {"screen": "Pause", "service": "Running", "paused": False}, "sim_at_least": 200}]
    steps += live_menu_assertions(f"{tag}_open")
    if arm in ("menu", "peers4", "resync"):
        steps += [menu_step("activate ButtonSettings"), {"op": "wait", "screen": "PauseSettings"},
                  menu_step("assert_visible CollectionBoxGameplaySettings 1"), menu_step("dump_player_options"),
                  {"op": "screenshot", "name": f"{tag}_settings"},
                  menu_step("post_command ButtonBackToMainMenu"), {"op": "wait", "screen": "Pause"},
                  {"op": "assert", "equals": {"screen": "Pause", "service": "Running", "paused": False}},
                  {"op": "wait", "sim_at_least": 360},
                  menu_step("post_command ButtonResume"), {"op": "wait", "screen": "Gameplay"},
                  {"op": "assert", "equals": {"screen": "Gameplay", "service": "Running", "paused": False}, "sim_at_least": 360},
                  {"op": "wait", "sim_at_least": 480}, {"op": "finish"}]
    elif arm == "pause":
        # The row follows the shared state, and a resume is a countdown, so each press waits for the state itself.
        steps += [menu_step("post_command ButtonPauseMatch"),
                  {"op": "wait", "scope": "menu", "control": "ButtonPauseMatch", "equals": {"visible": True, "text": "resume match"}},
                  {"op": "assert_control", "scope": "menu", "control": "ButtonPauseMatch", "text_contains": "resume match",
                   "equals": {"visible": True}, "fits": True},
                  {"op": "screenshot", "name": f"{tag}_paused"},
                  {"op": "assert", "equals": {"screen": "Pause", "service": "Running", "paused": False}},
                  menu_step("post_command ButtonPauseMatch"),
                  {"op": "wait", "scope": "menu", "control": "ButtonPauseMatch", "equals": {"visible": True, "text": "pause match"}},
                  {"op": "assert_control", "scope": "menu", "control": "ButtonPauseMatch", "text_contains": "pause match",
                   "equals": {"visible": True}, "fits": True},
                  menu_step("post_command ButtonResume"), {"op": "wait", "screen": "Gameplay"},
                  {"op": "wait", "sim_at_least": 480}, {"op": "finish"}]
    elif arm == "leave":
        # The confirmation replaces the rows; its text names what this seat loses when it leaves.
        steps += [menu_step("post_command ButtonLeaveMatch"), {"op": "wait", "screen": "PauseLeaveConfirm"},
                  menu_step("assert_visible LabelLeaveConfirm 1"), menu_step("assert_rect_inside LabelLeaveConfirm LeaveConfirmBox"),
                  menu_step("assert_text_fits LabelLeaveConfirm"), menu_step("assert_text_fits ButtonLeaveConfirm"),
                  menu_step("assert_text_fits ButtonLeaveCancel"), menu_step("assert_visible ButtonResume 0"),
                  {"op": "assert_control", "scope": "menu", "control": "LabelLeaveConfirm",
                   "text_contains": "match ends for everyone" if who == "host" else "fall to a teammate or to the AI",
                   "equals": {"visible": True}},
                  menu_step("dump_host_options"), {"op": "screenshot", "name": f"{tag}_confirm"},
                  # Cancel first: the confirmation must be escapable without touching the session.
                  menu_step("post_command ButtonLeaveCancel"), {"op": "wait", "screen": "Pause"},
                  {"op": "assert", "equals": {"screen": "Pause", "service": "Running", "paused": False}},
                  menu_step("post_command ButtonLeaveMatch"), {"op": "wait", "screen": "PauseLeaveConfirm"},
                  {"op": "signal", "name": "leaving"}, menu_step("post_command ButtonLeaveConfirm"),
                  # The engine's own leave ends this process; the script parks so the probe does not end it first.
                  {"op": "wait", "service": "Idle"}]
    else:
        raise ValueError(arm)
    # The probe refuses a deadline over its own 180 s bound.
    return {"schema": 1, "timeout_ms": 180000, "steps": steps}


def single_player_steps(tag):
    return {"schema": 1, "timeout_ms": 180000, "steps": [
        {"op": "wait", "sim_at_least": 200}, *escape(), {"op": "wait", "screen": "Pause"},
        {"op": "assert", "equals": {"screen": "Pause", "paused": True}},
        menu_step("assert_visible ButtonBackToMain 1"), menu_step("assert_visible ButtonSaveOrLoadGame 1"),
        menu_step("assert_visible ButtonPauseMatch 0"), menu_step("assert_visible ButtonLeaveMatch 0"),
        menu_step("dump_host_options"), {"op": "screenshot", "name": f"{tag}_sp"},
        {"op": "signal", "name": "done"}, {"op": "finish"}]}


def peer_names(arm):
    if arm == "peers4":
        return ("host", "client1", "client2", "client3")
    return ("host", "client")


def arm_ticks(arm):
    return ARM_TICKS.get(arm, TICKS)


def peer_args(arm, who, port, root, peers):
    args = ["-net-match-service-e2e", "-net-port", str(port), "-net-match-peers", str(peers),
            "-net-match-ticks", str(arm_ticks(arm)), "-net-match-input-delay", "3", "-net-autosave-seconds", "0",
            "-tick-hashes", "-out", str(root / f"{who}_trace.json"),
            "-net-match-report", str(root / f"{who}_report.json")]
    args += ["-net-host"] if who == "host" else ["-net-join", "127.0.0.1"]
    if arm == "resync":
        # The detector's own perturbation on one peer is what makes a real resync happen mid-match.
        args += ["-net-match-e2e-resync"] + (["-determinism-selftest-perturb"] if who == "client" else [])
    if arm == "desync":
        # No heal here: the divergence must surface as a named stop on both peers, not be repaired.
        args += ["-determinism-selftest-perturb"] if who == "client" else []
    return args


def probe_owners(arm):
    """Which peers drive the menu. The menu arm opens on every peer; the others open on one."""
    if arm in ("desync", "desync_clean"):
        return ()
    if arm in ("menu", "peers4"):
        return peer_names(arm)
    if arm == "leave":
        return ("client",)
    return ("host",)


def run_arm(make_run, set_visual_resolution, repo, arm, root, port, size, timeout):
    root.mkdir(parents=True, exist_ok=False)
    peers = peer_names(arm)
    runs, records, argv = {}, {}, {}
    try:
        for who in peers:
            args = peer_args(arm, who, port, root, len(peers))
            env = {"CCCP_HEADLESS": "1"}
            if who in probe_owners(arm):
                # One directory per peer: the probe refuses to start where a result already exists.
                inputs = root / f"{who}_inputs"
                inputs.mkdir()
                probe = inputs / "probe.json"
                probe.write_text(json.dumps(lifecycle_steps(arm, who, f"{arm}_{size[0]}x{size[1]}_{who}"), indent=2) + "\n", encoding="utf-8")
                env["CC_TEST_NET_UI_SCRIPT"] = str(probe)
            argv[who] = args
            runs[who] = make_run(repo, args, root / who, timeout, env=env)
            set_visual_resolution(runs[who], *size)

        def drive(who):
            try:
                records[who] = runs[who].start().finish()
            except Exception as error:
                records[who] = {"error": repr(error)}

        threads = [threading.Thread(target=drive, args=(who,), daemon=True) for who in peers]
        threads[0].start()
        threading.Event().wait(2.0)
        for thread in threads[1:]:
            thread.start()
        for thread in threads:
            thread.join()
    finally:
        for run in runs.values():
            run.close()
    return {"peers": list(peers), "argv": argv, "records": records,
            "logs": {who: str(root / who) for who in peers}}


def run_single_player(make_run, set_visual_resolution, repo, root, size, timeout):
    root.mkdir(parents=True, exist_ok=False)
    inputs = root / "sp_inputs"
    inputs.mkdir()
    script = root / "menu.txt"
    # The menu loop pumps the probe's drawn steps through the menu script; it leaves once they signal.
    script.write_text(f"wait_file {inputs / 'done.json'} 120\nexit\n", encoding="utf-8")
    probe = inputs / "probe.json"
    probe.write_text(json.dumps(single_player_steps(f"sp_{size[0]}x{size[1]}"), indent=2) + "\n", encoding="utf-8")
    args = ["-scenario", "SimBaseline", "-seed", "42", "-max-ticks", str(TICKS),
            "-menu-script", str(script), "-out", str(root / "sp_trace.json")]
    run = make_run(repo, args, root / "sp", timeout, env={"CCCP_HEADLESS": "1", "CC_TEST_NET_UI_SCRIPT": str(probe)})
    set_visual_resolution(run, *size)
    try:
        record = run.start().finish()
    except Exception as error:
        record = {"error": repr(error)}
    finally:
        run.close()
    return {"peers": ["sp"], "argv": {"sp": args}, "records": {"sp": record}, "logs": {"sp": str(root / "sp")}}


def probe_result(root, who):
    result = root / f"{who}_inputs" / "net-ui-result.json"
    return json.loads(result.read_text(encoding="utf-8")) if result.exists() else {}


def peer_report(root, who):
    report = root / f"{who}_report.json"
    return json.loads(report.read_text(encoding="utf-8")) if report.exists() else {}


def executed_commands(probe):
    """The script commands the probe actually ran, in order."""
    script = probe.get("script", {}).get("steps", [])
    return [script[step["index"]].get("command", step["op"]) for step in probe.get("steps", [])
            if 0 <= step.get("index", -1) < len(script)]


def inspect(arm, root, outcome, strict_compare):
    checks, details = {}, {"records": outcome["records"], "probes": {}, "lines": {}}
    logs = {who: read_log(root / who) for who in outcome["peers"]}
    for who in outcome["peers"]:
        record = outcome["records"].get(who, {})
        checks[f"{who}_ran"] = record.get("timed_out") is False and record.get("exit_code") is not None
        checks[f"{who}_no_fatal"] = "FATAL" not in logs[who]
        details["lines"][who] = [line for line in logs[who].splitlines()
                                 if LEFT_LOCAL in line or LEFT_REMOTE.match(line) or PAUSED.match(line)
                                 or RESUMED.match(line) or ACTIVITY_PAUSED.match(line) or DESYNC_STOP.search(line)]
    if arm == "sp":
        # The menu script quits from the pause menu before the scenario's tick budget, so the run ends on
        # the scenario's own unfinished verdict; a crash or a hang would not land there.
        checks["sp_exit_is_the_scenario_verdict"] = (outcome["records"]["sp"].get("exit_code") == 1
                                                     and outcome["records"]["sp"].get("timed_out") is False)
        checks["sp_activity_paused"] = any(ACTIVITY_PAUSED.match(line) for line in logs["sp"].splitlines())
        checks["sp_no_menu_failure"] = "[menu-script] FAILED" not in logs["sp"]
    elif arm in ("desync", "desync_clean"):
        # The runtime desync check end to end: both peers hash on the interval, put it on the wire and
        # act on the mismatch. The counters are what makes a dead exchange countable instead of silent.
        expected = arm_ticks(arm) // DESYNC_INTERVAL
        stops = {}
        mismatches = 0
        for who in outcome["peers"]:
            report = peer_report(root, who)
            counters = report.get("desync_check") or {}
            details.setdefault("desync_check", {})[who] = counters
            details.setdefault("runtime_error", {})[who] = report.get("runtime_error")
            mismatches += counters.get("mismatches", 0)
            # The input-delay ramp-in can swallow the first sample; every later one must be there.
            checks[f"{who}_submitted"] = counters.get("submissions", 0) >= expected - 1
            checks[f"{who}_sent"] = counters.get("sends", 0) >= expected - 1
            checks[f"{who}_compared"] = counters.get("compares", 0) >= expected - 1
            stop = DESYNC_STOP.search(report.get("runtime_error") or "")
            stops[who] = int(stop[1]) if stop else None
            if arm == "desync":
                checks[f"{who}_stopped_on_desync"] = stop is not None
                checks[f"{who}_stop_line_logged"] = DESYNC_STOP.search(logs[who]) is not None
                checks[f"{who}_exit"] = outcome["records"][who].get("exit_code") == 1
                # Caught within one sample interval of the perturbed tick, and never before it.
                checks[f"{who}_caught_within_interval"] = bool(
                    stop and PERTURB_TICK < stops[who] <= PERTURB_TICK + DESYNC_INTERVAL)
                checks[f"{who}_trace_written"] = (root / f"{who}_trace.json").exists()
            else:
                checks[f"{who}_no_desync"] = stop is None and DESYNC_STOP.search(logs[who]) is None
                checks[f"{who}_exit"] = outcome["records"][who].get("exit_code") == 0
                checks[f"{who}_no_mismatch"] = counters.get("mismatches", 0) == 0
        details["desync_stop_ticks"] = stops
        if arm == "desync":
            # Whoever compares first counts the mismatch; the other may take its stop off the wire.
            checks["mismatch_counted"] = mismatches >= 1
            checks["peers_named_one_tick"] = len(set(stops.values())) == 1 and None not in stops.values()
    else:
        for who in outcome["peers"]:
            if arm != "leave":
                # Nothing here may leave the match or pause the local activity.
                checks[f"{who}_activity_never_paused"] = not any(ACTIVITY_PAUSED.match(line) for line in logs[who].splitlines())
                checks[f"{who}_exit"] = outcome["records"][who].get("exit_code") == 0
                checks[f"{who}_stayed"] = LEFT_LOCAL not in logs[who] and not any(LEFT_REMOTE.match(line) for line in logs[who].splitlines())
        if arm == "pause":
            pauses = {who: [int(match[1]) for line in logs[who].splitlines() if (match := PAUSED.match(line))] for who in outcome["peers"]}
            resumes = {who: [int(match[1]) for line in logs[who].splitlines() if (match := RESUMED.match(line))] for who in outcome["peers"]}
            details["pause_ticks"], details["resume_ticks"] = pauses, resumes
            checks["pause_on_every_peer"] = all(len(value) == 1 for value in pauses.values()) and len(set(map(tuple, pauses.values()))) == 1
            checks["resume_on_every_peer"] = all(len(value) == 1 for value in resumes.values()) and len(set(map(tuple, resumes.values()))) == 1
        if arm == "leave":
            checks["leaver_left"] = LEFT_LOCAL in logs["client"]
            checks["host_saw_the_leave"] = any(LEFT_REMOTE.match(line) for line in logs["host"].splitlines())
            checks["leave_was_signalled"] = (root / "client_inputs" / "leaving.json").exists()
    for who in outcome["peers"]:
        probe = probe_result(root, who)
        if not probe:
            continue
        details["probes"][who] = {"error": probe.get("error"), "failed_step": probe.get("failed_step"),
                                  "commands": executed_commands(probe)}
        if arm == "leave":
            # The process quits on the confirmed leave, so the last recorded command is the oracle.
            checks[f"{who}_probe_confirmed_leave"] = "post_command ButtonLeaveConfirm" in executed_commands(probe)
        else:
            checks[f"{who}_probe_complete"] = probe.get("pass") is True and probe.get("complete") is True
    if arm in ("menu", "peers4", "pad"):
        first = outcome["peers"][0]
        for other in outcome["peers"][1:]:
            # The capped match hashes its completion tick too, so the trace holds one tick more than the cap.
            ok, compared = strict_compare(root / f"{first}_trace.json", root / f"{other}_trace.json", expected_ticks=TICKS + 1)
            checks[f"hashes_{first}_{other}"] = ok
            details.setdefault("peer_hashes", {})[other] = compared
    return {"pass": all(checks.values()), "checks": checks, "details": details}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--case", choices=("lifecycle",), required=True)
    parser.add_argument("--exe-sha256", required=True)
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--arms", nargs="*",
                        default=["menu", "pause", "leave", "sp", "peers4", "resync", "pad", "desync", "desync_clean"])
    # A later lane runs the same arms from its own scratch root and its own ports.
    parser.add_argument("--scratch-root", type=Path, default=SCRATCH)
    parser.add_argument("--port-base", type=int, default=PORT_BASE)
    options = parser.parse_args()
    if Path("D:/mx/LEAD_FAMILY.lock").exists():
        parser.error("verification family owns the machine; no driver may start")
    repo, root = options.repo.resolve(), options.out.resolve()
    scratch = options.scratch_root.resolve()
    ports = range(options.port_base, options.port_base + PORT_SPAN)
    if not root.is_relative_to(scratch) or root == scratch:
        parser.error(f"--out must name a fresh run beneath {scratch}")
    sys.path.insert(0, str(repo / "tools"))
    from compare_sim_traces import strict_compare
    from run_sim_test import make_run
    from test_telemetry_bundle import set_visual_resolution
    os.environ["CCCP_HEADLESS"] = "1"
    root.mkdir(parents=True, exist_ok=False)
    exe_sha = sha(repo / "Cortex Command.exe")
    result = {"pass": False, "case": options.case, "checks": {}, "arms": {}, "ticks_per_run": TICKS,
              "arm_ticks": ARM_TICKS, "ports": list(ports), "driver_sha256": sha(__file__), "exe_sha256": exe_sha,
              "peer_hash_scope": "unchanged strict_compare: controller excluded; every other subsystem at every tick"}
    try:
        if exe_sha.lower() != options.exe_sha256.lower():
            raise RuntimeError(f"the requested executable hash does not match: {exe_sha}")
        index = 0
        for size in SIZES:
            for arm in options.arms:
                if arm in FIRST_SIZE_ONLY and size != SIZES[0]:
                    continue
                name = f"{arm}_{size[0]}x{size[1]}"
                port = ports[index % len(ports)]
                index += 1
                arm_root = root / name
                if arm == "sp":
                    outcome = run_single_player(make_run, set_visual_resolution, repo, arm_root, size, options.timeout)
                else:
                    outcome = run_arm(make_run, set_visual_resolution, repo, arm, arm_root, port, size, options.timeout)
                inspected = inspect(arm, arm_root, outcome, strict_compare)
                inspected["port"] = port
                result["arms"][name] = inspected
                result["checks"][name] = inspected["pass"]
                (root / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        result["pass"] = bool(result["checks"]) and all(result["checks"].values())
    except Exception as error:
        result["error"] = str(error)
    finally:
        after = sha(repo / "Cortex Command.exe")
        if after != exe_sha:
            result["pass"] = False
            result["error"] = "the executable changed during the driver"
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"pass": result["pass"], "error": result.get("error"), "out": str(root), "exe_sha256": exe_sha}), flush=True)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
