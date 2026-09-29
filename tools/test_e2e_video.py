"""The e2e video driver's own rows: scenario shape, token substitution, checklist resolution, encode commands.

    python tools/test_e2e_video.py --repo <tree> --out <dir>

Nothing here launches the engine or touches a capture: every row runs against a synthetic frame index in
its own temporary directory, so this suite is safe to run on a machine that is building. The rows that need
a real capture are named at the bottom of the result and are the driver's own first run's job.
"""

import argparse
import contextlib
import io
import json
import os
import re
import shutil
from pathlib import Path
import sys
import tempfile
import threading
from types import SimpleNamespace
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import e2e_video as driver  # noqa: E402

SCENARIOS = ("sp-smoke", "mp-host-join", "mp-reconnect-repair", "world-late-join", "ui-surfaces",
             "mod-void-wanderers", "mp-leave", "mp-rematch", "mp-rollback-lag",
             "mp-moderation", "mp-host-migration", "mp-resume-from-disk", "mp-direct-vs-relay",
             "mod-void-wanderers-multiplayer", "mp-host-join-cross")


def row(results, name, ok, detail=""):
    results.append({"row": name, "pass": bool(ok), "detail": detail})
    print(f"[e2e-video] {'PASS' if ok else 'FAIL'} {name}{': ' + detail if detail else ''}", flush=True)
    return ok


def check_capture_binary(results, scratch):
    platform = sys.platform
    previous = os.environ.pop("CCCP_TEST_BINARY", None)
    try:
        sys.platform = "win32"
        ok = row(results, "manifest/windows-runner-binary", driver.capture_binary(scratch) == scratch.resolve() / "Cortex Command.exe")
        sys.platform = "darwin"
        ok &= row(results, "manifest/posix-default-binary", driver.capture_binary(scratch) == scratch.resolve() / "build-gns/CortexCommand")
        os.environ["CCCP_TEST_BINARY"] = str(scratch / "custom-engine")
        ok &= row(results, "manifest/posix-override-binary", driver.capture_binary(scratch) == (scratch / "custom-engine").resolve())
        return ok
    finally:
        sys.platform = platform
        if previous is None:
            os.environ.pop("CCCP_TEST_BINARY", None)
        else:
            os.environ["CCCP_TEST_BINARY"] = previous


def check_scratch_limit(results, scratch):
    resolver = getattr(driver, "scratch_limit", None)
    if resolver is None:
        return row(results, "scratch/selected-limit-is-supported", False)
    options = SimpleNamespace(scratch_limit_bytes=None)
    ok = row(results, "scratch/default-five-billion", resolver(options) == 5_000_000_000)
    retained = {"scratch_limit_bytes": 8_000_000_000}
    ok &= row(results, "scratch/finalizer-retains-selected-limit", resolver(options, retained) == 8_000_000_000)
    options.scratch_limit_bytes = 7_000_000_000
    ok &= row(results, "scratch/explicit-finalizer-limit", resolver(options, retained) == 7_000_000_000)
    for value in (0, -1, "0", "-1", "1.5", "invalid"):
        try:
            driver.positive_bytes(value)
            rejected = False
        except argparse.ArgumentTypeError:
            rejected = True
        ok &= row(results, f"scratch/rejects-{value}", rejected)
    ok &= row(results, "scratch/positive-byte-count", driver.positive_bytes("8000000000") == 8_000_000_000)
    for limit, expected in ((8_000_000_000, False), (6_000_000_000, True), (5_000_000_000, True)):
        with patch.object(driver, "scratch_bytes", return_value=6_000_000_000):
            try:
                driver.check_scratch_budget(scratch, limit)
                stopped = False
            except RuntimeError as error:
                stopped = str(limit) in str(error) and "no cleanup performed" in str(error)
        ok &= row(results, f"scratch/enforces-{limit}", stopped == expected)
    return ok


def check_render_arm(results, scratch):
    scenario = {"name": "render-arm", "path": "synthetic", "checklist": [], "size": "960x540",
                "peers": [{"name": "host", "args": [], "settings": {"ResolutionX": 800, "ResolutionY": 600,
                                                                    "NetworkShowDiagnostics": "1"}}]}
    options = SimpleNamespace(repo=scratch, size="1280x720", fps=3, port=49400, scratch_root=scratch,
                              setting=[("ResolutionX", "1920"), ("ResolutionX", "3840"),
                                       ("ResolutionY", "2160"), ("SessionDirectoryUrl", "")],
                              render_cap=0, dry_run=True)
    out = scratch / "render-plan"
    expected = {"ResolutionX": "3840", "ResolutionY": "2160", "NetworkShowDiagnostics": "1", "SessionDirectoryUrl": ""}
    try:
        with patch.object(driver, "make_run", side_effect=AssertionError("dry run constructed an engine")):
            plan = driver.run_one(options, scenario, {"name": "first"}, 0, out)
        peer = plan["peers"][0]
        ok = row(results, "render/dry-plan-seeds-command-line-last", peer["settings"] == expected and
                 peer["size"] == "3840x2160" and peer["render_cap"] == 0 and not out.exists())
        flag = peer["args"].index("-feel-render-settings")
        ok &= row(results, "render/dry-plan-private-render-flag", Path(peer["args"][flag + 1]) ==
                  out / "first/host/runtime/Userdata/FeelRender.ini")
    except (Exception, SystemExit) as error:
        ok = row(results, "render/dry-plan-seeds-command-line-last", False, str(error))
        ok &= row(results, "render/dry-plan-private-render-flag", False, str(error))

    for cap in ("0", "60", "144", "-1", "garbage"):
        stderr = io.StringIO()
        with patch.object(sys, "argv", ["e2e_video.py", "--list", "--render-cap", cap]), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(stderr):
            try:
                code = driver.main()
            except SystemExit as error:
                code = error.code
        expected_code = 0 if cap in ("0", "60") else 2
        ok &= row(results, f"render/cap-{cap}", code == expected_code and (expected_code == 0 or
                  "[feel] invalid render settings: expected RenderCapHz = 0 or 60" in stderr.getvalue()))

    handles = []

    class Handle:
        def __init__(self, repo, args, root, timeout, env):
            self.out, self.cwd = Path(root), Path(root) / "runtime"
            (self.cwd / "Userdata").mkdir(parents=True)
            (self.cwd / "Userdata/Settings.ini").write_text("Settings\n\tResolutionX = 960\n\tResolutionY = 540\n", encoding="utf-8")
            driver.write_json(self.out / "runtime.json", {"settings_overrides": {"EnableVSync": "0"}})
            self.argv = ["engine", *args]
            handles.append(self)
        def start(self): return self
        def finish(self): return {"exit_code": 0, "timed_out": False}
        def close(self): pass

    options.dry_run = False
    try:
        with patch.object(driver, "make_run", side_effect=Handle):
            capture_run = driver.run_one(options, scenario, {"name": "first"}, 0, scratch / "render-runtime")
        peer = capture_run["peers"][0]
        runtime = Path(peer["runtime"])
        ini = (runtime / "Userdata/Settings.ini").read_text(encoding="utf-8")
        ok &= row(results, "render/private-runtime-seeded", "ResolutionX = 3840" in ini and "ResolutionY = 2160" in ini and
                  (runtime / "Userdata/FeelRender.ini").read_text(encoding="utf-8") == "RenderCapHz = 0\n")
        ok &= row(results, "render/runner-gets-private-render-flag", handles[0].argv[-2:] ==
                  ["-feel-render-settings", str(runtime / "Userdata/FeelRender.ini")])
        document = driver.review(scenario, capture_run, Path(capture_run["root"]))
        capture = {"scenario": scenario["name"], "scenario_definition": scenario, "runs": [capture_run], "fps": 3,
                   "source": {}, "exe": {}, "started": "synthetic", "command": []}
        manifest = driver.scenario_manifest(capture, scratch / "render-runtime", 0)
        summary = driver.aggregate_review(capture, scratch / "render-runtime")
        for label, evidence in (("manifest", manifest["peers"][0]), ("review", document["peers"][0]),
                                ("summary", summary["peers"][0])):
            ok &= row(results, f"render/{label}-records-arm", evidence["size"] == "3840x2160" and
                      evidence["render_cap"] == 0 and all(evidence["settings"][key] == val for key, val in expected.items()))
    except (Exception, SystemExit) as error:
        ok &= row(results, "render/private-runtime-and-evidence", False, str(error))
    return ok


def check_module_requirements(results, scratch):
    repo = scratch / "module-requirements"
    module = repo / "Data/VoidWanderers.rte"
    module.mkdir(parents=True)
    (module / "Index.ini").write_text("DataModule\n\tSupportedGameVersion = 6.2.2\n", encoding="utf-8")
    ok = True
    for name in ("mod-void-wanderers", "mod-void-wanderers-multiplayer"):
        scenario = driver.load_scenario(name)
        ok &= row(results, f"{name}/version-warning-does-not-block-launch", not driver.requirement_findings(repo, scenario))
        missing = driver.requirement_findings(repo / "absent", scenario)
        ok &= row(results, f"{name}/missing-module-still-blocks", len(missing) == 1 and missing[0]["class"] == "data")
    exact = {"requires_version": [{"module": "VoidWanderers.rte", "version": "7.0.0", "reason": "Exact version required"}]}
    findings = driver.requirement_findings(repo, exact)
    ok &= row(results, "requirements/explicit-version-contract-is-preserved", len(findings) == 1 and
              findings[0]["declared"] == "6.2.2" and findings[0]["required"] == "7.0.0")
    return ok


# A scripted return, reclaim, hold or relaunch lands inside its round with room for the largest lever wait: the reclaim
# gap (neutral_through - activation, 78-90 frames measured on mp-rollback-lag) plus one full-state interval (60).
LEVER_MARGIN_TICKS = 90 + 60
# A killed peer's relaunch returns 741-873 frames after its drop with no fake lag (mp-join-garbage, mp-reconnect-repair,
# mp-host-stall-image-rejoin, full-state sampler on) and 1007-1375 behind 200 ms fake lag and 5 % loss (mp-rollback-lag
# lag-100), measured 2026-09-28 on 263 and on this branch.
RELAUNCH_BUDGET_TICKS = {False: 900, True: 1400}
TICK_LEVERS = ("-selftest-frame-stall", "-selftest-frame-stall-again", "-net-test-live-stall", "-selftest-draw-stall",
               "-selftest-late-script-stall", "-net-match-e2e-leave-tick", "-determinism-selftest-perturb-tick")
SCRIPTED_TICK_LINE = "[input-script] tick {} player 0 pressed FIRE"


def scheduled_gate_tick(gate, horizon):
    """The tick a kill gate fires at when the round's own clock schedules it; None for an event gate."""
    if "sim_tick" in gate:
        return int(gate["sim_tick"])
    pattern = gate.get("log")
    if not pattern or re.search(pattern, SCRIPTED_TICK_LINE.format(0)):
        return None
    return next((tick for tick in range(horizon) if re.search(pattern, SCRIPTED_TICK_LINE.format(tick))), None)


