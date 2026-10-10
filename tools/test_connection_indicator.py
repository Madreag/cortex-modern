"""Connection pictures through the existing menu gestures, scene probes and hidden runner.

The four-peer pictures require four explicitly assigned boxes. --single-engine runs
preferences and the single-player guard without inventing multiplayer evidence.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

from PIL import Image, ImageChops

from run_sim_test import make_run, seed_settings

SIZES = ("640x360", "960x540", "1280x720", "1920x1080")
BASE = "107dbdab7c7d780d40f6be4fb8f27e49db9e7a3a"
STATES = {
    "Good": (1, 250, "250 ms\nPresses arrive on time", "250 ms / Good\nPresses arrive on time", [105, 210, 120]),
    "Marginal": (2, 80, "80 ms / Unsteady\nPresses still arrive", "80 ms / Unsteady\nPresses still arrive", [245, 185, 70]),
    "Substituting": (3, 80, "80 ms / Inputs affected\nPresses may not register now", "80 ms / Inputs affected\nPresses may not register now", [245, 100, 100]),
    "Lost": (4, 0, "reconnecting\nThe AI plays your units", "-- ms / Reconnecting\nThe AI plays your units", [175, 180, 190]),
}
LEVER = "1:Good:250;2:Marginal:80;3:Substituting:80;4:Lost:0"
ON_HINT = "Shows how your presses arrive in a match."
OFF_HINT = "Hidden on your HUD; Seats still shows connections."
GUARD_INDEX = (
    "DataModule\n\tModuleName = User Scenes\n\tScanFolderContents = 1\n\tIgnoreMissingItems = 1\n"
    "\tAddActivity = GAScripted\n\t\tPresetName = Determinism ConnectionGuard\n"
    "\t\tSceneName = Grasslands\n\t\tScriptPath = UserScenes.rte/ConnectionGuard.lua\n"
    "\t\tLuaClassName = ConnectionGuard\n\t\tMinTeamsRequired = 1\n\t\tIsTestActivity = 1\n"
    "\t\tDefaultRequireClearPathToOrbit = 0\n\t\tDefaultFogOfWar = 0\n\t\tDefaultDeployUnits = 0\n"
)


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def menu(command):
    return {"op": "menu", "command": command}


def exact(control, text, *, scope=None, inside=None, rgb=None, clear=()):
    step = {"op": "assert_control", "control": control, "equals": {"visible": True, "text": text}, "fits": True, "unwrapped": not control.startswith("Combo")}
    if scope:
        step["scope"] = scope
    if inside:
        step["inside"] = inside
    if rgb:
        step["ink_rgb"] = rgb
    if clear:
        step["clear_of"] = list(clear)
    if control == "LabelOwnConnection":
        step["hud_area"] = True
    return step


def settings_steps(initial, pick=None):
    steps = [
        {"op": "wait", "screen": "MainScreen", "scope": "menu", "elapsed_ms": 800},
        menu("activate ButtonMainToOptions"),
        {"op": "wait", "screen": "SettingsScreen", "scope": "menu", "renders": 4},
        menu("activate TabNetworkSettings"), {"op": "wait", "scope": "menu", "renders": 4}, menu("assert_settings_page Network:Basics"),
        exact("LabelNetworkConnectionIndicator", "Connection indicator", scope="menu"),
        exact("ComboNetworkConnectionIndicator", "On" if initial else "Off", scope="menu"),
        exact("LabelNetworkConnectionIndicatorHint", ON_HINT if initial else OFF_HINT, scope="menu"),
    ]
    if pick is not None:
        steps += [menu("combo_select ComboNetworkConnectionIndicator " + ("On" if pick else "Off")),
                  {"op": "wait", "scope": "menu", "renders": 4},
                  exact("ComboNetworkConnectionIndicator", "On" if pick else "Off", scope="menu"),
                  exact("LabelNetworkConnectionIndicatorHint", ON_HINT if pick else OFF_HINT, scope="menu")]
    steps += [{"op": "screenshot_pair", "name": "toggle", "scope": "menu"},
              {"op": "signal", "name": "done", "scope": "menu"}, {"op": "finish"}]
    return steps


def run_single(repo, root, steps, size, *, runtime=None, exe=None, guard=False):
    root.mkdir(parents=True, exist_ok=False)
    probe = root / "probe/probe.json"
    write_json(probe, {"schema": 1, "timeout_ms": 25000, "steps": steps})
    script = root / "hold.menu.txt"
    # The bounded hold also lets a baseline's intentional missing-control failure exit.
    script.write_text("wait_ms 14000\nexit\n", encoding="utf-8")
    args = ["-menu-script", str(script), "-menu-script-out", str(root / "menu.json")]
    if guard:
        args += ["-scenario", "ConnectionGuard", "-seed", "42", "-max-ticks", "120", "-out", str(root / "trace.json")]
    run = make_run(repo, args, root / "engine", 40, runtime=runtime,
                   env={"CCCP_HEADLESS": "1", "CC_TEST_NET_UI_SCRIPT": str(probe), "CC_TEST_LINK_QUALITY": LEVER})
    if exe:
        run.argv[0] = str(exe.resolve())
    width, height = (int(part) for part in size.split("x"))
    seed_settings(run, {"ResolutionX": width, "ResolutionY": height, "ResolutionMultiplier": "1", "ShowAdvancedPerfStats": "0"})
    if guard:
        module = Path(run.cwd) / "Userdata/UserScenes.rte"
        module.mkdir(exist_ok=True)
        (module / "Index.ini").write_text(GUARD_INDEX, encoding="utf-8")
        (module / "ConnectionGuard.lua").write_bytes((Path(__file__).parent / "fixtures/ConnectionGuard.lua").read_bytes())
    try:
        record = run.start().finish()
    finally:
        run.close()
    result_path = probe.parent / "net-ui-result.json"
    observed = json.loads(result_path.read_text(encoding="utf-8")) if result_path.is_file() else {}
    result = {"pass": record.get("exit_code") == 0 and not record.get("timed_out") and observed.get("pass") is True and observed.get("complete") is True,
              "record": record, "probe": observed, "runtime": str(run.cwd)}
    write_json(root / "result.json", result)
    return result


def failure_line(result):
    return result.get("probe", {}).get("reason", result.get("probe", {}).get("error", "probe incomplete"))


def guard_steps():
    return [{"op": "wait", "paused": True, "sim_at_least": 1},
            {"op": "mouse_move", "x": 8, "y": 8}, {"op": "wait", "renders": 4},
            {"op": "assert", "equals": {"service": "Idle"}, "connections_absent": True},
            {"op": "screenshot_pair", "name": "guard"}, {"op": "signal", "name": "done"}, {"op": "finish"}]


def compare_guard(before, after, size):
    first = next(iter(Path(before).glob("engine/runtime/ScreenShots/guard_composited*.png")), None)
    second = next(iter(Path(after).glob("engine/runtime/ScreenShots/guard_composited*.png")), None)
    if first is None or second is None:
        return {"pass": False, "reason": "whole-frame capture missing"}
    with Image.open(first) as a, Image.open(second) as b:
        a, b = a.convert("RGB"), b.convert("RGB")
        expected = tuple(int(part) for part in size.split("x"))
        if a.size != expected or b.size != expected:
            return {"pass": False, "reason": "whole-frame dimensions differ"}
        changed = sum(pixel != (0, 0, 0) for pixel in ImageChops.difference(a, b).getdata())
        return {"pass": changed == 0, "changed_pixels": changed, "before_rgb_sha256": hashlib.sha256(a.tobytes()).hexdigest(),
                "after_rgb_sha256": hashlib.sha256(b.tobytes()).hexdigest()}


def panel_steps(first):
    steps = [{"op": "key_down", "key": "F6"}, {"op": "wait", "renders": 2}, {"op": "key_up", "key": "F6"},
             {"op": "wait", "panel_open": True, "renders": 4}]
    ordered = [first] + [state for state in STATES if state != first]
    for state in ordered:
        peer, _, _, text, rgb = STATES[state]
        others = ["NetworkSeatLink" + str(row[0]) for other, row in STATES.items() if other != state]
        steps.append(exact("NetworkSeatLink" + str(peer), text, inside="NetworkSeats", rgb=rgb, clear=others + ["NetworkSeatsRoster", "NetworkSeatsSummary"]))
    return steps


def match_steps(surface, state, indicator=True):
    steps = [{"op": "wait", "service": "Running", "screen": "Gameplay", "sim_at_least": 1, "paused": False, "renders": 4}]
    if surface == "HUD":
        steps.append(exact("LabelOwnConnection", STATES[state][2], inside="BoxOwnConnection", rgb=STATES[state][4], clear=["BoxNetMatchStatus"]))
    elif surface == "toggle":
        if indicator:
            steps.append(exact("LabelOwnConnection", STATES["Good"][2], inside="BoxOwnConnection", rgb=STATES["Good"][4]))
        else:
            steps.append({"op": "assert_control", "control": "LabelOwnConnection", "equals": {"visible": False}})
        steps += panel_steps("Good")
    elif surface == "Seats":
        steps += panel_steps(state)
    steps += [{"op": "screenshot_pair", "name": "connection"}, {"op": "signal", "name": "done"}, {"op": "finish"}]
    return steps


def scene_document():
    scripts, runs, checklist = {}, [], []
    common = ["-net-match-service-e2e", "-net-port", "{PORT}", "-net-match-peers", "4", "-net-match-humans", "4",
              "-net-match-cpu-slots", "0", "-net-match-ticks", "900", "-net-autosave-seconds", "0", "-seed", "42",
              "-net-match-report", "{OUT}/{PEER}-match.json", "-out", "{OUT}/{PEER}-trace.json"]

    def add_match(name, size, surface, state, indicator=True, retained=None):
        peers = []
        for index, who in enumerate(("host", "seat2", "seat3", "seat4")):
            key = name + "-" + who
            steps = match_steps(surface, state, indicator) if who == "host" or surface in ("Seats", "toggle") else [
                {"op": "wait", "service": "Running", "sim_at_least": 1}, {"op": "signal", "name": "done"}, {"op": "finish"}]
            if who != "host" and surface == "toggle":
                steps = match_steps("Seats", "Good")
            steps.insert(1, menu("video_mark " + name))
            scripts[key + ".probe.json"] = json.dumps({"schema": 1, "timeout_ms": 90000, "steps": steps})
            lever = LEVER if surface != "HUD" else "1:" + state + ":" + str(STATES[state][1]) + ";2:Good:40;3:Good:40;4:Good:40"
            peer = {"name": who, "args": common + (["-net-host"] if index == 0 else ["-net-join", "{HOST_ADDRESS}"]),
                    "probe": key + ".probe.json", "env": {"CC_TEST_LINK_QUALITY": lever},
                    "settings": {"NetworkMatchStatusMode": "Auto", "NetworkShowDiagnostics": "0", "NetworkChatVisible": "0", "NetworkToastsEnabled": "0"}}
            if retained and index == 0:
                peer["retained_runtime"] = retained
            else:
                peer["settings"]["NetworkConnectionIndicator"] = "1" if indicator else "0"
            peers.append(peer)
        runs.append({"name": name, "size": size, "peers": peers})
        checklist.append({"id": name, "run": name, "peer": "host", "mark": name, "screen": "game", "what": "Exact connection labels, pixel colours and containment", "assert": "completed native picture probe"})

    for size in SIZES:
        for surface in ("HUD", "Seats"):
            for state in STATES:
                add_match(surface + "_" + state + "_" + size, size, surface, state, surface == "HUD")
    previous = None
    for name, initial, pick in (("toggle-off", True, False), ("toggle-on", False, True), ("toggle-on-relaunch", True, None)):
        key = name + ".probe.json"
        scripts[key] = json.dumps({"schema": 1, "timeout_ms": 25000, "steps": settings_steps(initial, pick)})
        scripts[name + ".menu.txt"] = "wait_file {PROBE_DIR}/done.json 30\nexit\n"
        peer = {"name": "host", "args": [], "probe": key, "menu_script": name + ".menu.txt"}
        if previous:
            peer["retained_runtime"] = previous
        runs.append({"name": name, "size": "640x360", "peers": [peer]})
        previous = {"run": name, "peer": "host"}
        if name == "toggle-off":
            add_match("toggle-off-match", "640x360", "toggle", "Good", False, previous)
            previous = {"run": "toggle-off-match", "peer": "host"}
    add_match("toggle-on-match", "640x360", "toggle", "Good", True, previous)
    return {"schema": 1, "name": "connection-indicator", "title": "Connection feedback follows presses at every supported size",
            "timeout_s": 120, "scripts": scripts, "runs": runs, "checklist": checklist}


def run_multiplayer(options, out, repo):
    scenario = out / "connection-indicator.json"
    write_json(scenario, scene_document())
    command = [sys.executable, str(Path(__file__).parent / "e2e_video.py"), "--repo", str(repo), "--out", str(out / "capture"),
               "--scenario", str(scenario.with_suffix("")), "--peer-boxes", options.peer_boxes, "--fps", "1", "--capture-peer", "host",
               "--token", "HOST_ADDRESS=" + options.host_address, "--port", str(options.port), "--port-block", str(options.port) + "-" + str(options.port + 79)]
    if options.pool_registry:
        command += ["--pool-registry", str(options.pool_registry)]
    code = subprocess.run(command, check=False).returncode
    capture_path = out / "capture/capture.json"
    capture = json.loads(capture_path.read_text(encoding="utf-8")) if capture_path.is_file() else {}
    cases = {}
    baseline = repo == options.baseline_repo
    for run in scene_document()["runs"]:
        name = run["name"]
        paths = list((out / "capture" / name / "host").rglob("net-ui-result.json"))
        native = json.loads(paths[0].read_text(encoding="utf-8")) if len(paths) == 1 else {}
        if baseline and name.startswith(("HUD_", "Seats_")):
            surface, state, size = name.split("_")
            expected = "LabelOwnConnection" if surface == "HUD" else "NetworkSeatLink" + str(STATES[state][0])
            reason = native.get("error", "probe missing")
            red = native.get("pass") is False and "unknown control: " + expected in reason and native.get("failed_observation", {}).get("service") == "Running"
            line = ("RED " if red else "FAIL ") + name + " " + reason
            cases[name] = {"pass": red, "line": line}
        elif baseline:
            continue
        else:
            peers = list((out / "capture" / name).rglob("net-ui-result.json"))
            native_pass = len(peers) == len(run["peers"]) and all(
                json.loads(path.read_text(encoding="utf-8")).get("pass") is True and json.loads(path.read_text(encoding="utf-8")).get("complete") is True for path in peers)
            png_checks = []
            for path in peers:
                peer_result = json.loads(path.read_text(encoding="utf-8"))
                image_path = path.parent / "connection.png"
                color_steps = [row for row in peer_result.get("steps", []) if row["op"] == "assert_control" and row["observed"].get("control", {}).get("ink_pixels")]
                if not color_steps:
                    continue
                if not image_path.is_file():
                    png_checks.append(False)
                    continue
                with Image.open(image_path) as frame:
                    frame = frame.convert("RGB")
                    for row in color_steps:
                        script = peer_result["script"]["steps"][row["index"]]
                        x, y, width, height = row["observed"]["control"]["rect"]
                        pixels = frame.crop((x, y, x + width, y + height)).getdata()
                        png_checks.append(sum(pixel == tuple(script["ink_rgb"]) for pixel in pixels) >= 3)
            passed = native_pass and native.get("complete") is True and all(png_checks)
            if name.startswith(("HUD_", "Seats_")) or name.endswith("-match"):
                passed &= bool(png_checks)
            cases[name] = {"pass": passed, "line": ("GREEN " if passed else "FAIL ") + name,
                           "saved_frame_colour_checks": len(png_checks)}
        print(cases[name]["line"], flush=True)
    scored = {"cases": cases, "driver_exit_code": code, "capture": capture.get("name"), "pass": bool(cases) and all(row["pass"] for row in cases.values())}
    write_json(out / "picture-results.json", scored)
    return scored


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--baseline-repo", type=Path)
    parser.add_argument("--baseline-exe", type=Path)
    parser.add_argument("--single-engine", action="store_true")
    parser.add_argument("--peer-boxes")
    parser.add_argument("--pool-registry", type=Path)
    parser.add_argument("--host-address")
    parser.add_argument("--port", type=int, default=49840)
    options = parser.parse_args()
    if not options.single_engine and (not options.peer_boxes or not options.host_address):
        parser.error("four-seat pictures need --peer-boxes and --host-address assigned by the lead")
    if options.baseline_repo:
        sha = subprocess.check_output(["git", "-C", str(options.baseline_repo), "rev-parse", "HEAD"], text=True).strip()
        if sha != BASE:
            parser.error("baseline checkout must be " + BASE)
    options.out.mkdir(parents=True, exist_ok=False)
    result = {"base": BASE, "cases": {}, "needs_testing": [], "pass": False}
    if options.baseline_repo:
        before = run_single(options.baseline_repo, options.out / "RED-toggle", settings_steps(True, False), "640x360", exe=options.baseline_exe)
        red = not before["pass"] and "LabelNetworkConnectionIndicator" in failure_line(before)
        result["cases"]["toggle_RED"] = {"pass": red, "line": failure_line(before)}
        print(("RED" if red else "FAIL") + " toggle 640x360 " + failure_line(before), flush=True)
    else:
        result["needs_testing"].append("exact baseline executable and checkout for RED and whole-frame guards")
    runtime = None
    for name, initial, pick in (("default-on", True, False), ("off-relaunch", False, True), ("on-relaunch", True, None)):
        current = run_single(options.repo, options.out / name, settings_steps(initial, pick), "640x360", runtime=runtime)
        result["cases"][name] = {"pass": current["pass"], "line": "GREEN " + name if current["pass"] else failure_line(current)}
        runtime = Path(current["runtime"])
        print(result["cases"][name]["line"], flush=True)
    for size in SIZES:
        after_root = options.out / ("guard-tip-" + size)
        after = run_single(options.repo, after_root, guard_steps(), size, guard=True)
        guard = {"pass": after["pass"], "compared": False}
        if options.baseline_repo:
            before_root = options.out / ("guard-base-" + size)
            before = run_single(options.baseline_repo, before_root, guard_steps(), size, exe=options.baseline_exe, guard=True)
            guard = {**compare_guard(before_root, after_root, size), "compared": True, "base_probe_pass": before["pass"], "tip_probe_pass": after["pass"]}
            guard["pass"] &= before["pass"] and after["pass"]
        result["cases"]["guard_" + size] = guard
        print(("GREEN" if guard["pass"] and guard["compared"] else "CAPTURED" if guard["pass"] else "FAIL") + " guard " + size + " " + json.dumps(guard), flush=True)
    if options.single_engine:
        result["needs_testing"].append("four-peer HUD, Seats and toggle visibility pictures on explicitly assigned boxes")
        for size in SIZES:
            for surface in ("HUD", "Seats"):
                for state in STATES:
                    result["cases"][surface + "_" + state + "_" + size] = {"pass": False, "status": "NEEDS TESTING"}
        write_json(options.out / "connection-indicator.json", scene_document())
    else:
        tip = run_multiplayer(options, options.out / "tip", options.repo)
        result["cases"].update(tip["cases"])
        result["cases"]["scene_driver_tip"] = {"pass": tip["driver_exit_code"] == 0}
        if options.baseline_repo:
            base = run_multiplayer(options, options.out / "base", options.baseline_repo)
            result["cases"].update({"RED_" + name: row for name, row in base["cases"].items()})
    result["pass"] = not result["needs_testing"] and all(row.get("pass", False) for row in result["cases"].values())
    write_json(options.out / "result.json", result)
    print(json.dumps({"pass": result["pass"], "needs_testing": result["needs_testing"]}), flush=True)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