def lever_violations(scenario):
    """Every scripted lever whose landing leaves less than the margin before its round's final frame."""
    found = []
    for run in scenario.get("runs") or [{"name": "run0", "peers": scenario.get("peers", [])}]:
        peers = run.get("peers", [])
        rounds = {peer["name"]: int(peer["args"][peer["args"].index("-net-match-ticks") + 1])
                  for peer in peers if "-net-match-ticks" in peer.get("args", [])}
        if not rounds:
            continue
        last = max(rounds.values())
        kills = {}

        def late(label, landing, final):
            if landing + LEVER_MARGIN_TICKS > final:
                found.append(f"{scenario['name']}/{run.get('name', 'run0')}: {label} lands at {landing}, "
                             f"inside {LEVER_MARGIN_TICKS} of the round's final frame {final}")

        # A scenario whose subject is a peer frozen through the round's end declares it; that freeze must still start inside.
        frozen_through_end = set(run.get("frozen_through_end", []))
        for peer in peers:
            final, args = rounds.get(peer["name"], last), peer.get("args", [])
            for flag in TICK_LEVERS:
                for index in (i for i, arg in enumerate(args) if arg == flag and i + 1 < len(args)):
                    tick, _, ms = args[index + 1].partition(":")
                    if peer["name"] in frozen_through_end and flag == "-selftest-frame-stall":
                        if int(tick) > final:
                            found.append(f"{scenario['name']}/{run.get('name', 'run0')}: {peer['name']} freezes at {tick}, after the round's final frame {final}")
                        continue
                    late(f"{peer['name']} {flag} {args[index + 1]}", int(tick) + -(-int(ms or 0) * 60 // 1000), final)
            bounds = []
            if peer.get("kill_at_tick"):
                bounds.append(int(peer["kill_at_tick"]))
            if peer.get("kill_after_s"):
                bounds.append(int(float(peer["kill_after_s"]) * 60))
            gates = peer.get("kill_when") or []
            for gate in gates if isinstance(gates, list) else [gates]:
                tick = scheduled_gate_tick(gate, 4 * final)
                if tick is not None:
                    bounds.append(tick)
            if bounds:
                late(f"{peer['name']} scripted drop", min(bounds), final)
            kills[peer["name"]] = min(bounds) if bounds else None
        for peer in peers:
            gate = peer.get("start_when") or {}
            # A peer started after another run's peer ended begins a new round (a resume from disk), not a return into this one.
            if gate.get("ended") and gate.get("peer") in kills and gate.get("run", run.get("name")) == run.get("name"):
                args = peer.get("args", [])
                budget = RELAUNCH_BUDGET_TICKS[bool(int(args[args.index("-net-fake-lag") + 1]) if "-net-fake-lag" in args else 0)]
                final = rounds.get(peer["name"], last)
                if kills[gate["peer"]] is None:
                    found.append(f"{scenario['name']}/{run.get('name', 'run0')}: {peer['name']} relaunches after {gate['peer']}, "
                                 "whose drop no tick lever bounds")
                else:
                    late(f"{peer['name']} relaunch return (drop {kills[gate['peer']]} + {budget})", kills[gate["peer"]] + budget, final)
        feel = run.get("feel_gate") or {}
        if feel.get("silent_tick"):
            late("feel_gate silent_tick", int(feel["silent_tick"]), last)
        if feel.get("rejoin_through_tick"):
            late("feel_gate rejoin_through_tick", int(feel["rejoin_through_tick"]), last)
    return found


def check_levers_inside_rounds(results):
    """No scenario schedules a return, reclaim, hold or relaunch that the round ends before (row 496)."""
    found = [row for path in sorted(driver.SCENARIO_DIR.glob("*.json")) if "checklist" in (scenario := json.loads(path.read_text(encoding="utf-8")))
             for row in lever_violations(scenario)]
    return row(results, "scenarios/levers-land-inside-their-round", not found, "; ".join(found))


def unconfirmed_clicks(steps):
    """Each mouse_up sent before a wait saw its control read pushed after the mouse_down."""
    found, pressed = [], {}
    for index, step in enumerate(steps):
        control = step.get("control")
        if step.get("op") == "mouse_down":
            pressed[control] = False
        elif step.get("op") == "wait" and control in pressed and step.get("equals", {}).get("pushed") is True:
            pressed[control] = True
        elif step.get("op") == "mouse_up" and control in pressed and not pressed.pop(control):
            found.append(f"step {index} {control}")
    return found


def check_probe_clicks_seen(results):
    """A probe releases a click only once the control reads pushed: a catch-up frame can separate the down from the GUI."""
    found = [f"{path.name} {where}" for path in sorted(driver.SCENARIO_DIR.glob("*.probe.json"))
             for where in unconfirmed_clicks(json.loads(path.read_text(encoding="utf-8")).get("steps", []))]
    return row(results, "probes/click-released-after-its-control-reads-pushed", not found, "; ".join(found))


def check_rematch_contract(results):
    scenario = driver.load_scenario("mp-rematch")
    runs = {run["name"]: run for run in scenario.get("runs", [])}
    ok = row(results, "rematch/two-distinct-runs", set(runs) == {"rematch", "injected-desync"})
    if not ok:
        return False
    regular, injected = runs["rematch"], runs["injected-desync"]
    ok &= row(results, "rematch/full-second-round-hash-range", regular.get("hash_gate") == {
        "name": "round2-hashes", "peers": ["host", "client"], "first_tick": 1, "cap": 600})
    peers = {peer["name"]: peer for peer in injected["peers"]}
    host_args, client_args = peers["host"]["args"], peers["client"]["args"]
    ok &= row(results, "rematch/one-sided-existing-perturbation", "-determinism-selftest-perturb" in host_args and
              "-determinism-selftest-perturb" not in client_args and
              host_args[host_args.index("-determinism-selftest-perturb-tick") + 1] == "240")
    ok &= row(results, "rematch/automatic-repair-enabled", all("-net-match-e2e-resync" in peer["args"] and
              "-net-match-service-e2e" in peer["args"] for peer in peers.values()))
    probes = [json.loads(driver.scenario_text(scenario, peer["probe"])) for peer in peers.values()]
    ok &= row(results, "rematch/no-manual-repair-substitution", all("ButtonMatchRepairNow" not in json.dumps(probe) for probe in probes))
    items = [item for item in scenario["checklist"] if item.get("run") == "injected-desync"]
    ok &= row(results, "rematch/repair-needs-video-and-native-evidence", all(
        any(item.get("peer") == name and item.get("screen") == "ResyncOverlay" and
            any("reloading from the host snapshot" in pattern for pattern in item.get("log_regex", [])) for item in items) and
        any(item.get("peer") == name and item.get("readback") and
            any("match relaunched from the snapshot" in pattern for pattern in item.get("log_regex", [])) for item in items)
        for name in peers))
    return ok


def check_scenarios(results):
    """Every scenario parses, names peers, points at scripts that exist and keeps its own port slice."""
    ok = True
    seen_bases = {}
    for name in SCENARIOS:
        scenario = driver.load_scenario(name)
        ok &= row(results, f"{name}/schema", scenario["schema"] == 1 and scenario["name"] == name)
        base = scenario.get("port_base", driver.PORT_LO)
        runs = scenario.get("runs") or [{"name": "run0", "peers": scenario.get("peers", [])}]
        ports = [driver.port_for(index, base) for index in range(len(runs))]
        inside = all(driver.PORT_LO <= port <= driver.PORT_HI for port in ports)
        clash = [other for other, used in seen_bases.items() if set(used) & set(ports)]
        seen_bases[name] = ports
        ok &= row(results, f"{name}/ports", inside and not clash, f"{ports} clash={clash}")
        for index, run in enumerate(runs):
            peers = run.get("peers") or scenario.get("peers") or []
            ok &= row(results, f"{name}/run{index}/peers", bool(peers), str([peer["name"] for peer in peers]))
            names = [peer["name"] for peer in peers]
            ok &= row(results, f"{name}/run{index}/unique-peers", len(set(names)) == len(names))
            for peer in peers:
                for key in ("menu_script", "input_script", "probe"):
                    if not peer.get(key):
                        continue
                    try:
                        text = driver.scenario_text(scenario, peer[key])
                    except SystemExit as error:
                        ok &= row(results, f"{name}/{peer['name']}/{key}", False, str(error))
                        continue
                    if key != "probe":
                        ok &= row(results, f"{name}/{peer['name']}/{key}", bool(text.strip()))
                        continue
                    probe = json.loads(text)
                    fits = (probe.get("schema") == 1 and 0 < probe.get("timeout_ms", 0) <= 180000 and
                            0 < len(probe.get("steps", [])) <= 256)
                    ok &= row(results, f"{name}/{peer['name']}/probe", fits,
                              f"{len(probe.get('steps', []))} steps, {probe.get('timeout_ms')} ms")
        checklist = scenario.get("checklist", [])
        ids = [item["id"] for item in checklist]
        ok &= row(results, f"{name}/checklist", bool(ids) and len(set(ids)) == len(ids), f"{len(ids)} items")
        ok &= row(results, f"{name}/checklist-what", all(item.get("what") for item in checklist))
        # An item that cannot be driven names why, so a reviewer never reads a gap as a pass.
        ok &= row(results, f"{name}/checklist-blocked-named",
                  all(item.get("blocked_by") for item in checklist if item.get("assert") == "not scripted"))
    return ok


# The engine's screen-name functions; the frame recorder writes only what these return (Gameplay reads as game).
SCREEN_NAME_SOURCES = (("Source/Main.cpp", "static std::string RecordedScreenName()"),
                       ("Source/Menus/MainMenuGUI.cpp", "std::string MainMenuGUI::AutomationActiveScreenName() const"),
                       ("Source/Menus/PauseMenuGUI.cpp", "std::string PauseMenuGUI::AutomationActiveScreenName() const"),
                       ("Source/Menus/ScenarioGUI.h", "std::string AutomationScreen() const"))
NOMINAL_TICKS_PER_S = 60
# How far a probe's nominal clock may run off the engine's before a screen counts as outside an item's window.
SCREEN_WINDOW_SLACK_TICKS = 60


def recorder_screens(repo):
    """Every screen name the frame recorder can write, read from the engine's own screen-name functions."""
    names = set()
    for relative, head in SCREEN_NAME_SOURCES:
        text = (repo / relative).read_text(encoding="utf-8", errors="replace")
        start = text.index(head)
        depth = 0
        for end in range(text.index("{", start), len(text)):
            depth += {"{": 1, "}": -1}.get(text[end], 0)
            if depth == 0:
                break
        names |= set(re.findall(r'"([A-Za-z]+)"', text[start:end]))
    main = (repo / "Source/Main.cpp").read_text(encoding="utf-8", errors="replace")
    for call in re.findall(r"RecordVideoFrame\(([^;]*)\);", main):
        names |= set(re.findall(r'"([A-Za-z]+)"', call))
    return names


def item_screens(item):
    screen = item.get("screen")
    return set(screen) if isinstance(screen, list) else {screen} if screen else set()


def probe_screen_timeline(steps):
    """The screens an in-match probe holds, on a nominal sim clock: a tick wait anchors the clock, elapsed and render
    waits advance it at the nominal rate while the match runs, a wait on the service or a file leaves it unknown until
    the next anchor. The screen is the last one the probe waited on; once the probe ends any menu it left may close.
    Each segment is (screens, first tick, last tick, first step, last step); a tick is None where the clock is unknown."""
    segments, marks = [], {}
    screens, opened, opened_step, tick, paused = {"game"}, 0.0, 0, 0.0, False
    for index, step in enumerate(steps):
        op = step.get("op")
        command = step.get("command", "") if op == "menu" else ""
        if command.startswith("video_mark "):
            marks.setdefault(command.split(" ", 1)[1], index)
        if command == "activate ButtonPauseMatch":
            paused = not paused
        if op == "wait_file" or op == "wait" and "service" in step and not {"sim_at_least", "lockstep_frame_at_least"} & set(step):
            tick = None
        elif op == "wait":
            if tick is not None and not paused:
                tick += max(step.get("elapsed_ms", 0) * NOMINAL_TICKS_PER_S / 1000, step.get("renders", 0))
            anchor = max(step.get("sim_at_least", 0), step.get("lockstep_frame_at_least", 0))
            if anchor:
                tick = max(tick or 0.0, anchor)
        if op == "wait" and step.get("screen"):
            segments.append((frozenset(screens), opened, tick, opened_step, index))
            screens, opened, opened_step = {"game" if step["screen"] == "Gameplay" else step["screen"]}, tick, index
    segments.append((frozenset(screens), opened, tick, opened_step, len(steps)))
    segments.append((frozenset(screens | {"game"}), tick, float("inf"), len(steps), len(steps)))
    return segments, marks


def unreachable_item_screen(item, steps):
    """Why no frame of this item can match on a peer this probe drives, or None when one can (or it cannot be told)."""
    wanted = item_screens(item)
    segments, marks = probe_screen_timeline(steps)
    if item.get("mark"):
        start = marks.get(item["mark"])
        if start is None:
            return None
        following = min([index for index in marks.values() if index > start], default=len(steps) + 1)
        held = set().union(*(screens for screens, _, _, first, last in segments if first < following and last >= start))
        return None if wanted & held else f"mark {item['mark']} holds {sorted(held)}"
    low, high = (item.get("sim_ticks") or [None, None])[:2]
    if low is None or high is None:
        return None
    low, high = low - SCREEN_WINDOW_SLACK_TICKS, high + SCREEN_WINDOW_SLACK_TICKS
    inside = [(screens, first, last) for screens, first, last, _, _ in segments
              if (first is None or first <= high) and (last is None or last >= low)]
    if any(first is None or last is None for _, first, last in inside):
        return None
    held = set().union(*(screens for screens, _, _ in inside))
    return None if wanted & held else f"ticks {item['sim_ticks']} hold {sorted(held)}"


def check_item_screens_reachable(results, repo):
    """Every checklist item's screen filter names a screen the recorder writes and, on a peer an in-match probe drives,
    one that probe holds over the item's ticks or marked steps: an item no frame can match reads as missing evidence."""
    known = recorder_screens(repo)
    ok = row(results, "checklist-screens/recorder-names-read", {"game", "Pause", "PauseMatchOptions", "MultiplayerScreen"} <= known,
             str(sorted(known)))
    unknown, unreachable, judged = [], [], 0
    for path in sorted(driver.SCENARIO_DIR.glob("*.json")):
        scenario = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(scenario, dict) or scenario.get("schema") != 1 or scenario.get("driver", "tools/e2e_video.py") != "tools/e2e_video.py":
            continue
        scenario["path"] = str(path)
        runs = scenario.get("runs") or [{"name": "run0", "peers": scenario.get("peers", [])}]
        for item in scenario.get("checklist", []):
            names = item_screens(item)
            if names - known:
                unknown.append(f"{scenario['name']}/{item['id']} {sorted(names - known)}")
            if not names:
                continue
            scope = item.get("run")
            for run in runs:
                if scope and run.get("name") not in ([scope] if isinstance(scope, str) else scope):
                    continue
                for peer in run.get("peers") or scenario.get("peers") or []:
                    # A probe on a menu-script peer starts wherever the script leaves it; only in-match probes start on game.
                    if item.get("peer", peer["name"]) != peer["name"] or not peer.get("probe") or peer.get("menu_script"):
                        continue
                    steps = json.loads(driver.scenario_text(scenario, peer["probe"])).get("steps", [])
                    judged += 1
                    why = unreachable_item_screen(item, steps)
                    if why:
                        unreachable.append(f"{scenario['name']}/{run.get('name')}/{peer['name']}/{item['id']}: {why}")
    ok &= row(results, "checklist-screens/recorder-writes-every-named-screen", not unknown, "; ".join(unknown[:5]))
    ok &= row(results, "checklist-screens/probe-holds-every-named-screen", not unreachable,
              f"{judged} item-peer pairs on in-match probes; " + "; ".join(unreachable[:5]))
    return ok


def check_e2e_host_end_completion(results, repo):
    """In -net-match-service-e2e mode a member completes on its host's End Match, as the product path does, and the
    local stop of a failed handover still fails the run. Read from the controller-stop chain in Main.cpp."""
    text = (repo / "Source/Main.cpp").read_text(encoding="utf-8", errors="replace")
    start = text.index("static void HandleControllerReplayFailure(")
    body = text[start:text.index("\n}\n", start)]
    declared = re.search(r"const bool e2eHostEndedRound = ([^;]*);", body)
    condition = " ".join(declared[1].split()) if declared else ""
    ok = row(results, "main/e2e-member-host-end-is-a-completion",
             all(term in condition for term in ("s_netMatchServiceE2E", "IsCompleteControllerStop(error)", "!g_NetMatchService.IsHost()",
                                                "NetMatchServiceState::Running")), condition[:200])
    # The poll and the controller path stop with "tick N lockstep stopped: Complete:..."; the finish path with the bare reason.
    helper = re.search(r"static bool IsCompleteControllerStop\(const std::string& error\) \{(.*?)\n\}\n", text, re.S)
    helper_body = helper[1] if helper else ""
    ok &= row(results, "main/e2e-host-end-read-behind-the-stop-prefix",
              'error.starts_with("Complete:")' in helper_body and '" lockstep stopped: Complete:"' in helper_body
              and 'error.starts_with("tick ")' in helper_body, helper_body[:200])
    ok &= row(results, "main/e2e-failed-handover-is-not-a-host-end", 'error.find("host handover ended") == std::string::npos' in condition)
    branch = re.search(r'else if \(error\.find\("Complete:"\) != std::string::npos &&\s*error\.find\("e2e complete"\)[^{]*\{(.*?)\n\t\t\} else if', body, re.S)
    ok &= row(results, "main/e2e-completion-branch-takes-the-host-end",
              bool(branch) and "e2eHostEndedRound" in branch[0] and "completed_by_host_end=1" in branch[1] and "FinishMatch" in branch[1])
    scenario = driver.load_scenario("mp-host-draw-stall-menu")
    required = [item for item in scenario["checklist"] if any("completed_by_host_end=1" in pattern for pattern in item.get("log_regex", []))]
    ok &= row(results, "mp-host-draw-stall-menu/members-complete-on-host-end", sorted(item["peer"] for item in required) == ["client", "survivor"]
              and all("controller sync failed" in item.get("forbidden_log_regex", []) for item in required))
    return ok


def check_substitution(results):
    tokens = {"PORT": 49411, "PROBE_DIR": Path("D:/x/host-stage/probe"), "PEER": "host"}
    text = driver.substitute("settext TextHostPort {PORT}\nwait_file {PROBE_DIR}/done.json 240\n", tokens)
    ok = row(results, "substitute/text", "49411" in text and "{PORT}" not in text and "{PROBE_DIR}" not in text)
    args = driver.substitute(["-net-port", "{PORT}", "-peer", "{PEER}"], tokens)
    ok &= row(results, "substitute/list", args == ["-net-port", "49411", "-peer", "host"], str(args))
    nested = driver.substitute({"a": ["{PEER}"], "b": {"c": "{PORT}"}}, tokens)
    ok &= row(results, "substitute/nested", nested == {"a": ["host"], "b": {"c": "49411"}}, str(nested))
    ok &= row(results, "substitute/unknown-token-left", driver.substitute("{NOPE}", tokens) == "{NOPE}")
    ok &= row(results, 'substitute/quoted-native-path-has-no-escapes', driver.substitute('dump_match_identity "{PROBE_DIR}/id.json"', tokens) == 'dump_match_identity "D:/x/host-stage/probe/id.json"')
    return ok


def check_directory_port_block(results):
    """A scenario's session directory keeps its place below the top of the driver's block, so a lane's own --port-block
    carries it with the runs instead of refusing the run."""
    from e2e import directory
    relay, ui = driver.load_scenario("mp-direct-vs-relay"), driver.load_scenario("ui-surfaces")
    default = (driver.directory_port_for(relay, relay["runs"][1], relay["port_base"]), driver.directory_port_for(ui, ui["runs"][0], ui["port_base"]))
    ok = row(results, "directory/default-block-keeps-the-scenario-port", default == (relay["directory_port"], ui["directory_port"]), str(default))
    saved = driver.PORT_LO, driver.PORT_HI
    try:
        driver.PORT_LO, driver.PORT_HI = 49860, 49879
        lane = (driver.directory_port_for(relay, relay["runs"][1], 49860), driver.directory_port_for(ui, ui["runs"][0], 49860))
    finally:
        driver.PORT_LO, driver.PORT_HI = saved
    ok &= row(results, "directory/lane-block-carries-the-directory", lane == (49878, 49879), str(lane))
    refused = None
    try:
        with directory.serve(Path("unused"), 49478, (49860, 49879)):
            pass
    except ValueError as error:
        refused = str(error)
    ok &= row(results, "directory/serve-refuses-a-port-outside-the-block", bool(refused) and "49860-49879" in refused, str(refused))
    ok &= row(results, "directory/no-directory-without-a-port", driver.directory_port_for({"name": "x", "runs": [{}]}, {}, 49400) is None)
    return ok


def check_launch_contract(results, scratch):
    stage = scratch / "launch"
    stage.mkdir()
    scenario = {"scripts": {"probe.json": json.dumps({"schema": 1, "steps": [
        {"op": "wait_file", "path": "{PROBE_DIR_client}/done.json"}]}), "menu.txt": "exit\n"}}
    peer = {"probe": "probe.json", "menu_script": "menu.txt", "args": ["-net-port", "{PORT}"]}
    tokens = {"PROBE_DIR_client": r"D:\mx\client-stage\probe", "VIDEO": stage / "video",
              "MENU_SCRIPT": stage / "menu.txt", "PORT": 49400}
    env = driver.stage_peer(scenario, peer, stage, tokens)
    probe = json.loads(Path(env["CC_TEST_NET_UI_SCRIPT"]).read_text(encoding="utf-8"))
    ok = row(results, "launch/windows-probe-path",
             probe["steps"][0]["path"] == r"D:\mx\client-stage\probe/done.json")
    args = driver.peer_arguments(peer, tokens, 30)
    ok &= row(results, "launch/menu-script-passed",
              args[args.index("-menu-script") + 1] == str(stage / "menu.txt"), str(args))
    ok &= row(results, "launch/headless", env["CCCP_HEADLESS"] == "1")
    return ok


def check_index_and_checklist(results, scratch):
    """A synthetic capture: the checklist resolves to the frames that carry each screen and tick."""
    video = scratch / "video"
    (video / "frames").mkdir(parents=True)
    rows = []
    for frame in range(12):
        screen = "MainScreen" if frame < 4 else ("game" if frame < 9 else "Pause")
        rows.append({"frame": frame, "wall_ms": frame * 33, "sim_tick": frame * 50,
                     "screen": screen, "resolution": [960, 540], "saved": True})
    (video / "frames.jsonl").write_text("".join(json.dumps(line) + "\n" for line in rows), encoding="utf-8")
    (video / "manifest.json").write_text(json.dumps(
        {"schema": 1, "fps": 30, "frames_saved": 12, "frames_dropped": 0, "frames_rate_limited": 3,
         "resolution": [960, 540], "first_sim_tick": 0, "last_sim_tick": 550}) + "\n", encoding="utf-8")

    read = driver.read_index(video)
    ok = row(results, "index/read", len(read) == 12 and read[0]["frame"] == 0, f"{len(read)} rows")
    ok &= row(results, "manifest/read", driver.read_manifest(video)["fps"] == 30)
    ok &= row(results, "index/missing-is-empty", driver.read_index(scratch / "nothing") == [])
    ok &= row(results, "manifest/missing-is-empty", driver.read_manifest(scratch / "nothing") == {})

    ok &= row(results, "checklist/by-screen", driver.frame_range(read, {"screen": "game"}) == [4, 8])
    ok &= row(results, "checklist/by-screen-and-ticks",
              driver.frame_range(read, {"screen": "game", "sim_ticks": [250, 350]}) == [5, 7])
    ok &= row(results, "checklist/never-seen", driver.frame_range(read, {"screen": "PauseMatchOptions"}) is None)
    ok &= row(results, "checklist/any-of-screens-and-ticks",
              driver.frame_range(read, {"screen": ["PauseMatchOptions", "Pause"], "sim_ticks": [300, 550]}) == [9, 11])
    ok &= row(results, "checklist/ticks-only", driver.frame_range(read, {"sim_ticks": [0, 100]}) == [0, 2])

    # A truncated index is what a killed peer leaves: it still reads, and the checklist still resolves.
    torn = scratch / "torn"
    (torn / "frames").mkdir(parents=True)
    (torn / "frames.jsonl").write_text(json.dumps(rows[0]) + "\n" + json.dumps(rows[1])[:20], encoding="utf-8")
    ok &= row(results, "index/torn-line-skipped", len(driver.read_index(torn)) == 1)
    return ok


def check_encode(results, scratch):
    """The encode and sheet refuse an empty capture by name instead of shelling out to nothing."""
    empty = scratch / "empty"
    (empty / "frames").mkdir(parents=True)
    result = driver.encode("ffmpeg", empty, 30, scratch / "out.mp4")
    ok = row(results, "encode/no-frames", result["encoded"] is False and "no frames" in result["reason"])
    sheet = driver.contact_sheet(empty, [], scratch / "sheet.png", 15, "ffmpeg")
    ok &= row(results, "sheet/no-frames", sheet["written"] is False)
    missing = driver.encode(None, scratch / "video", 30, scratch / "out.mp4")
    ok &= row(results, "encode/no-ffmpeg-empty-capture", missing["encoded"] is False and "no frames" in missing["reason"])
    (scratch / "video/frames/frame-000000.png").write_bytes(b"frame")
    missing = driver.encode(None, scratch / "video", 30, scratch / "out.mp4")
    ok &= row(results, "encode/no-ffmpeg", missing["encoded"] is False and "ffmpeg" in missing["reason"])
    located = driver.find_ffmpeg()
    row(results, "encode/ffmpeg-located", True, str(located))
    if located:
        from PIL import Image
        video = scratch / "timed-video"
        (video / "frames").mkdir(parents=True)
        rows = [{"frame": frame, "wall_ms": wall, "saved": True} for frame, wall in enumerate((1000, 1100, 2100))]
        for row_value in rows:
            Image.new("RGB", (16, 16), (row_value["frame"] * 100, 20, 30)).save(
                video / "frames" / f"frame-{row_value['frame']:06d}.png")
        (video / "frames.jsonl").write_text("".join(json.dumps(value) + "\n" for value in rows), encoding="utf-8")
        result = driver.encode(located, video, 10, scratch / "timed.mp4")
        duration = float(result.get("ffprobe", {}).get("format", {}).get("duration", 0))
        ok &= row(results, "encode/keeps-one-second-stall", result["encoded"] and 1.1 <= duration <= 1.4, str(result))
    return ok


def check_review(results, scratch):
    scenario = {"name": "paired-review", "checklist": [
        {"id": "mp-play", "screen": "game", "what": "Both peers have gameplay evidence."}]}
    capture = {"name": "run0", "peers": [
        {"peer": "host", "root": str(scratch / "host"), "video_dir": str(scratch / "video"),
         "probe_dir": str(scratch / "host-stage/probe"), "index": driver.read_index(scratch / "video"),
         "menu_script_failures": [], "video": None, "contact_sheet": None},
        {"peer": "client", "root": str(scratch / "client"), "video_dir": str(scratch / "video"),
         "probe_dir": str(scratch / "client-stage/probe"), "index": driver.read_index(scratch / "video"),
         "menu_script_failures": ["[menu-script] FAILED: assert_substate"], "video": None,
         "contact_sheet": None}]}
    out = scratch / "review"
    out.mkdir()
    document = driver.review(scenario, capture, out)
    ok = row(results, "review/written", (out / "review.json").is_file())
    ok &= row(results, "review/every-item-resolved", all("state" in item for item in document["checklist"]))
    unpeered = [item for item in document["checklist"] if item["id"] == "mp-play"]
    ok &= row(results, "review/peerless-item-covers-both", len(unpeered) == 2, str(len(unpeered)))
    ok &= row(results, "review/failures-carried",
              document["failures"]["client"] == ["[menu-script] FAILED: assert_substate"])
    scenario_items = [item for item in document["checklist"] if item["id"] != "no-assert-dialogs"]
    ok &= row(results, "review/no-probe-is-named",
              all(item.get("probe") == "awaiting-review" for item in scenario_items))
    # The dialog row is written for every capture: a player would have had to answer each line it lists.
    dialog_rows = [item for item in document["checklist"] if item["id"] == "no-assert-dialogs"]
    ok &= row(results, "review/assert-dialog-row-present",
              len(dialog_rows) == 1 and dialog_rows[0]["probe"] == "pass" and dialog_rows[0]["assert_dialogs"] == [],
              str(dialog_rows))
    (scratch / "host").mkdir(parents=True, exist_ok=True)
    (scratch / "host/stdout.log").write_text(
        "[menu-script] loaded 2 steps\nRTE Assert (headless, continued like Ignore): Assertion in file 'X.cpp'\n",
        encoding="utf-8")
    fired = driver.review(scenario, capture, out)
    flagged = [item for item in fired["checklist"] if item["id"] == "no-assert-dialogs"][0]
    ok &= row(results, "review/assert-dialog-row-reports-the-line",
              flagged["probe"] == "fail" and "continued like Ignore" in flagged["finding"]["reason"]
              and flagged["assert_dialogs"][0]["peer"] == "host", str(flagged.get("assert_dialogs")))
    (scratch / "host/stdout.log").unlink()
    document = driver.review(scenario, capture, out)
    ok &= row(results, "review/verdict-is-not-a-pass", document["verdict"] == "agent-review-required")
    ok &= row(results, "review/no-mp4-is-not-video-evidence", all(item["frames"] is None for item in document["checklist"]))
    capture["peers"][0]["record"] = {"exit_code": 1, "timed_out": False}
    document = driver.review({"name": "exit-review", "checklist": []}, capture, out)
    ok &= row(results, "review/failed-exit-is-a-run-finding", len(document["run_findings"]) == 1)
    ok &= row(results, "review/summary-rejects-unseen-run-failure", driver.review_only(SimpleNamespace(review_only=out)) == 1)
    capture["peers"][0]["record"].update(exit_code=137, injected_termination="scenario drop after recorded tick 601")
    capture["peers"][0]["expected_termination"] = True
    document = driver.review({"name": "exit-review", "checklist": []}, capture, out)
    ok &= row(results, "review/planned-drop-keeps-native-exit", not document["run_findings"] and capture["peers"][0]["record"]["exit_code"] == 137)
    marker_video = scratch / "markers"
    marker_video.mkdir()
    (marker_video / "events.jsonl").write_text(''.join(json.dumps(value) + '\n' for value in [
        {"wall_ms": 10, "message": "video_mark first"}, {"wall_ms": 12, "message": "assert_label Value shown PASS"},
        {"wall_ms": 25, "message": "video_mark next"}]), encoding="utf-8")
    record = {"video_dir": str(marker_video), "probe_dir": str(scratch / "absent-probe"), "index": [
        {"frame": index, "wall_ms": at, "sim_tick": 0, "screen": "SettingsScreen", "saved": True}
        for index, at in enumerate((0, 10, 20, 30))]}
    frames, observed = driver.item_evidence(record, {"mark": "first", "screen": "SettingsScreen", "events": ["assert_label Value shown PASS"]})
    ok &= row(results, "review/marker-bounds", frames == [1, 2] and observed["probe"] == "pass", str(frames))
    tied = scratch / "markers-tied"
    tied.mkdir()
    (tied / "events.jsonl").write_text(''.join(json.dumps(value) + '\n' for value in [
        {"wall_ms": 10, "message": "video_mark first"}, {"wall_ms": 25, "message": "assert_label Value shown PASS"},
        {"wall_ms": 25, "message": "video_mark next"}, {"wall_ms": 25, "message": "assert_label Next shown PASS"}]), encoding="utf-8")
    tied_record = {**record, "video_dir": str(tied)}
    _, observed = driver.item_evidence(tied_record, {"mark": "first", "screen": "SettingsScreen", "events": ["assert_label Value shown PASS"]})
    ok &= row(results, "review/assert-in-the-next-marks-millisecond-kept", observed["probe"] == "pass")
    _, observed = driver.item_evidence(tied_record, {"mark": "next", "screen": "SettingsScreen", "events": ["assert_label Value shown PASS"]})
    ok &= row(results, "review/earlier-marks-assert-not-borrowed", observed["probe"] == "fail")
    _, observed = driver.item_evidence(tied_record, {"mark": "next", "screen": "SettingsScreen", "events": ["assert_label Next shown PASS"]})
    ok &= row(results, "review/next-marks-own-assert-kept", observed["probe"] == "pass")
    frames, observed = driver.item_evidence(record, {"mark": "missing", "screen": "SettingsScreen"})
    ok &= row(results, "review/missing-marker-is-not-a-screen-pass", frames is None and observed["probe"] == "not-reached")
    return ok


def check_interruption(results, scratch):
    out = scratch / "interrupted"
    first = out / "first"
    first.mkdir(parents=True)
    scenario = {"name": "interrupted", "runs": [{"name": "first", "peers": [{"name": "host"}]},
                                                {"name": "second", "peers": [{"name": "client"}]}],
                "checklist": [{"id": "play", "screen": "game", "what": "Gameplay is visible."}]}
    run = {"name": "first", "root": str(first), "size": "640x360", "interrupted": "test interruption",
           "peers": [{"peer": "host", "root": str(first / "host"), "video_dir": str(scratch / "video"),
                      "probe_dir": str(first / "probe"), "record": {}, "manifest": {},
                      "index": driver.read_index(scratch / "video"), "menu_script_failures": []}]}
    capture = {"scenario": "interrupted", "scenario_definition": scenario, "runs": [run], "source": {}, "exe": {},
               "started": "test", "fps": 6, "command": [], "interrupted": "test interruption"}
    driver.review(scenario, run, first)
    manifest = driver.scenario_manifest(capture, out, 1)
    review = driver.aggregate_review(capture, out)
    ok = row(results, "interruption/manifest-keeps-saved-frames", manifest["frame_count"] == 12 and manifest["interrupted"] == "test interruption")
    started_items = [item for item in review["checklist"] if item["id"] != "no-assert-dialogs"]
    ok &= row(results, "interruption/unstarted-checklist-retained", len(started_items) == 2 and started_items[1]["run"] == "second")
    ok &= row(results, "interruption/missing-video-explained", all(item["frames"] is None and item["finding"]["reason"] == "test interruption" for item in review["checklist"]))
    return ok


def check_item_assertions(results, scratch):
    root = scratch / "item-assertions"
    probe = root / "probe"
    probe.mkdir(parents=True)
    observed = {"complete": True, "pass": True, "script": {"steps": [{"op": "wait"}, {"op": "assert"}]},
                "steps": [{"index": 0, "observed": {"sim_frame": 100}}, {"index": 1, "observed": {"sim_frame": 700}}]}
    path = probe / "net-ui-result.json"
    path.write_text(json.dumps(observed), encoding="utf-8")
    record = {"root": str(root), "video_dir": str(root / "video"), "index": [], "probe_dir": str(probe)}
    _, evidence = driver.item_evidence(record, {"probe_steps": [0, 1], "sim_progress": 600})
    ok = row(results, "review/simulation-progress", evidence["probe"] == "pass")
    observed["steps"][1]["observed"]["sim_frame"] = 200
    path.write_text(json.dumps(observed), encoding="utf-8")
    _, evidence = driver.item_evidence(record, {"probe_steps": [0, 1], "sim_progress": 600})
    ok &= row(results, "review/missing-simulation-progress", evidence["probe"] == "fail")
    (root / "runtime").mkdir()
    (root / "runtime/LogConsole.txt").write_text("parked brains\nERROR: Lua failure\n", encoding="utf-8")
    checks = driver.log_assertions(root, ["parked brains"], ["^ERROR:"])
    ok &= row(results, "review/console-errors-retained", checks[0]["matches"][0]["line"] == 1 and checks[1]["matches"][0]["line"] == 2 and checks[1]["forbidden"])
    _, evidence = driver.item_evidence(record, {"log_regex": ["parked brains"], "forbidden_log_regex": ["^ERROR:"]})
    ok &= row(results, "review/positive-log-cannot-hide-lua-error", evidence["probe"] == "fail")
    observed['steps'][0]['observed']['control'] = {'text': 'Host connection lost / RTT -- ms'}
    path.write_text(json.dumps(observed), encoding='utf-8')
    item = {'readback': [{'step': 0, 'path': ['control', 'text'], 'not_contains': 'LIVE'}]}
    _, evidence = driver.item_evidence(record, item)
    ok &= row(results, 'review/forbidden-control-text', evidence['readback_assertions'][0]['pass'])
    item['readback'][0]['step'] = 2
    _, evidence = driver.item_evidence(record, item)
    ok &= row(results, 'review/missing-control-is-not-negative-proof', evidence['probe'] == 'fail')
    toasts = {"report_toasts": {"report": "{peer}-match.json",
                                "require": [{"kind": "seat_held", "text": "^Held - AI in control - rejoining$", "ticks": [600, 800]}]}}
    record.update(peer="leaver", root=str(root / "leaver"))
    _, evidence = driver.item_evidence(record, toasts)
    ok &= row(results, "review/missing-report-toast-fails", evidence["probe"] == "fail", str(evidence.get("reason")))
    (root / "leaver-match.json").write_text(json.dumps({"ui": {"toasts": [
        {"tick": 1, "kind": "player_joined", "text": "host joined"},
        {"tick": 605, "kind": "seat_held", "text": "held - AI in control"}]}}), encoding="utf-8")
    _, evidence = driver.item_evidence(record, toasts)
    ok &= row(results, "review/other-toast-text-is-not-the-required-one", evidence["probe"] == "fail")
    (root / "leaver-match.json").write_text(json.dumps({"ui": {"toasts": [
        {"tick": 600, "kind": "seat_held", "text": "Held - AI in control - rejoining"}]}}), encoding="utf-8")
    _, evidence = driver.item_evidence(record, toasts)
    ok &= row(results, "review/report-toast-passes", evidence["probe"] == "pass")
    _, evidence = driver.item_evidence(record, {**toasts, "log_regex": [r"private catch-up complete frame=\d+ in_place=1"]})
    ok &= row(results, "review/report-toast-cannot-hide-a-missing-log-line", evidence["probe"] == "fail")
    return ok


def check_committed_window(results, scratch, scenario_texts=None):
    """A play window counts the round's committed frames and closes on the round's own frame count: the discovery
    run's loaded windows (581 and 587 committed in a 10.5 s wall window) are a load artefact, not missing play."""
    loaded = [{"sim_frame": 9000, "lockstep_frame": 17, "at_ms": 1000}, {"sim_frame": 9581, "lockstep_frame": 598, "at_ms": 11900}]
    progress = driver.window_progress(loaded, 600)
    ok = row(results, "window/counts-committed-round-frames", progress["counted"] == "lockstep_frame" and progress["observed"] == 581
             and progress["window_ms"] == 10900 and not progress["pass"])
    solo = [{"sim_frame": 100, "lockstep_frame": 0, "at_ms": 0}, {"sim_frame": 700, "lockstep_frame": 0, "at_ms": 10000}]
    ok &= row(results, "window/single-player-counts-sim-updates", driver.window_progress(solo, 600)["counted"] == "sim_frame"
              and driver.window_progress(solo, 600)["pass"])
    scenario = driver.load_scenario("mod-void-wanderers-multiplayer")
    for item in [item for item in scenario["checklist"] if item.get("sim_progress")]:
        peer = next(peer for peer in scenario["peers"] if peer["name"] == item["peer"])
        text = (scenario_texts or {}).get(peer["probe"]) or driver.scenario_text(scenario, peer["probe"])
        steps = json.loads(text)["steps"]
        mark = next(index for index, step in enumerate(steps) if step.get("command") == "video_mark " + item["mark"])
        closing = steps[mark + 1]
        # The mark lands within the round's first 100 frames, so a window closing at frame >= 100 + the requirement holds it.
        ok &= row(results, f"window/{item['id']}-closes-on-committed-frames",
                  closing.get("op") == "wait" and closing.get("lockstep_frame_at_least", 0) >= item["sim_progress"] + 100, json.dumps(closing))
    # The other play windows close on the round's frame too: past the frame their window opens on by the requirement.
    for name in ("mp-host-join", "mp-host-join-cross", "mp-resume-from-disk"):
        scenario = driver.load_scenario(name)
        for item in [item for item in scenario["checklist"] if item.get("sim_progress")]:
            peers = scenario.get("peers") or next(run["peers"] for run in scenario["runs"] if run["name"] == item.get("run"))
            probe = next(peer["probe"] for peer in peers if peer["name"] == item["peer"])
            steps = json.loads((scenario_texts or {}).get(probe) or driver.scenario_text(scenario, probe))["steps"]
            if item.get("probe_steps"):
                opening = steps[item["probe_steps"][0]]
                closing = next((steps[index] for index in reversed(item["probe_steps"][1:]) if steps[index].get("op") == "wait"), {})
            else:
                mark = next(index for index, step in enumerate(steps) if step.get("command") == "video_mark " + item["mark"])
                opening, closing = steps[mark], steps[mark + 1]
            opens_at = opening.get("lockstep_frame_at_least", 0)
            counted = opening.get("op") == "menu" or "lockstep_frame_at_least" in opening
            ok &= row(results, f"window/{name}/{item['id']}-closes-on-committed-frames",
                      counted and closing.get("op") == "wait" and closing.get("lockstep_frame_at_least", 0) >= opens_at + item["sim_progress"]
                      and "sim_at_least" not in closing, json.dumps([opening, closing]))
    return ok


def check_resumed_play(results, scratch):
    """Play after the automatic repair is judged on the engine's own records, never on probe steps a script may skip."""
    root = scratch / "resumed-play" / "injected-desync"
    peer = root / "client"
    probe = root / "client-stage" / "probe"
    probe.mkdir(parents=True)
    peer.mkdir(parents=True)
    lines = ["[net-lockstep] start round=11 frame=1 local_peer=2 peers=2 input_delay=3",
             "[net-match] resync: reloading from the host snapshot",
             "[net-lockstep] start round=22 frame=241 local_peer=2 peers=2 input_delay=3",
             "[net-match] resync: match relaunched from the snapshot"]
    (peer / "stdout.log").write_text("\n".join(lines) + "\n", encoding="utf-8")

    def trace(last, moving=True):
        rows = [{"tick": tick, "subsystems": {"actors": f"a{tick}"}} for tick in range(1, 244)]
        rows += [{"tick": tick, "subsystems": {"actors": f"b{tick}" if moving else "still"}} for tick in range(241, last + 1)]
        (root / "client_trace.json").write_text(json.dumps({"runs": [{"tick_hashes": rows}]}), encoding="utf-8")

    (root / "client-match.json").write_text(json.dumps({"actors": 2, "resyncs": 1}), encoding="utf-8")
    # The discovery run's red: the probe stopped after the completion toast, so the steps from the play mark on are absent.
    script = [{"op": "menu", "command": "video_mark injected-repair-complete"}, {"op": "menu", "command": "video_mark injected-play-resumed"},
              {"op": "wait", "service": "Running", "sim_at_least": 900}, {"op": "finish"}]
    (probe / "net-ui-result.json").write_text(json.dumps({"complete": False, "pass": False, "script": {"steps": script},
                                                           "steps": [{"index": 0, "observed": {"sim_frame": 300}}]}), encoding="utf-8")
    video = peer / "video"
    video.mkdir()
    (video / "events.jsonl").write_text("\n".join(json.dumps({"wall_ms": ms, "message": message}) for ms, message in (
        (100, "video_mark injected-repair-complete"), (200, "video_mark injected-play-resumed"))) + "\n", encoding="utf-8")
    record = {"root": str(peer), "peer": "client", "video_dir": str(video), "index": [], "probe_dir": str(probe)}
    spec = {"relaunch": r"\[net-match\] resync: match relaunched from the snapshot", "through_tick": 900,
            "trace": "{peer}_trace.json", "report": "{peer}-match.json"}
    trace(1200)
    _, old = driver.item_evidence(record, {"mark": "injected-play-resumed", "sim_progress": 120})
    _, new = driver.item_evidence(record, {"resumed_play": spec})
    ok = row(results, "resumed-play/probe-steps-a-script-skipped-were-the-red", old["probe"] in ("not-reached", "fail")
             and not any(row["observed"] for row in old["assertions"]), old["probe"])
    ok &= row(results, "resumed-play/engine-records-decide", new["probe"] == "pass" and new["resumed_play"]["resume_frame"] == 241
              and new["resumed_play"]["first_committed_after_repair"] == 241 and new["resumed_play"]["missing_count"] == 0,
              json.dumps(new["resumed_play"])[:300])
    trace(700)
    _, short = driver.item_evidence(record, {"resumed_play": spec})
    ok &= row(results, "resumed-play/a-trace-short-of-the-tick-is-red", short["probe"] == "fail" and short["resumed_play"]["missing_count"] == 200)
    trace(1200, moving=False)
    _, still = driver.item_evidence(record, {"resumed_play": spec})
    ok &= row(results, "resumed-play/frozen-actors-are-red", still["probe"] == "fail")
    trace(1200)
    (peer / "stdout.log").write_text("\n".join(lines[:3]) + "\n", encoding="utf-8")
    _, unhealed = driver.item_evidence(record, {"resumed_play": spec})
    ok &= row(results, "resumed-play/no-relaunch-line-is-red", unhealed["probe"] == "fail")
    (root / "client-match.json").write_text(json.dumps({"actors": 0, "resyncs": 1}), encoding="utf-8")
    (peer / "stdout.log").write_text("\n".join(lines) + "\n", encoding="utf-8")
    _, empty = driver.item_evidence(record, {"resumed_play": spec})
    ok &= row(results, "resumed-play/no-live-actor-is-red", empty["probe"] == "fail")
    scenario = driver.load_scenario("mp-rematch")
    resumed = [item for item in scenario["checklist"] if item["id"].startswith("injected-play-resumed-")]
    ok &= row(results, "resumed-play/rematch-items-read-engine-records", len(resumed) == 2 and all(
        item.get("resumed_play", {}).get("through_tick") == 900 and not item.get("mark") and not item.get("probe_steps") for item in resumed))
    return ok


def check_listed_rows(results, scratch):
    root = scratch / "listed-rows"
    root.mkdir(parents=True)
    record = {"root": str(root), "video_dir": str(root / "video"), "index": [], "probe_dir": str(root / "probe")}
    own = "[LAN] Host - P4 A... Duel (1/2) 127.0.0.1:49402"
    other = "[LAN] Host - P4 A... Duel (1/2) 127.0.0.1:49466"

    def listing(rows, shown):
        text = ("[menu-script] assert_text_fits ListLanGames " + "".join(f" row={json.dumps(r)} width=221 available=244" for r in rows)
                + " PASS\n" + f'[menu-script] assert_label TextJoinPort "" text="{shown}" PASS\n')
        (root / "stdout.log").write_text(text, encoding="utf-8")

    rows_item = {"own_session_rows": {"control": "ListLanGames", "expected": 1, "address": "127.0.0.1"}}
    port_item = {"join_port_follows_list": {"control": "ListLanGames", "field": "TextJoinPort"}}
    listing([other, own], "49466")
    _, evidence = driver.item_evidence(record, rows_item, 49402)
    ok = row(results, "listed-rows/other-session-not-counted", evidence["probe"] == "pass" and evidence["own_session_rows"]["other_sessions"] == [other])
    _, evidence = driver.item_evidence(record, port_item, 49402)
    ok &= row(results, "listed-rows/port-follows-first-listed-row", evidence["probe"] == "pass")
    listing([other, own], "49402")
    _, evidence = driver.item_evidence(record, port_item, 49402)
    ok &= row(results, "listed-rows/port-not-following-list-fails", evidence["probe"] == "fail")
    listing([own, own.replace("127.0.0.1", "192.168.50.130")], "49402")
    _, evidence = driver.item_evidence(record, rows_item, 49402)
    ok &= row(results, "listed-rows/own-session-twice-fails", evidence["probe"] == "fail" and len(evidence["own_session_rows"]["own_rows"]) == 2)
    listing([own.replace("127.0.0.1", "192.168.50.130")], "49402")
    _, evidence = driver.item_evidence(record, rows_item, 49402)
    ok &= row(results, "listed-rows/own-session-off-loopback-fails", evidence["probe"] == "fail")
    (root / "stdout.log").write_text("[menu-script] assert_label TextJoinPort \"\" text=\"49402\" PASS\n", encoding="utf-8")
    _, evidence = driver.item_evidence(record, rows_item, 49402)
    ok &= row(results, "listed-rows/missing-readback-fails", evidence["probe"] == "fail" and evidence["own_session_rows"]["rows"] is None)
    directory = root / "directory"
    directory.mkdir()
    ok &= row(results, "directory-session/unlisted-is-none", driver.directory_session(directory, 49475) is None)
    (directory / "listed.json").write_text(json.dumps({"sessions": [{"listen_port": 49443, "session_id": "other"},
                                                                     {"listen_port": 49475, "session_id": "ours"}]}), encoding="utf-8")
    ok &= row(results, "directory-session/own-port-row", driver.directory_session(directory, 49475) == "ours")
    menu = root / "menu.txt"
    menu.write_text("settext TextJoinAddress session:{DIRECTORY_SESSION}\n", encoding="utf-8")
    bound = driver.bind_directory_session(menu, "ours")
    ok &= row(results, "directory-session/bound-into-script", bound and menu.read_text(encoding="utf-8") == "settext TextJoinAddress session:ours\n")
    return ok


def check_frame_gaps(results, scratch):
    root = scratch / "frame-gaps"
    root.mkdir(parents=True)
    index = [{"frame": 0, "wall_ms": 0, "screen": "MultiplayerScreen"},
             {"frame": 1, "wall_ms": 160, "screen": "MultiplayerScreen"},
             {"frame": 2, "wall_ms": 320, "screen": "MultiplayerScreen"}]
    record = {"root": str(root), "video_dir": str(root / "video"), "index": index, "probe_dir": str(root / "probe")}
    item = {"frame_gap": {"max_ms": 1000}}
    _, evidence = driver.item_evidence(record, item)
    ok = row(results, "review/frame-gap-clean-menu", evidence["probe"] == "pass" and evidence["frame_gap"]["worst"]["gap_ms"] == 160)
    index[2]["wall_ms"] = 2500
    _, evidence = driver.item_evidence(record, item)
    ok &= row(results, "review/frame-gap-render-stall", evidence["probe"] == "fail" and evidence["frame_gap"]["over"][0]["gap_ms"] == 2340,
              json.dumps(evidence["frame_gap"]["over"]))
    index[2]["screen"] = "Loading"
    _, evidence = driver.item_evidence(record, item)
    ok &= row(results, "review/frame-gap-ignores-loading", evidence["probe"] == "pass")
    _, evidence = driver.item_evidence({**record, "index": []}, item)
    ok &= row(results, "review/frame-gap-needs-frames", evidence["probe"] == "fail")
    return ok


def check_stop_request(results, scratch):
    out = scratch / "stop-request"
    out.mkdir()
    (out / "stop-request.json").write_text('{"class":"engine","reason":"unit stop"}', encoding="utf-8")
    handles = []

    class Handle:
        def __init__(self, root):
            self.out = self.cwd = Path(root)
            self.out.mkdir()
            (self.cwd / "Userdata").mkdir()
            self.argv = ["engine"]
            self.stopped = threading.Event()
            self.closed = False
            handles.append(self)

        def start(self):
            return self

        def finish(self):
            if not self.stopped.wait(5):
                return {"exit_code": 124, "timed_out": True}
            return {"exit_code": 137, "injected_termination": self.reason}

        def terminate(self, reason):
            self.reason = reason
            self.stopped.set()

        def close(self):
            self.closed = True

    original_make, original_seed = driver.make_run, driver.seed_settings
    try:
        driver.make_run = lambda repo, args, root, timeout, env: Handle(root)
        driver.seed_settings = lambda handle, seed: None
        options = SimpleNamespace(repo=scratch, size="640x360", fps=3, port=49478, scratch_root=scratch)
        scenario = {"name": "stop", "path": "synthetic", "peers": [{"name": "host", "args": []}]}
        captured = driver.run_one(options, scenario, {"name": "first"}, 0, out)
    finally:
        driver.make_run, driver.seed_settings = original_make, original_seed
    ok = row(results, "interruption/stops-and-closes-runner", len(handles) == 1 and handles[0].closed and
               "unit stop" in captured["interrupted"] and captured["peers"][0]["record"]["exit_code"] == 137)
    scenario['checklist'] = [{'id': 'play', 'what': 'The match continues.', 'screen': 'game'}]
    report = driver.review(scenario, captured, out)
    ok &= row(results, 'interruption/engine-stop-keeps-its-class', report['run_findings'][0]['class'] == 'engine' and report['checklist'][0]['finding']['class'] == 'engine')
    return ok


def check_finalizer(results, scratch):
    out = scratch / "finalize"
    peer = out / "first/host"
    (peer / "video").mkdir(parents=True)
    (peer / "launch.json").write_text(json.dumps({"started": True, "exe_sha256": "retained-exe"}), encoding="utf-8")
    (peer / "video/frames.jsonl").write_text(json.dumps({"frame": 0, "wall_ms": 100, "sim_tick": 1, "screen": "game"}) + "\n", encoding="utf-8")
    scenario = {"name": "finalize", "peers": [{"name": "host"}], "runs": [{"name": "first"}, {"name": "second"}],
                "checklist": [{"id": "game", "what": "Game is drawn.", "screen": "game"}]}
    capture = {"scenario": "finalize", "scenario_definition": scenario, "runs": [], "source": {"tip": "retained-tip"},
               "exe": {"sha256": "retained-exe"}, "fps": 3, "started": driver.stamp(), "command": [],
               "scratch_root": str(scratch), "scratch_limit_bytes": 8_000_000_000}
    (out / "capture.json").write_text(json.dumps(capture), encoding="utf-8")
    code = driver.finalize_only(SimpleNamespace(finalize_only=out, metadata_only=True, scratch_root=scratch, sheet_every=3))
    manifest = json.loads((out / "manifest.json").read_text())
    review = json.loads((out / "review.json").read_text())
    saved = json.loads((out / "capture.json").read_text())
    ok = row(results, "finalize/keeps-provenance-and-frames", code == 1 and manifest["frame_count"] == 1 and manifest["source"]["tip"] == "retained-tip")
    ok &= row(results, "finalize/does-not-invent-process-exit", saved["runs"][0]["peers"][0]["record"]["exit_code"] is None)
    finalized_items = [item for item in review["checklist"] if item["id"] != "no-assert-dialogs"]
    ok &= row(results, "finalize/names-unstarted-run", len(finalized_items) == 2 and finalized_items[1]["run"] == "second")
    ok &= row(results, "finalize/manifest-retains-budget", manifest.get("scratch_limit_bytes") == 8_000_000_000 and
              manifest.get("scratch_root") == str(scratch))
    with patch.object(driver, "scratch_bytes", return_value=6_000_000_000), patch.object(driver, "render") as rendered:
        driver.finalize_only(SimpleNamespace(finalize_only=out, metadata_only=False, scratch_root=None,
                                            scratch_limit_bytes=None, sheet_every=3))
        ok &= row(results, "finalize/encodes-below-retained-budget", rendered.call_count == 1)
    # The cap judges the retained set: a finalizer over it still encodes, then retires the frames it encoded.
    with patch.object(driver, "scratch_bytes", return_value=8_000_000_000), patch.object(driver, "render") as rendered, \
            patch.object(driver, "retire_transients") as retired:
        driver.finalize_only(SimpleNamespace(finalize_only=out, metadata_only=False, scratch_root=None,
                                            scratch_limit_bytes=None, sheet_every=3))
        ok &= row(results, "finalize/encodes-and-retires-at-any-footprint", rendered.call_count == 1 and retired.call_count == 1)
    saved["scenario_definition"]["runs"] = [{"name": "first"}]
    saved.pop("interrupted", None)
    saved["runs"][0].pop("interrupted", None)
    (peer / "launch.json").write_text(json.dumps({"started": True, "exit_code": 0, "elapsed_seconds": 7.5}), encoding="utf-8")
    (out / "capture.json").write_text(json.dumps(saved), encoding="utf-8")
    (out / "manifest.json").write_text(json.dumps({"wall_seconds": 7.5}), encoding="utf-8")
    driver.finalize_only(SimpleNamespace(finalize_only=out, metadata_only=True, scratch_root=scratch, sheet_every=3))
    complete = json.loads((out / "capture.json").read_text())
    measured = json.loads((out / "manifest.json").read_text())
    ok &= row(results, "finalize/preserves-completed-run-and-wall-time", not complete.get("interrupted") and measured["wall_seconds"] == 7.5)
    return ok


def check_completion(results, scratch):
    probe = scratch / "completion-probe"
    probe.mkdir()
    peer = {"probe_dir": str(probe), "record": {"exit_code": 0, "timed_out": False},
            "index": [{"frame": 0}], "video": "retained.mp4", "menu_script_failures": []}
    ok = row(results, "completion/clean", driver.peer_completed(peer))
    (probe / "probe.json").write_text('{}', encoding="utf-8")
    (probe / "net-ui-result.json").write_text('{"pass": false, "complete": false}', encoding="utf-8")
    ok &= row(results, "completion/probe-failure-with-zero-exit", not driver.peer_completed(peer))
    (probe / "net-ui-result.json").write_text('{"pass": true, "complete": true}', encoding="utf-8")
    peer["record"].update(exit_code=137, injected_termination="scenario drop after recorded tick 900")
    ok &= row(results, "completion/unplanned-exit", not driver.peer_completed(peer))
    peer["expected_termination"] = True
    ok &= row(results, "completion/planned-drop", driver.peer_completed(peer))
    peer["record"]["injected_termination"] = "another scenario peer failed"
    ok &= row(results, "completion/observer-abort-is-not-a-planned-drop", not driver.peer_completed(peer))
    return ok


def check_drop_receipts(results, scratch):
    root = scratch / "drop-receipt"
    video = root / "video"
    video.mkdir(parents=True)
    record = {"root": str(root), "video_dir": str(video), "probe_dir": str(root / "probe"),
              "record": {}, "index": [{"frame": 0, "wall_ms": 1000, "sim_tick": 601, "screen": "game"}]}
    item = {"id": "drop", "drop_tick": 601, "screen": "game"}
    _, evidence = driver.item_evidence(record, item)
    ok = row(results, "drop/no-receipt-is-not-proof", evidence["probe"] == "fail")
    driver.write_json(video / "injected-drop.json", {"requested_tick": 601, "last_recorded_frame": record["index"][0]})
    record["record"]["injected_termination"] = "capture interrupted"
    _, evidence = driver.item_evidence(record, item)
    ok &= row(results, "drop/observer-stop-is-not-injected-drop", evidence["probe"] == "fail")
    record["record"]["injected_termination"] = "scenario drop after recorded tick 601"
    _, evidence = driver.item_evidence(record, item)
    ok &= row(results, "drop/receipt-and-runner-termination", evidence["probe"] == "pass")
    record.update(peer="client", video=str(root / "client.mp4"), menu_script_failures=[])
    host = {**record, "peer": "host"}
    scenario = {"name": "drop", "checklist": [{"id": "host-sees-drop", "peer": "host", "screen": "game",
                 "peer_drop": {"peer": "client", "tick": 601}}]}
    capture = {"name": "run0", "peers": [host, record]}
    document = driver.review(scenario, capture, root)
    ok &= row(results, "drop/survivor-requires-peer-receipt", document["checklist"][0]["peer_drop"]["pass"])
    record["record"]["injected_termination"] = None
    document = driver.review(scenario, capture, root)
    ok &= row(results, "drop/peer-failure-is-not-planned-drop", document["checklist"][0]["probe"] == "fail")
    (root / "runtime").mkdir()
    (root / "runtime/LogConsole.txt").write_text("later process\n")
    (root / "console.log").write_text("original process\n")
    matches = driver.log_assertions(root, ["original process"], ["later process"])
    ok &= row(results, "resume/console-evidence-is-stable", len(matches[0]["matches"]) == 1 and not matches[1]["matches"])
    probe = root / "probe"
    probe.mkdir()
    value = {"steps": [{"index": 7, "observed": {"control": {"text": "ClientA is now hosting", "visible": True}}}]}
    driver.write_json(probe / "net-ui-result.json", value)
    item = {"screen": "game", "probe_steps": [7], "readback": [{"step": 7, "path": ["control", "text"], "contains": "is now hosting"}, {"step": 7, "path": ["control", "visible"], "equals": True}]}
    _, evidence = driver.item_evidence(record, item)
    ok &= row(results, "readback/visible-named-toast", evidence["probe"] == "pass")
    value["steps"][0]["observed"]["control"]["visible"] = False
    driver.write_json(probe / "net-ui-result.json", value)
    _, evidence = driver.item_evidence(record, item)
    ok &= row(results, "readback/hidden-text-is-not-visible-proof", evidence["probe"] == "fail")
    value["steps"][0]["observed"]["control"].update(visible=True, text="LIVE")
    driver.write_json(probe / "net-ui-result.json", value)
    _, evidence = driver.item_evidence(record, item)
    ok &= row(results, "readback/changed-toast-fails-without-blocking-observer", evidence["probe"] == "fail")
    return ok


def check_gameplay_epochs(results, scratch):
    video, stage = scratch / "epochs-video", scratch / "epochs-stage"
    video.mkdir(); stage.mkdir()
    rows = [{"frame": 0, "screen": "game", "sim_tick": 10, "wall_ms": 100},
            {"frame": 1, "screen": "Pause", "sim_tick": 20, "wall_ms": 200},
            {"frame": 2, "screen": "game", "sim_tick": 30, "wall_ms": 300},
            {"frame": 3, "screen": "game", "sim_tick": 0, "wall_ms": 400},
            {"frame": 4, "screen": "game", "sim_tick": 10, "wall_ms": 500}]
    (video / "frames.jsonl").write_text("".join(json.dumps(row) + "\n" for row in rows))
    driver.gameplay_signals(video, stage, 2)
    first = json.loads((stage / "gameplay-started.json").read_text())
    second = json.loads((stage / "gameplay-epoch-2.json").read_text())
    return row(results, "epochs/waits-for-recorded-counter-reset", first["frame"] == 0 and second["frame"] == 3 and second["sim_tick"] == 0)


def check_menu_commands(results):
    repo = TOOLS.parent
    source = (repo / "Source/Main.cpp").read_text() + (repo / "Source/Menus/MenuAutomation.cpp").read_text()
    known = set(re.findall(r'(?:command|cmd|op)\s*==\s*"([a-z_]+)"', source))
    unknown = []
    for path in driver.SCENARIO_DIR.glob("*.txt"):
        if ".menu." not in path.name and path.name not in ("sp-smoke.menus.txt", "wait-for-probe.txt"):
            continue
        for number, line in enumerate(path.read_text().splitlines(), 1):
            words = line.strip().split()
            if not words or words[0].startswith("#"):
                continue
            verb = words[1] if words[0] == "observe" else words[0]
            if verb not in known:
                unknown.append(f"{path.name}:{number}: {verb}")
    ok = row(results, "menu/verbs-exist-in-engine", not unknown, str(unknown))
    ok &= row(results, "menu/rejects-misspelled-ready-wait", "wait_remote_ready" in known and "wait_remoteready" not in known)
    return ok


def check_resume_seams(results, scratch):
    import run_sim_test
    from compare_sim_traces import CORE
    tokens = driver.supplied_tokens(["TURN_USER=override"], {"TURN_USER": "env", "TURN_PASS": "secret"})
    ok = row(results, "tokens/environment-and-override", tokens == {"TURN_USER": "override", "TURN_PASS": "secret"})
    scenario = {"name": "tokens", "path": "unit", "peers": [{"name": "host", "args": ["{TURN_SERVER}"]}]}
    missing = driver.run_preflight(scenario, {}, [], {})
    ok &= row(results, "tokens/unresolved-prevents-launch", missing["class"] == "harness" and missing["tokens"] == ["TURN_SERVER"])
    ok &= row(results, "tokens/resolved", driver.run_preflight(scenario, {}, [], {"TURN_SERVER": "turn:example"}) is None)
    scenario["peers"][0]["args"] = ["{DIRECTORY_ROOT}/listed.json"]
    ok &= row(results, "tokens/directory-service-root", driver.run_preflight(scenario, {}, [], {}) is None)
    same = {"name": "same", "peers": [{"name": "first", "args": []}, {"name": "second", "args": [], "retain_runtime_from": {"run": "same", "peer": "first"}, "start_when": {"peer": "first", "ended": True}}]}
    ok &= row(results, "resume/same-run-owner-must-end", driver.run_preflight({"path": "unit"}, same, [], {}) is None)
    same["peers"][1]["start_when"] = {"peer": "first", "sim_tick": 10}
    ok &= row(results, "resume/rejects-concurrent-runtime-use", driver.run_preflight({"path": "unit"}, same, [], {})["class"] == "harness")
    indexed = [{"frame": 0, "screen": "game", "sim_tick": 100, "service_state": "Starting"}, {"frame": 1, "screen": "game", "sim_tick": 1200, "service_state": "Running"}]
    ok &= row(results, "frames/rejoin-requires-running-service", driver.frame_range(indexed, {"screen": "game", "service_state": "Running"}) == [1, 1])
    runtime = scratch / "retained/runtime"
    (runtime / "Userdata").mkdir(parents=True)
    (runtime / "Autosaves").mkdir()
    (runtime / "Temp").mkdir()
    (runtime / "Userdata/Settings.ini").write_text("SettingsMan\n")
    (runtime / "Userdata/NetworkIdentity.key").write_bytes(b"same-identity")
    for tick in (60, 120):
        (runtime / f"Autosaves/match-{tick}.ccmanifest").write_text(f"MatchId = match\nSavedTick = {tick}\n")
        (runtime / f"Autosaves/match-{tick}.ccsave").write_bytes(b"retained-checkpoint")
    (runtime / "Autosaves/match.admission").write_bytes(b"same-admission")
    peer = {"peer": "host", "root": str(runtime.parent), "runtime": str(runtime), "record": {"exit_code": 137}}
    captures = [{"name": "died", "peers": [peer]}]
    condition = {"run": "died", "peers": ["host"], "ended": True}
    ok &= row(results, "resume/ended-prior-run", driver.cross_run_ready(captures, condition))
    ok &= row(results, "resume/no-missing-peer", not driver.cross_run_ready(captures, {**condition, "peers": ["host", "client"]}))
    saved, evidence = driver.checkpoint_tokens(peer)
    ok &= row(results, "resume/newest-complete-checkpoint", saved == {"RESUME_MATCH": "match", "RESUME_TICK": 120} and len(evidence) == 4)
    original = run_sim_test.IsolatedRun
    try:
        run_sim_test.IsolatedRun = lambda argv, cwd, out, timeout, **kwargs: SimpleNamespace(argv=argv, cwd=cwd, out=out, **kwargs)
        handle = run_sim_test.make_run(scratch, ["-test"], scratch / "resumed/host", runtime=runtime)
        ok &= row(results, "resume/same-runtime-without-copy", handle.cwd == runtime.resolve() and not (handle.out / "runtime").exists() and (runtime / "Userdata/NetworkIdentity.key").read_bytes() == b"same-identity")
    finally:
        run_sim_test.IsolatedRun = original
    rows = [{"tick": tick, "total": "a" * 64, "subsystems": {key: "b" * 64 for key in CORE | {"controller"}}} for tick in range(1, 6)]
    paths = [scratch / "left.json", scratch / "right.json"]
    for path in paths:
        driver.write_json(path, {"runs": [{"tick_hashes": rows}]})
    result = driver.compare_hash_range(*paths, 3, 5, scratch / "range-ok")
    ok &= row(results, "migration/full-post-boundary-range", result["status"] == "PASS" and result["exclusions"] == [])
    gate_root = scratch / "round-gate"
    gate_root.mkdir()
    for name in ("host", "client"):
        driver.write_json(gate_root / f"{name}_trace.json", {"runs": [{"tick_hashes": rows}]})
    capture = {"root": str(gate_root), "peers": [{"peer": "host"}, {"peer": "client"}]}
    driver.feel_probes({"hash_gate": {"name": "round2-hashes", "peers": ["host", "client"], "first_tick": 1, "cap": 5}}, capture, {})
    ok &= row(results, "hash-gate/both-peers-share-full-record-proof", all(peer["gates"]["round2-hashes"]["status"] == "PASS" for peer in capture["peers"]))
    rows[3]["subsystems"]["controller"] = "c" * 64
    driver.write_json(paths[1], {"runs": [{"tick_hashes": rows}]})
    result = driver.compare_hash_range(*paths, 3, 5, scratch / "range-controller")
    ok &= row(results, "migration/controller-is-not-excluded", result["status"] == "FAIL" and result["first_difference"] == 4)
    driver.write_json(paths[1], {"runs": [{"tick_hashes": rows[:-1]}]})
    result = driver.compare_hash_range(*paths, 3, 5, scratch / "range-missing")
    ok &= row(results, "migration/missing-cap-fails", result["status"] == "FAIL" and not result["full_rows_equal"])
    driver.write_json(paths[1], {"runs": [{"tick_hashes": rows[:2]}]})
    result = driver.compare_hash_range(*paths, 3, 5, scratch / "range-prefix-only")
    ok &= row(results, 'migration/prefix-names-first-missing-tick', result['status'] == 'FAIL' and result['first_missing_tick'] == 3)
    driver.write_json(paths[1], {"runs": [{"tick_hashes": rows[:2] + rows[3:]}]})
    result = driver.compare_hash_range(*paths, 3, 5, scratch / "range-gap")
    ok &= row(results, 'migration/gap-keeps-strict-failure-and-missing-tick', result['status'] == 'FAIL' and result['first_missing_tick'] == 3 and bool(result['validation_errors']))
    return ok


def check_cross_capture(results, scratch):
    from e2e.cross import select_peer, identity_agrees, merge_halves
    scenario = driver.load_scenario('mp-host-join-cross')
    host = select_peer(scenario, 'host')
    client = select_peer(scenario, 'client')
    ok = row(results, 'cross/one-local-peer', [peer['name'] for peer in host['peers']] == ['host'] and [peer['name'] for peer in client['peers']] == ['client'])
    ok &= row(results, 'cross/no-remote-file-rendezvous', all('{PROBE_DIR_' not in driver.scenario_text(value, value['peers'][0]['probe']) for value in (host, client)))
    ok &= row(results, 'cross/missing-address-skips-client', driver.run_preflight(client, {'name': 'run0', 'peers': client['peers']}, [], {})['tokens'] == ['HOST_ADDRESS'])
    ok &= row(results, 'cross/host-needs-no-address-token', driver.run_preflight(host, {'name': 'run0', 'peers': host['peers']}, [], {}) is None)
    ok &= row(results, 'cross/address-token-from-environment', driver.supplied_tokens([], {'HOST_ADDRESS': '192.0.2.1'}) == {'HOST_ADDRESS': '192.0.2.1'})
    identity = {'session_id': 5, 'round': 7, 'config_hash': 'a' * 64, 'peer_id': 1, 'host': True}
    remote = {**identity, 'peer_id': 2, 'host': False}
    ok &= row(results, 'cross/shared-native-identity', identity_agrees([identity, remote]))
    ok &= row(results, 'cross/reject-different-round', not identity_agrees([identity, {**remote, 'round': 8}]))
    ok &= row(results, 'cross/reject-same-peer', not identity_agrees([identity, {**remote, 'peer_id': 1}]))
    roots = []
    for name, definition, ident, platform in [('host', host, identity, 'win32'), ('client', client, remote, 'darwin')]:
        root = scratch / ('cross-' + name)
        root.mkdir()
        roots.append(root)
        video, sheet = root / (name + '.mp4'), root / (name + '-sheet.png')
        video.write_bytes(b'synthetic contract video')
        sheet.write_bytes(b'synthetic contract sheet')
        driver.write_json(root / 'capture.json', {'synthetic': True, 'scenario': scenario['name'], 'scenario_definition': definition, 'out': str(root)})
        driver.write_json(root / 'manifest.json', {'synthetic': True, 'platform': platform, 'source': {'tip': 'synthetic'}, 'frame_count': 1, 'peers': [{'peer': name, 'match_identity': ident, 'video': driver.file_evidence(video), 'contact_sheet': driver.file_evidence(sheet)}]})
        driver.write_json(root / 'review.json', {'scenario': scenario['name'], 'checklist': [{'id': name, 'peer': name, 'frames': [0, 0], 'probe': 'pass'}]})
    ok &= row(results, 'cross/merged-media-and-contract', merge_halves(roots, scratch / 'cross-merged'))
    merged = json.loads((scratch / 'cross-merged/manifest.json').read_text())
    ok &= row(results, 'cross/shared-key-and-platforms', merged['match_identity_gate']['match_id'] == '0000000000000005-0000000000000007' and merged['match_identity_gate']['cross_platform'])
    # Every real half carries the frameless assert-dialog row; it passes on its probe and fails the merge when it fails.
    for root, name in zip(roots, ('host', 'client')):
        driver.write_json(root / 'review.json', {'scenario': scenario['name'], 'checklist': [
            {'id': name, 'peer': name, 'frames': [0, 0], 'probe': 'pass'},
            {'id': 'no-assert-dialogs', 'peer': 'all', 'frames': None, 'state': 'checked', 'probe': 'pass'}]})
    ok &= row(results, 'cross/assert-dialog-row-passes-on-its-probe', merge_halves(roots, scratch / 'cross-merged-dialogs'))
    dialog = json.loads((roots[1] / 'review.json').read_text())
    dialog['checklist'][1].update(probe='fail', finding={'class': 'engine', 'reason': 'assert dialog: synthetic'})
    driver.write_json(roots[1] / 'review.json', dialog)
    ok &= row(results, 'cross/assert-dialog-fails-the-merge', not merge_halves(roots, scratch / 'cross-merged-dialog-red'))
    dialog['checklist'][1].update(probe='pass')
    dialog['checklist'][1].pop('finding')
    driver.write_json(roots[1] / 'review.json', dialog)
    (roots[1] / 'client.mp4').write_bytes(b'changed synthetic video')
    ok &= row(results, 'cross/transferred-media-tamper-fails', not merge_halves(roots, scratch / 'cross-tampered'))
    bad = {'name': 'run0', 'peers': [{'name': 'host', 'kill_when': {'peer': 'absent', 'event': 'ready'}}]}
    ok &= row(results, 'drop/unknown-event-peer-skips-before-launch', driver.run_preflight({'peers': bad['peers']}, bad, [], {})['class'] == 'harness')
    good = {'name': 'run0', 'peers': [{'name': 'host', 'kill_when': {'peer': 'host', 'probe_complete': True}}]}
    ok &= row(results, 'drop/completed-probe-gate', driver.run_preflight({'peers': good['peers']}, good, [], {}) is None)
    result = scratch / 'completed-probe.json'
    result.write_text('{"complete":true,"pass":false}')
    ok &= row(results, 'drop/failed-probe-is-not-planned-completion', not driver.completed_probe(result))
    result.write_text('{"complete":true,')
    ok &= row(results, 'drop/partial-write-keeps-observing', not driver.completed_probe(result))
    result.write_text('{"complete":true,"pass":true}')
    ok &= row(results, 'drop/successful-probe-completes', driver.completed_probe(result))
    return ok


def check_cross_transfer(results, scratch):
    from e2e.cross import CaptureRelocation, merge_halves, select_peer
    scenario = driver.load_scenario('mp-host-join-cross')
    identities = {'host': {'session_id': 5, 'round': 7, 'config_hash': 'a' * 64, 'peer_id': 1, 'host': True},
                  'client': {'session_id': 5, 'round': 7, 'config_hash': 'a' * 64, 'peer_id': 2, 'host': False}}
    roots, originals = [], {}
    for name, platform in (('host', 'win32'), ('client', 'darwin')):
        root = scratch / ('transferred-' + name)
        (root / 'run0' / (name + '-stage') / 'probe').mkdir(parents=True)
        roots.append(root)
        original = str(root) if name == 'host' else '/Users/erol/captures/client-half'
        originals[name] = original
        def evidence(relative, data):
            path = root / relative
            path.write_bytes(data)
            return {**driver.file_evidence(path), 'path': original.replace('\\', '/') + '/' + relative}
        video = evidence(f'run0/{name}.mp4', b'synthetic transferred video')
        sheet = evidence(f'run0/{name}-sheet.png', b'synthetic transferred sheet')
        native = evidence(f'run0/{name}-stage/probe/match-identity.json', json.dumps(identities[name]).encode())
        trace = evidence(f'run0/{name}_trace.json', b'{"synthetic":true,"ticks":[1,2]}')
        peer = {'peer': name, 'match_identity': identities[name], 'video': video, 'contact_sheet': sheet,
                'identity_file': native, 'trace': trace}
        driver.write_json(root / 'capture.json', {'scenario': scenario['name'], 'scenario_definition': select_peer(scenario, name), 'out': original})
        driver.write_json(root / 'manifest.json', {'platform': platform, 'source': {'tip': 'synthetic-transfer'}, 'frame_count': 2, 'peers': [peer]})
        driver.write_json(root / 'review.json', {'scenario': scenario['name'], 'command': ['--out', original], 'checklist': [{
            'id': name, 'peer': name, 'frames': [0, 1], 'probe': 'pass', 'video': video['path'], 'contact_sheet': sheet['path'],
            'identity_file': native, 'trace': trace, 'readback': [{'path': ['control', 'visible'], 'equals': True}]}]})
    before = {str(path): path.read_bytes() for root in roots for path in (root / 'capture.json', root / 'manifest.json', root / 'review.json')}
    ok = row(results, 'cross-transfer/unix-half-relocates', merge_halves(roots, scratch / 'transferred-merged'))
    manifest = json.loads((scratch / 'transferred-merged/manifest.json').read_text())
    review = json.loads((scratch / 'transferred-merged/review.json').read_text())
    peer = next(peer for peer in manifest['peers'] if peer['peer'] == 'client')
    item = next(item for item in review['checklist'] if item['peer'] == 'client')
    ok &= row(results, 'cross-transfer/original-media-path-retained', peer['video'].get('original_path') == originals['client'] + '/run0/client.mp4')
    ok &= row(results, 'cross-transfer/review-media-and-auxiliary-paths', item['video'] == str(roots[1] / 'run0/client.mp4') and
              item['identity_file']['path'] == str(roots[1] / 'run0/client-stage/probe/match-identity.json') and
              item['trace']['path'] == str(roots[1] / 'run0/client_trace.json'))
    ok &= row(results, 'cross-transfer/provenance-and-inputs-unchanged', bool(manifest.get('relocations')) and
              all(Path(path).read_bytes() == data for path, data in before.items()))
    ok &= row(results, 'cross-transfer/property-path-is-not-a-file', item['readback'][0]['path'] == ['control', 'visible'])
    trace_path = roots[1] / 'run0/client_trace.json'
    trace_bytes = trace_path.read_bytes()
    trace_path.write_bytes(b'changed trace')
    ok &= row(results, 'cross-transfer/changed-trace-refused', not merge_halves(roots, scratch / 'transferred-bad-trace'))
    trace_path.write_bytes(trace_bytes)
    native_path = roots[1] / 'run0/client-stage/probe/match-identity.json'
    native_bytes = native_path.read_bytes()
    native_path.write_bytes(b'{"changed":true}')
    ok &= row(results, 'cross-transfer/changed-identity-refused', not merge_halves(roots, scratch / 'transferred-bad-identity'))
    native_path.write_bytes(native_bytes)
    client_manifest = json.loads((roots[1] / 'manifest.json').read_text())
    outside = scratch / 'outside-client.mp4'
    outside.write_bytes(b'synthetic transferred video')
    saved_video = client_manifest['peers'][0]['video']
    for variant, escaped in [('outside', str(outside)), ('traversal', originals['client'] + '/../outside-client.mp4')]:
        client_manifest['peers'][0]['video'] = {**saved_video, 'path': escaped}
        driver.write_json(roots[1] / 'manifest.json', client_manifest)
        ok &= row(results, f'cross-transfer/{variant}-refused', not merge_halves(roots, scratch / ('transferred-' + variant)))
    client_manifest['peers'][0]['video'] = saved_video
    saved_trace = client_manifest['peers'][0].pop('trace')
    client_manifest['cross_auxiliary'] = {'schema': 1, 'required': ['identity_file', 'trace']}
    driver.write_json(roots[1] / 'manifest.json', client_manifest)
    ok &= row(results, 'cross-transfer/fresh-capture-requires-auxiliary-hashes', not merge_halves(roots, scratch / 'transferred-missing-required-trace'))
    client_manifest['peers'][0]['trace'] = saved_trace
    driver.write_json(roots[1] / 'manifest.json', client_manifest)
    client_review = json.loads((roots[1] / 'review.json').read_text())
    client_review['checklist'][0]['video'] = str(outside)
    driver.write_json(roots[1] / 'review.json', client_review)
    ok &= row(results, 'cross-transfer/review-path-outside-refused', not merge_halves(roots, scratch / 'transferred-review-outside'))
    relocation = CaptureRelocation(originals['client'], roots[1])
    with patch.object(Path, 'resolve', return_value=scratch / 'outside-resolved-file'):
        try:
            relocation.path(originals['client'] + '/run0/client.mp4', 'video')
            rejected = False
        except ValueError:
            rejected = True
    ok &= row(results, 'cross-transfer/resolved-link-escape-refused', rejected)
    produced = scratch / 'cross-producer'
    (produced / 'probe').mkdir(parents=True)
    driver.write_json(produced / 'probe/match-identity.json', identities['host'])
    (produced / 'host_trace.json').write_text('{"synthetic":true}')
    native_peer = {'peer': 'host', 'root': str(produced), 'record': {}, 'index': [], 'manifest': {},
                   'probe_dir': str(produced / 'probe'), 'args': ['-out', str(produced / 'host_trace.json')]}
    capture = {'scenario': scenario['name'], 'scenario_definition': select_peer(scenario, 'host'),
               'source': {'tip': 'synthetic'}, 'exe': {}, 'started': driver.stamp(), 'fps': 6,
               'runs': [{'name': 'run0', 'size': '640x360', 'peers': [native_peer]}]}
    produced_manifest = driver.scenario_manifest(capture, produced, 1)
    ok &= row(results, 'cross-transfer/producer-hashes-native-identity-and-trace',
              produced_manifest.get('cross_auxiliary', {}).get('required') == ['identity_file', 'trace'] and
              produced_manifest['peers'][0]['identity_file']['sha256'] == driver.file_evidence(produced / 'probe/match-identity.json')['sha256'] and
              produced_manifest['peers'][0]['trace']['sha256'] == driver.file_evidence(produced / 'host_trace.json')['sha256'])
    return ok


def check_migration_timing(results, scratch):
    from e2e.timing import migration_timing
    root = scratch / 'timing'
    root.mkdir()
    events = [
        {'wall_ms': 2000, 'message': 'net_status peer=2 host_lost=1 local_slow=1'},
        {'wall_ms': 4600, 'message': 'net_status peer=2 host_lost=0 local_slow=1'},
        {'wall_ms': 4700, 'message': 'handover_toast peer=2 longest_ms=2700 text=Host left - ClientA is now hosting'},
        {'wall_ms': 5000, 'message': 'net_status peer=2 host_lost=0 local_slow=0'},
        {'wall_ms': 5500, 'message': 'net_status peer=2 host_lost=0 local_slow=1'}]
    (root / 'events.jsonl').write_text('\n'.join(json.dumps(event) for event in events))
    capture = {'peers': [{'peer': 'host', 'index': [{'frame': 1, 'screen': 'game', 'wall_ms': 1900}]},
                         {'peer': 'clienta', 'video_dir': str(root), 'index': [{'frame': 2, 'wall_ms': 4750}, {'frame': 3, 'wall_ms': 6000}]}]}
    item = migration_timing(capture, ['clienta'])['survivors'][0]
    ok = row(results, 'migration/timing-from-host-last-frame', item['seconds_from_host_last_frame'] == 2.8 and item['overlay_longest_ms_at_toast'] == 2700 and item['toast_nearest_frame']['frame'] == 2)
    ok &= row(results, 'migration/complete-and-truncated-slow-banners', item['local_slow_intervals'] == [{'start_ms': 2000, 'end_ms': 5000, 'seconds': 3.0, 'complete': True}, {'start_ms': 5500, 'end_ms': 6000, 'seconds': .5, 'complete': False}])
    capture['peers'][0]['index'] = []
    item = migration_timing(capture, ['clienta'])['survivors'][0]
    ok &= row(results, 'migration/no-invented-loss-time', not item['timing_complete'] and item['seconds_from_host_last_frame'] is None)
    return ok


def check_recorder_flush(results, scratch):
    """A probe-complete drop waits for the frames the recorder already took; it never waits past its bound."""
    video = scratch / "recorder-flush" / "video"
    video.mkdir(parents=True)
    stop = threading.Event()
    missing = driver.await_recorder(video, stop, timeout_s=.2)
    ok = row(results, "recorder-flush/no-output-does-not-wait", missing["flushed"] is False and missing["reason"] == "no recorder output")
    (video / "events.jsonl").write_text('{"wall_ms": 1000, "message": "video_mark toast-open"}\n{"wall_ms": 1200, "message": "net_status"}\n')
    (video / "frames.jsonl").write_text('{"frame": 0, "wall_ms": 990, "saved": true}\n')
    behind = driver.await_recorder(video, stop, timeout_s=.2)
    ok &= row(results, "recorder-flush/behind-times-out", behind["flushed"] is False and behind["reason"] == "timed out"
              and behind["target_wall_ms"] == 1200 and behind["last_frame_wall_ms"] == 990, str(behind))

    def write_late_frame():
        with (video / "frames.jsonl").open("a") as index:
            index.write('{"frame": 1, "wall_ms": 1210, "saved": true}\n')
    writer = threading.Timer(.1, write_late_frame)
    writer.start()
    caught_up = driver.await_recorder(video, stop, timeout_s=5)
    writer.join()
    ok &= row(results, "recorder-flush/waits-for-the-written-frame", caught_up["flushed"] is True and caught_up["last_frame_wall_ms"] == 1210, str(caught_up))
    stop.set()
    (video / "events.jsonl").write_text('{"wall_ms": 5000, "message": "late"}\n')
    stopped = driver.await_recorder(video, stop, timeout_s=5)
    ok &= row(results, "recorder-flush/stops-with-the-watchers", stopped["flushed"] is False and stopped["reason"] == "capture stopping", str(stopped))
    return ok


def check_log_gates(results, scratch):
    """A gate on a peer's own stdout line: a drop or a start waits for the product to print it."""
    log = scratch / "log-gate" / "stdout.log"
    log.parent.mkdir()
    log.write_text("[net-match] held client: replaying the private committed tail\n")
    pattern = r"\[net-match\] private catch-up complete frame=\d+"
    ok = row(results, "log-gate/unprinted-line-is-unmet", not driver.log_line_seen(log, pattern))
    ok &= row(results, "log-gate/missing-log-is-unmet", not driver.log_line_seen(log.parent / "absent.log", pattern))
    log.write_text(log.read_text() + "[net-match] private catch-up complete frame=742\n")
    ok &= row(results, "log-gate/printed-line-is-met", driver.log_line_seen(log, pattern))
    peers = [{"name": "client", "kill_when": {"peer": "client", "log": pattern}}]
    ok &= row(results, "log-gate/drop-on-a-log-line-passes-preflight", driver.run_preflight({"peers": peers}, {"name": "run0", "peers": peers}, [], {}) is None)
    both = [{"name": "client", "kill_when": {"peer": "client", "log": pattern, "event": "video_mark x"}}]
    ok &= row(results, "log-gate/drop-with-two-triggers-is-refused",
              driver.run_preflight({"peers": both}, {"name": "run0", "peers": both}, [], {})["class"] == "harness")
    scenario = driver.load_scenario("mp-rollback-lag")
    peers = {peer["name"]: peer for peer in scenario["runs"][0]["peers"]}
    drops = peers["client"].get("kill_when") or []
    # The drop follows the catch-up from the silent seat's hold, never an earlier return from a slow-player hold.
    silent = scenario["runs"][0]["feel_gate"]["silent_tick"]
    line = "[net-match] private catch-up complete frame={} in_place=1"
    ok &= row(results, "log-gate/rollback-lag-drops-the-client-after-catch-up",
              any(gate.get("log") and re.search(gate["log"], line.format(silent)) and not re.search(gate["log"], line.format(silent - 1))
                  for gate in drops))
    args = peers["host"]["args"]
    round_ticks = int(args[args.index("-net-match-ticks") + 1])
    window_end = scenario["runs"][0]["feel_gate"].get("ticks")
    # The fallback reads the host's own scripted-input line (its sim clock, not its recorded frames, which trail the sim
    # under load) and leaves the relaunch at least 600 ticks (10 s at 60 Hz) of the round.
    host_gate = next((gate["log"] for gate in drops if gate.get("peer") == "host" and gate.get("log")), None)
    fires = [tick for tick in range(1, round_ticks + 1) if host_gate and re.search(host_gate, f"[input-script] tick {tick} player 0 pressed FIRE")]
    ok &= row(results, "log-gate/rollback-lag-drops-inside-the-round-without-a-catch-up",
              bool(fires) and fires[0] > window_end - 60 and fires[0] <= round_ticks - 600, f"first host tick={fires[:1]} round={round_ticks}")
    ok &= row(results, "feel-window/rollback-lag-gates-keep-the-1200-tick-window", window_end == 1200 and round_ticks > window_end,
              f"window_end={window_end} round={round_ticks}")
    fixture = next(entry for entry in peers["host"]["runtime_files"] if entry.get("copy") == "tools/feel/FeelBaseline.lua")
    staged = driver.staged_copy(driver.Path(__file__).resolve().parents[1] / fixture["copy"], fixture.get("replace", [])).decode("utf-8")
    ok &= row(results, "feel-window/rollback-lag-activity-runs-the-whole-round", f"local WINDOW_TICKS = {round_ticks};" in staged)
    bad = scratch / "staged-copy.lua"
    bad.write_text("local A = 1;\nlocal A = 1;\n", encoding="utf-8")
    try:
        driver.staged_copy(bad, [[r"^local A = \d+;$", "local A = 2;"]])
        refused = False
    except ValueError:
        refused = True
    ok &= row(results, "staged-copy/ambiguous-replacement-is-refused", refused)
    rejoin = [item for item in scenario["checklist"] if item.get("peer") == "client-rejoined"]
    ok &= row(results, "feel-window/rollback-lag-rejoin-is-judged-on-the-reclaim",
              bool(rejoin) and all(any("seat-reclaimed" in pattern for pattern in item.get("log_regex", [])) for item in rejoin))
    either = [{"name": "host"}, {"name": "client", "kill_when": [{"peer": "client", "log": pattern}, {"peer": "host", "sim_tick": 800}]}]
    ok &= row(results, "log-gate/first-of-several-drop-gates-passes-preflight", driver.run_preflight({"peers": either}, {"name": "run0", "peers": either}, [], {}) is None)
    stray = [{"name": "client", "kill_when": [{"peer": "client", "log": pattern}, {"peer": "absent", "sim_tick": 800}]}]
    ok &= row(results, "log-gate/unknown-peer-in-a-gate-list-is-refused", driver.run_preflight({"peers": stray}, {"name": "run0", "peers": stray}, [], {})["class"] == "harness")
    ok &= row(results, "log-gate/rollback-lag-survivor-waits-for-the-client-route", peers["survivor"].get("start_when", {}).get("peer") == "host"
              and "RouteAllowed" in peers["survivor"]["start_when"].get("log", ""))
    return ok


def check_fullstate_applicability(results, scratch):
    """A pair that never started a lockstep round has no full-state verdict; one that started and shares no sample is red."""
    root = scratch / "fullstate-applicability"
    root.mkdir()
    host, client = root / "host.log", root / "client.log"
    host.write_text("[menu-script] dump_lobby state=Completed members=2\n")
    client.write_text("[menu-script] dump_lobby state=Failed members=1\n")
    lobby = driver.fullstate_verdict(host, client)
    ok = row(results, "fullstate/lobby-scenario-is-not-applicable", lobby["passed"] is None and bool(lobby["not_applicable"]), str(lobby))
    capture = {"name": "run0", "peers": [], "fullstate": {"host/client": lobby}}
    document = driver.review({"name": "lobby", "checklist": []}, capture, root)
    ok &= row(results, "fullstate/not-applicable-is-no-finding", not any("full-state" in f["reason"] for f in document["run_findings"]))
    client.write_text(driver.LOCKSTEP_ROUND_START + "1 frame=1 local_peer=2 peers=2 input_delay=3\n")
    started = driver.fullstate_verdict(host, client)
    ok &= row(results, "fullstate/started-round-without-samples-is-red", started["passed"] is False
              and started["reasons"] == ["the peers share no full-state sample"], str(started))
    capture["fullstate"] = {"host/client": started}
    document = driver.review({"name": "lobby", "checklist": []}, capture, root)
    ok &= row(results, "fullstate/red-verdict-is-a-finding", any("full-state oracle" in f["reason"] for f in document["run_findings"]))
    capture["feel_window"] = {"end_tick": 1200, "measured": {"host": {"first_tick": 300, "last_tick": 1200}}}
    document = driver.review({"name": "lobby", "checklist": []}, capture, root)
    ok &= row(results, "feel-window/review-carries-the-window", document.get("feel_window") == capture["feel_window"])
    return ok


def fullstate_line(tick, sections, round_id=7):
    parts = ",".join(f"{name}:{value}" for name, value in sections.items())
    return f"[fullstate] tick={tick} hash={tick:016x} sections={parts} round={round_id}"


def check_footprint_retirement(results, scratch):
    """A run is never stopped for footprint before its verdict; its transients go once the verdict is written."""
    import gzip  # noqa: PLC0415
    root = scratch / "retire"
    with patch.object(driver, "scratch_bytes", return_value=9_000_000_000):
        try:
            peak = driver.note_footprint(root, 0)
        except RuntimeError:
            peak = None
    ok = row(results, "footprint/measured-not-enforced-mid-run", peak == 9_000_000_000, str(peak))
    peers = []
    for name, encoded in (("host", True), ("client", False)):
        peer = root / "run0" / name
        (peer / "video/frames").mkdir(parents=True)
        (peer / "feel").mkdir()
        for frame in range(3):
            (peer / f"video/frames/frame-{frame:06d}.png").write_bytes(b"png" * 100)
        (peer / "feel/raw.jsonl").write_text('{"type":"committed","tick":300,"wall_ms":5}\n' * 50, encoding="utf-8")
        video, sheet = root / "run0" / f"{name}.mp4", root / "run0" / f"{name}-sheet.png"
        if encoded:
            video.write_bytes(b"mp4")
            sheet.write_bytes(b"sheet")
        peers.append({"peer": name, "root": str(peer), "video_dir": str(peer / "video"),
                      "video": str(video) if encoded else None, "contact_sheet": str(sheet) if encoded else None})
    original = (root / "run0/host/feel/raw.jsonl").read_bytes()
    capture = {"name": "run0", "root": str(root / "run0"), "peers": peers}
    driver.retire_transients(capture)
    host, client = root / "run0/host", root / "run0/client"
    ok &= row(results, "footprint/encoded-frames-retired", not any((host / "video/frames").glob("*.png"))
              and (root / "run0/host.mp4").is_file() and (root / "run0/host-sheet.png").is_file())
    ok &= row(results, "footprint/unencoded-frames-kept", len(list((client / "video/frames").glob("*.png"))) == 3)
    ok &= row(results, "footprint/feel-record-packed-losslessly", not (host / "feel/raw.jsonl").exists()
              and gzip.decompress((host / "feel/raw.jsonl.gz").read_bytes()) == original
              and not (client / "feel/raw.jsonl").exists() and (client / "feel/raw.jsonl.gz").is_file(), str(capture.get("retired")))
    over = {"name": "run0", "peers": [], "retained": {"root": str(root), "bytes": 6_000_000_000, "limit": 5_000_000_000}}
    document = driver.review({"name": "retained", "checklist": []}, over, root)
    ok &= row(results, "footprint/retained-set-over-cap-is-a-finding",
              any(f["class"] == "harness" and "retained set 6000000000" in f["reason"] for f in document["run_findings"]))
    under = {**over, "retained": {**over["retained"], "bytes": 1_000}}
    document = driver.review({"name": "retained", "checklist": []}, under, root)
    ok &= row(results, "footprint/retained-set-under-cap-is-no-finding", not document["run_findings"])
    calls = []
    options = SimpleNamespace(fps=3, sheet_every=3, scratch_root=root, scratch_limit_bytes=1)
    with patch.object(driver, "feel_probes", side_effect=lambda *a: calls.append("verdict")), \
            patch.object(driver, "render", side_effect=lambda *a: calls.append("render")), \
            patch.object(driver, "retire_transients", side_effect=lambda *a: calls.append("retire")), \
            patch.object(driver, "scratch_bytes", return_value=9_000_000_000):
        driver.finish_run({"name": "s", "checklist": []}, {"name": "run0"}, {"name": "run0", "root": str(root / "run0"), "peers": []}, {}, options)
    ok &= row(results, "footprint/verdict-first-at-any-footprint", calls == ["verdict", "render", "retire"], str(calls))
    return ok


def check_activity_over_applicability(results, scratch):
    """A returner whose rounds began only after the activity ended has nothing to compare; one that ran before the end is red."""
    root = scratch / "activity-over"
    root.mkdir()
    host, client = root / "host.log", root / "client.log"
    shared = {"header": "a", "globals.sim_rng": "b"}
    ended = "[net-match-service-e2e] activity over at frame 1800: no full-state sample follows"
    host.write_text("\n".join([driver.LOCKSTEP_ROUND_START + "7 frame=1 local_peer=1 peers=3 input_delay=16",
                               fullstate_line(1740, shared), ended]) + "\n")
    client.write_text(driver.LOCKSTEP_ROUND_START + "7 frame=1814 local_peer=2 peers=3 input_delay=16\n")
    late = driver.fullstate_verdict(host, client)
    ok = row(results, "fullstate/returner-after-activity-end-is-not-applicable",
             late["passed"] is None and "1800" in late.get("not_applicable", ""), str(late))
    client.write_text(driver.LOCKSTEP_ROUND_START + "7 frame=1772 local_peer=2 peers=3 input_delay=16\n")
    early = driver.fullstate_verdict(host, client)
    ok &= row(results, "fullstate/returner-before-activity-end-without-sample-is-red", early["passed"] is False
              and early["reasons"] == ["the peers share no full-state sample"], str(early))
    host.write_text(host.read_text().replace(ended + "\n", ""))
    client.write_text(driver.LOCKSTEP_ROUND_START + "7 frame=1814 local_peer=2 peers=3 input_delay=16\n")
    unmarked = driver.fullstate_verdict(host, client)
    ok &= row(results, "fullstate/no-end-line-stays-red", unmarked["passed"] is False, str(unmarked))
    # Green-4 on this lane: the host's busy writer replaced its periodic capture, so the reclaim frame is a labelled capture.
    reclaim = fullstate_line(1628, shared).replace("[fullstate]", "[fullstate-reclaim]")
    host.write_text(host.read_text() + reclaim + "\n")
    client.write_text(driver.LOCKSTEP_ROUND_START + "7 frame=1628 local_peer=2 peers=3 input_delay=18\n" + reclaim + "\n")
    labelled = driver.fullstate_verdict(host, client)
    ok &= row(results, "fullstate/reclaim-frame-samples-are-compared", labelled["passed"] is True and labelled["sampled_ticks"] == [1628], str(labelled))
    client.write_text(client.read_text().replace("globals.sim_rng:b", "globals.sim_rng:c"))
    differing = driver.fullstate_verdict(host, client)
    ok &= row(results, "fullstate/differing-reclaim-frame-is-red", differing["passed"] is False and "tick 1628" in differing["reasons"][0], str(differing))
    return ok


def check_injected_exemption(results, scratch):
    """A scenario-declared injection's tick is the script once its repair items pass; nothing else is exempt."""
    root = scratch / "injected"
    root.mkdir()
    host, client = root / "host.log", root / "client.log"
    same, perturbed = {"header": "1", "globals.sim_rng": "2", "scene": "3"}, {"header": "9", "globals.sim_rng": "8", "scene": "3"}
    host.write_text("\n".join([driver.LOCKSTEP_ROUND_START + "7 frame=1 local_peer=1 peers=2 input_delay=3",
                               "[lockstep] desync at frame 240 against Client (submitted 8, sent 8, compared 8)",
                               *(fullstate_line(tick, perturbed if tick == 240 else same) for tick in (180, 240, 300))]) + "\n")
    client.write_text("\n".join([driver.LOCKSTEP_ROUND_START + "7 frame=1 local_peer=2 peers=2 input_delay=3",
                                 *(fullstate_line(tick, same) for tick in (180, 240, 300))]) + "\n")
    raw = driver.fullstate_verdict(host, client)
    repairs = ["injected-repair-overlay-host", "injected-repair-complete-client"]
    scenario = {"name": "mp-rematch",
                "runs": [{"name": "injected-desync", "injected_desync": {"peer": "host", "tick": 240, "repair_items": repairs}},
                         {"name": "plain"}],
                "checklist": [{"id": name, "run": "injected-desync", "peer": name.rsplit("-", 1)[1], "screen": "game", "what": name}
                              for name in repairs]}

    def findings(name, verdict, probe="pass"):
        def evidence(record, item, port=None):
            return [1], {"probe": probe, **({"reason": "repair absent"} if probe == "fail" else {})}
        records = [{"peer": peer, "root": str(root / peer), "video_dir": str(root / peer / "video"), "video": "x.mp4", "index": [], "encode": {}, "menu_script_failures": []}
                   for peer in ("host", "client")]
        run = {"name": name, "peers": records, "fullstate": {"host/client": json.loads(json.dumps(verdict))}}
        with patch.object(driver, "item_evidence", side_effect=evidence):
            document = driver.review(scenario, run, root)
        return [f for f in document["run_findings"] if "full-state" in f["reason"]], run

    declared, run = findings("injected-desync", raw)
    ok = row(results, "fullstate/declared-injection-exempt-when-repaired", raw["passed"] is False and not declared
             and run["fullstate"]["host/client"].get("exempted", [{}])[0].get("tick") == 240, str(run["fullstate"]))
    unrepaired, _ = findings("injected-desync", raw, probe="fail")
    ok &= row(results, "fullstate/declared-injection-red-when-repair-fails", bool(unrepaired))
    undeclared, _ = findings("plain", raw)
    ok &= row(results, "fullstate/undeclared-divergence-stays-red", bool(undeclared))
    host.write_text(host.read_text().replace(fullstate_line(300, same), fullstate_line(300, perturbed)))
    second, _ = findings("injected-desync", driver.fullstate_verdict(host, client))
    ok &= row(results, "fullstate/other-divergent-tick-stays-red", len(second) == 1 and "tick 300" in second[0]["reason"], str(second))
    # The lockstep desync the host injected is excused only where its own log names the injection (run 1 had none).
    def unplanned(marker):
        for peer in ("host", "client"):
            (root / peer).mkdir(exist_ok=True)
            lines = (["[net-test] live perturb frame=240"] if marker and peer == "host" else []) + ["[lockstep] desync at frame 240 against Peer (submitted 8, sent 8, compared 8)"]
            (root / peer / "stdout.log").write_text("\n".join(lines) + "\n")
        records = [{"peer": peer, "root": str(root / peer), "video_dir": str(root / peer / "video"), "video": "x.mp4", "index": [], "encode": {}, "menu_script_failures": []}
                   for peer in ("host", "client")]
        with patch.object(driver, "item_evidence", side_effect=lambda record, item, port=None: ([1], {"probe": "pass"})):
            document = driver.review(scenario, {"name": "injected-desync", "peers": records}, root)
        return [f for f in document["run_findings"] if f["reason"].startswith("Unplanned desync")]
    ok &= row(results, "rematch/named-injection-excuses-its-desync", not unplanned(True))
    ok &= row(results, "rematch/unnamed-injection-stays-red", len(unplanned(False)) == 2)
    rematch = driver.load_scenario("mp-rematch")
    run = next(run for run in rematch["runs"] if run["name"] == "injected-desync")
    declaration = run.get("injected_desync") or {}
    args = next((peer for peer in run["peers"] if peer["name"] == declaration.get("peer")), {}).get("args", [])
    ids = {item["id"] for item in rematch["checklist"]}
    ok &= row(results, "rematch/injection-declared-at-the-perturbed-tick",
              "-determinism-selftest-perturb-tick" in args
              and args[args.index("-determinism-selftest-perturb-tick") + 1] == str(declaration.get("tick"))
              and set(declaration.get("repair_items", [None])) <= ids)
    return ok


def check_acceptance_rows(results, scratch):
    ok = row(results, 'audit-07/missing-probe-fails', driver.probe_verdict('', {})['probe'] == 'fail')
    resumed = driver.load_scenario('mp-resume-from-disk-autosave')
    ok &= row(results, 'audit-09/autosave-reloads-and-plays', any(r.get('name') == 'resumed' for r in resumed['runs'])
              and any(i.get('sim_progress') for i in resumed['checklist']))
    root = scratch / 'round-history'; root.mkdir()
    for peer in ('host', 'client'):
        rows = [dict(round=round_id, tick=t, sim_gated=str(t), subsystems={key:str(t) for key in driver.CORE | {'controller'}})
                for round_id in (7, 8) for t in range(1, 4)]
        (root / (peer + '-live.jsonl')).write_text(''.join(json.dumps(r) + '\n' for r in rows))
    if not hasattr(driver, 'compare_round_histories'):
        return row(results, 'audit-08/every-round-required', False) and ok
    config = dict(peers=['host', 'client'], rounds=2, ticks=3)
    good = driver.compare_round_histories(root, config)
    ok &= row(results, 'audit-08/all-rounds-equal', good['status'] == 'PASS', str(good))
    rows[0]['sim_gated'] = 'different'
    (root / 'client-live.jsonl').write_text(''.join(json.dumps(r) + '\n' for r in rows))
    ok &= row(results, 'audit-08/earlier-round-divergence-fails', driver.compare_round_histories(root, config)['status'] == 'FAIL')
    rows = rows[3:]
    (root / 'client-live.jsonl').write_text(''.join(json.dumps(r) + '\n' for r in rows))
    ok &= row(results, 'audit-08/missing-earlier-round-fails', driver.compare_round_histories(root, config)['status'] == 'FAIL')
    # Run 1's rematch: the host ended the first round at 292 of 600, so only the last round plays the full count.
    early = [dict(round=round_id, tick=t, sim_gated=str(t), subsystems={key: str(t) for key in driver.CORE | {'controller'}})
             for round_id, last in ((7, 2), (8, 3)) for t in range(1, last + 1)]
    for peer in ('host', 'client'):
        (root / (peer + '-live.jsonl')).write_text(''.join(json.dumps(r) + '\n' for r in early))
    ok &= row(results, 'audit-08/host-ended-round-runs-to-its-end', driver.compare_round_histories(root, config)['status'] == 'PASS',
              str(driver.compare_round_histories(root, config)))
    extra = early[:2] + [dict(early[1], tick=3)] + early[2:]
    (root / 'client-live.jsonl').write_text(''.join(json.dumps(r) + '\n' for r in extra))
    ok &= row(results, 'audit-08/a-tick-past-the-hosts-end-fails', driver.compare_round_histories(root, config)['status'] == 'FAIL')
    (root / 'client-live.jsonl').write_text(''.join(json.dumps(r) + '\n' for r in early[:-1]))
    ok &= row(results, 'audit-08/a-short-last-round-fails', driver.compare_round_histories(root, config)['status'] == 'FAIL')
    for name in ('mp-held-seat', 'mp-inplace-rehold', 'world-late-join'):
        scenario = driver.load_scenario(name)
        runs = scenario.get('runs') or [scenario]
        ok &= row(results, 'audit-07/' + name + '-has-behavior-gate', all(r.get('behavior_gate') or scenario.get('behavior_gate') for r in runs))
        for run in runs:
            for peer in run.get('peers', scenario.get('peers', [])):
                if peer.get('probe'):
                    probe = json.loads(driver.scenario_text(scenario, peer['probe']))
                    ok &= row(results, 'audit-07/' + name + '-' + peer['name'] + '-native-probe-schema',
                              probe.get('schema') == 1 and 0 < probe.get('timeout_ms', 0) <= 180000
                              and 0 < len(probe.get('steps', [])) <= 256)
    behavior = scratch / 'behavior'; behavior.mkdir()
    spec = driver.load_scenario('mp-held-seat')['behavior_gate']
    (behavior / 'clientb').mkdir()
    (behavior / 'clientb/stdout.log').write_text('[net-lockstep] start round=7 frame=1 local_peer=3\n')
    for peer in spec['observers']:
        (behavior / peer).mkdir()
        (behavior / peer / 'stdout.log').write_text('[net-match] hold peer=3 frame=364 AI in control\n')
        driver.write_json(behavior / spec['reports'][peer], dict(exit_code=0, running_ticks=4201, lockstep=dict(steady_missing_frame_stalls=0)))
        probe = behavior / f'{peer}-stage/probe'; probe.mkdir(parents=True)
        state = dict(members=[dict(peer=3, state='Held - AI in control')], resyncing=False, private_catch_up=False)
        driver.write_json(probe / 'net-ui-result.json', dict(**{'pass': True}, complete=True,
            steps=[dict(observed=dict(menu_observation=json.dumps(state)))]))
    good = driver.native_behavior(behavior, spec)
    ok &= row(results, 'audit-07/native-held-seat-positive', good['status'] == 'PASS', str(good))
    path = behavior / 'host-stage/probe/net-ui-result.json'; saved_probe = path.read_text()
    probe_doc = json.loads(saved_probe)
    probe_doc['steps'][0]['observed']['menu_observation'] = json.dumps(dict(members=[dict(peer=3, state='Held - AI in control')]))
    driver.write_json(path, probe_doc)
    ok &= row(results, 'audit-07/missing-private-state-is-not-false', driver.native_behavior(behavior, spec)['status'] == 'FAIL')
    path.write_text(saved_probe)
    (behavior / 'host/stdout.log').write_text('')
    ok &= row(results, 'audit-07/absent-hold-fails', driver.native_behavior(behavior, spec)['status'] == 'FAIL')
    (behavior / 'host/stdout.log').write_text('[net-match] hold peer=3 frame=364 AI in control\n')
    driver.write_json(behavior / 'host-match.json', dict(exit_code=0, running_ticks=4201))
    ok &= row(results, 'audit-07/missing-stall-count-fails', driver.native_behavior(behavior, spec)['status'] == 'FAIL')
    driver.write_json(behavior / 'host-match.json', dict(exit_code=0, running_ticks=4201, lockstep=dict(steady_missing_frame_stalls=1)))
    ok &= row(results, 'audit-07/nonzero-stall-count-fails', driver.native_behavior(behavior, spec)['status'] == 'FAIL')
    rehold = scratch / 'rehold-native'; rehold.mkdir()
    spec = dict(kind='rehold', target='client', observers=['host', 'survivor'], holds=2,
                reports={p: p+'-report.json' for p in ('host', 'survivor')},
                ticks=dict(host=12, survivor=12), steady_peers=['host', 'survivor'])
    (rehold / 'client').mkdir()
    driver.write_json(rehold / 'client/launch.json', dict(argv=['engine', '-net-test-live-stall', '2:600', '-net-test-live-stall', '7:600']))
    (rehold / 'client/stdout.log').write_text('[net-lockstep] start round=7 frame=1 local_peer=2\n'
        '[net-test] live stall frame=2\n[net-test] live stall frame=7\n'
        '[net-match] private catch-up complete frame=6\n[net-match] private catch-up complete frame=10\n')
    for peer in spec['observers']:
        (rehold / peer).mkdir()
        (rehold / peer / 'stdout.log').write_text('[net-match] hold peer=2 frame=3 AI in control\n'
            '[net-match] seat-reclaimed peer=2 frame=6\n[net-match] hold peer=2 frame=8 AI in control\n'
            '[net-match] seat-reclaimed peer=2 frame=10\n')
        driver.write_json(rehold / spec['reports'][peer], dict(exit_code=0, running_ticks=12, steady_missing_frame_stalls=0))
        probe = rehold / f'{peer}-stage/probe'; probe.mkdir(parents=True)
        state = dict(members=[dict(peer=2, state='Playing')], resyncing=False, private_catch_up=False)
        driver.write_json(probe / 'net-ui-result.json', dict(**{'pass': True}, complete=True,
            steps=[dict(observed=dict(menu_observation=json.dumps(state)))]))
    good = driver.native_behavior(rehold, spec)
    ok &= row(results, 'audit-07/reclaimed-seat-positive', good['status'] == 'PASS', str(good))
    with (rehold / 'host/stdout.log').open('a') as stream:
        stream.write('[net-match] hold peer=2 frame=11 AI in control\n')
    ok &= row(results, 'audit-07/unscheduled-rehold-fails', driver.native_behavior(rehold, spec)['status'] == 'FAIL')
    # Run 1's lever fired at 659 for a 653 request, once the returned seat was back in play: that is its own tick, not a miss.
    late = scratch / 'rehold-late-lever'; shutil.copytree(rehold, late)
    (late / 'host/stdout.log').write_text((rehold / 'survivor/stdout.log').read_text())
    driver.write_json(late / 'client/launch.json', dict(argv=['engine', '-net-test-live-stall', '2:600', '-net-test-live-stall', '6:600']))
    ok &= row(results, 'audit-07/a-lever-firing-after-its-request-is-its-stall', driver.native_behavior(late, spec)['status'] == 'PASS',
              str(driver.native_behavior(late, spec)))
    driver.write_json(late / 'client/launch.json', dict(argv=['engine', '-net-test-live-stall', '2:600', '-net-test-live-stall', '8:600']))
    ok &= row(results, 'audit-07/a-lever-firing-before-its-request-fails', driver.native_behavior(late, spec)['status'] == 'FAIL')
    # A lobby match's report keeps its ticks in last_match and its exit code in the process record (run 1, mp-held-seat).
    lobby = scratch / 'behavior-lobby-report'; shutil.copytree(behavior, lobby)
    held = driver.load_scenario('mp-held-seat')['behavior_gate']
    for peer in held['observers']:
        driver.write_json(lobby / held['reports'][peer], dict(last_match=dict(running_ticks=4300), runner=dict(lockstep=dict(steady_missing_frame_stalls=0))))
        driver.write_json(lobby / peer / 'launch.json', dict(exit_code=0))
    ok &= row(results, 'audit-07/lobby-report-counters', driver.native_behavior(lobby, held)['status'] == 'PASS', str(driver.native_behavior(lobby, held)))
    driver.write_json(lobby / 'host/launch.json', dict(exit_code=1))
    ok &= row(results, 'audit-07/lobby-report-failed-exit', driver.native_behavior(lobby, held)['status'] == 'FAIL')
    world = scratch / 'world-native'; world.mkdir(); (world / 'world').mkdir()
    spec = dict(kind='world-continuity', ticks=dict(world=8, **{'client-first': 3, 'client-late': 3}), late_tick=6,
                reports={p:p+'-report.json' for p in ('world', 'client-first', 'client-late')})
    driver.write_json(world / 'world/launch.json', dict(argv=['-net-persistent-world','-net-world-fresh']))
    for peer, ticks in [('world',range(1,9)),('client-first',range(2,5)),('client-late',range(6,9))]:
        driver.write_json(world / spec['reports'][peer], dict(exit_code=0,running_ticks=spec['ticks'][peer]))
        (world / (peer+'-live.jsonl')).write_text(''.join(json.dumps(dict(round=7,tick=t,sim_gated=str(t),subsystems={key:str(t) for key in driver.CORE | {'controller'}}))+'\n' for t in ticks))
    good = driver.native_behavior(world, spec)
    ok &= row(results, 'audit-07/world-continuity-positive', good['status'] == 'PASS', str(good))
    path = world / 'world-live.jsonl'; data = path.read_text(); path.write_text('\n'.join(data.splitlines()[1:])+'\n')
    ok &= row(results, 'audit-07/world-coverage-gap-fails', driver.native_behavior(world, spec)['status'] == 'FAIL')
    path.write_text(data)
    # Run 1: the late joiner labelled its catch-up ticks with its round index and its live ticks with the world's round id.
    late = world / 'client-late-live.jsonl'; kept = late.read_text()
    rows = [json.loads(line) for line in kept.splitlines()]
    late.write_text(''.join(json.dumps(dict(r, round=1 if r['tick'] < 7 else 7)) + '\n' for r in rows))
    mapped = driver.native_behavior(world, spec)
    ok &= row(results, 'audit-07/world-joiner-catch-up-label-maps-to-the-round', mapped['status'] == 'PASS', str(mapped))
    late.write_text(''.join(json.dumps(dict(r, round=1 if r['tick'] == 8 else 7)) + '\n' for r in rows))
    ok &= row(results, 'audit-07/world-joiner-label-after-its-live-ticks-fails', driver.native_behavior(world, spec)['status'] == 'FAIL')
    late.write_text(''.join(json.dumps(dict(r, round=1 if r['tick'] < 7 else 7, subsystems=dict(r['subsystems'], actors='x') if r['tick'] == 6 else r['subsystems'])) + '\n' for r in rows))
    ok &= row(results, 'audit-07/world-joiner-mapped-tick-still-compared', driver.native_behavior(world, spec)['status'] == 'FAIL')
    # A world with no actors in play hashes none of them (this lane's world-late-join: ticks 1769-2321 had no actor on either side).
    world_rows_path = world / 'world-live.jsonl'; world_kept = world_rows_path.read_text()
    empty = lambda r: dict(r, subsystems={k: v for k, v in r['subsystems'].items() if k not in ('actors', 'rot_angle', 'controller')}) if r['tick'] in (6, 7) else r
    world_rows_path.write_text(''.join(json.dumps(empty(json.loads(line))) + '\n' for line in world_kept.splitlines()))
    late.write_text(''.join(json.dumps(empty(r)) + '\n' for r in rows))
    ok &= row(results, 'audit-07/world-without-actors-compares-what-it-hashes', driver.native_behavior(world, spec)['status'] == 'PASS', str(driver.native_behavior(world, spec)))
    late.write_text(''.join(json.dumps(empty(r) if r['tick'] != 8 else dict(r, subsystems={k: v for k, v in r['subsystems'].items() if k != 'actors'})) + '\n' for r in rows))
    ok &= row(results, 'audit-07/world-joiner-missing-a-hashed-subsystem-fails', driver.native_behavior(world, spec)['status'] == 'FAIL')
    world_rows_path.write_text(world_kept)
    late.write_text(kept)
    driver.write_json(world / 'world/launch.json', dict(argv=['-net-persistent-world']))
    ok &= row(results, 'audit-07/world-fresh-flag-required', driver.native_behavior(world, spec)['status'] == 'FAIL')
    review_root = scratch / 'unplanned-desync'; review_root.mkdir()
    (review_root / 'stdout.log').write_text('[lockstep] desync at frame 2 against Client\n')
    capture = dict(name='run0', peers=[dict(peer='host', root=str(review_root), video_dir=str(review_root), probe_dir='',
        index=[dict(frame=1, screen='game', sim_tick=3)], video='retained.mp4', menu_script_failures=[], record=dict(exit_code=0))])
    checked = driver.review(dict(name='plain', checklist=[dict(id='picture', screen='game')]), capture, review_root)
    ok &= row(results, 'audit-08/unplanned-desync-fails-without-fullstate', bool(checked['run_findings']))
    ok &= row(results, 'audit-07/assertionless-item-awaits-review', checked['checklist'][0]['state'] == 'AWAITING REVIEW')
    probe = review_root / 'probe'; probe.mkdir()
    driver.write_json(probe / 'net-ui-result.json', dict(**{'pass': True}, complete=True,
        script={'steps':[{'op':'finish'}]}, steps=[dict(index=0,observed={})]))
    capture['peers'][0]['probe_dir'] = str(probe)
    checked = driver.review(dict(name='plain',checklist=[dict(id='picture',screen='game')]),capture,review_root)
    ok &= row(results, 'audit-07/global-probe-success-is-not-item-proof', checked['checklist'][0]['state'] == 'AWAITING REVIEW')
    checked = driver.review(dict(name='plain',checklist=[dict(id='picture',screen='game',probe_steps=[])]),capture,review_root)
    ok &= row(results, 'audit-07/empty-step-list-is-not-item-proof', checked['checklist'][0]['state'] == 'AWAITING REVIEW')
    with patch.object(driver, 'item_evidence', return_value=([1,1], {'probe':'none'})):
        checked = driver.review(dict(name='plain', checklist=[dict(id='picture', screen='game')]), capture, review_root)
    ok &= row(results, 'audit-07/probe-none-is-a-finding', bool(checked['checklist'][0].get('finding')))
    return ok


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path)
    options = parser.parse_args()
    results = []
    if options.out:
        options.out.mkdir(parents=True, exist_ok=True)
        retained = options.out / "scratch"
        retained.mkdir()
    with contextlib.nullcontext(retained) if options.out else tempfile.TemporaryDirectory() as temporary:
        scratch = Path(temporary)
        ok = check_scenarios(results)
        ok &= check_item_screens_reachable(results, options.repo)
        ok &= check_e2e_host_end_completion(results, options.repo)
        ok &= check_capture_binary(results, scratch)
        ok &= check_scratch_limit(results, scratch)
        ok &= check_render_arm(results, scratch)
        ok &= check_module_requirements(results, scratch)
        ok &= check_rematch_contract(results)
        ok &= check_levers_inside_rounds(results)
        ok &= check_probe_clicks_seen(results)
        ok &= check_substitution(results)
        ok &= check_directory_port_block(results)
        ok &= check_launch_contract(results, scratch)
        ok &= check_index_and_checklist(results, scratch)
        ok &= check_encode(results, scratch)
        ok &= check_review(results, scratch)
        ok &= check_interruption(results, scratch)
        ok &= check_item_assertions(results, scratch)
        ok &= check_committed_window(results, scratch)
        ok &= check_resumed_play(results, scratch)
        ok &= check_listed_rows(results, scratch)
        ok &= check_frame_gaps(results, scratch)
        ok &= check_stop_request(results, scratch)
        ok &= check_finalizer(results, scratch)
        ok &= check_completion(results, scratch)
        ok &= check_resume_seams(results, scratch)
        ok &= check_menu_commands(results)
        ok &= check_drop_receipts(results, scratch)
        ok &= check_gameplay_epochs(results, scratch)
        ok &= check_cross_capture(results, scratch)
        ok &= check_cross_transfer(results, scratch)
        ok &= check_migration_timing(results, scratch)
        ok &= check_recorder_flush(results, scratch)
        ok &= check_fullstate_applicability(results, scratch)
        ok &= check_footprint_retirement(results, scratch)
        ok &= check_activity_over_applicability(results, scratch)
        ok &= check_injected_exemption(results, scratch)
        ok &= check_log_gates(results, scratch)
        ok &= check_acceptance_rows(results, scratch)
    summary = {"schema": 1, "pass": bool(ok), "rows": results,
               "needs_a_real_capture": ["the engine's -record-video output itself",
                                        "ffmpeg encode of a real frame sequence",
                                        "the contact sheet's burned-in labels",
                                        "every scenario's menu script and probe against a running engine"]}
    if options.out:
        options.out.mkdir(parents=True, exist_ok=True)
        (options.out / "e2e-video-selftest.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"[e2e-video] {'PASS' if ok else 'FAIL'} {sum(1 for r in results if r['pass'])}/{len(results)} rows")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
