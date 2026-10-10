"""Connection pictures through the existing menu gestures, scene probes and hidden runner.

Four separately assigned boxes provide proof. --layout-detector launches four
local peers for layout only; --single-engine runs preferences and the pixel guard.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import threading

from PIL import Image, ImageChops

from run_sim_test import make_run, seed_settings

SIZES = ("640x360", "960x540", "1280x720", "1920x1080")
BASE = "107dbdab7c7d780d40f6be4fb8f27e49db9e7a3a"
STATES = {
    "Good": (1, 250, "250 ms", "250 ms / Good\nPresses arrive on time", [105, 210, 120]),
    "Marginal": (2, 80, "80 ms / Unsteady\nPresses still arrive", "80 ms / Unsteady\nPresses still arrive", [245, 185, 70]),
    "Substituting": (3, 80, "80 ms / Inputs affected\nPresses may not register now", "80 ms / Inputs affected\nPresses may not register now", [245, 100, 100]),
    "Lost": (4, 0, "reconnecting\nThe AI plays your units", "-- ms / Reconnecting\nThe AI plays your units", [175, 180, 190]),
}
LEVER = "1:Good:250;2:Marginal:80;3:Substituting:80;4:Lost:0"
SEAT_NAMES = ["Seat1", "Seat2", "Seat3", "Seat4"]
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


def saved_indicator(runtime):
    values = re.findall(r"(?m)^\s*NetworkConnectionIndicator\s*=\s*(\S+)\s*$", (Path(runtime) / "Userdata/Settings.ini").read_text(encoding="utf-8-sig"))
    return values[0] in ("1", "true") if len(values) == 1 and values[0] in ("0", "1", "false", "true") else None


def recording_environment(registry):
    from e2e_video import box_name, declared_tool_dirs, find_ffmpeg
    import box_facts

    environment = dict(os.environ)
    prefixes = [str(path) for path in declared_tool_dirs()]
    catalogs = [registry or environment.get("CORTEX_POOL_REGISTRY"), box_facts.file()]
    for catalog in catalogs:
        if not catalog or not Path(catalog).is_file():
            continue
        boxes = json.loads(Path(catalog).read_text(encoding="utf-8-sig")).get("boxes", [])
        for box in boxes:
            names = [box.get(key) for key in ("name", "hostname", "computer_name", "ssh", "instance")] + list(box.get("aliases") or [])
            if box_name().casefold() not in {str(name).casefold() for name in names if name}:
                continue
            for entry in [box, box.get("runner") or {}, *(box.get("task_slots") or [])]:
                prefixes.extend(entry.get("path_prepend") or [])
    prefixes = list(dict.fromkeys(prefixes))
    environment["PATH"] = os.pathsep.join(prefixes + [environment.get("PATH", "")])
    wanted = environment.get("CCCP_FFMPEG") or "ffmpeg"
    encoder = shutil.which(wanted, path=environment["PATH"])
    if not encoder and not environment.get("CCCP_FFMPEG"):
        encoder = find_ffmpeg()
    if not encoder:
        raise ValueError("ffmpeg is absent on " + box_name() + "; runner PATH prefix: " +
                         (os.pathsep.join(prefixes) or "<undeclared; set path_prepend in the box registry>") + "; registry: " + str(catalogs[0]))
    environment["CCCP_FFMPEG"] = encoder
    print("[connection-pictures] ffmpeg " + encoder + "; runner PATH prefix: " + os.pathsep.join(prefixes), flush=True)
    return environment


def run_single(repo, root, steps, size, *, runtime=None, exe=None, guard=False, timeout=240, expected_saved=None):
    root = Path(root).resolve()
    root.mkdir(parents=True, exist_ok=False)
    probe = root / "probe/probe.json"
    write_json(probe, {"schema": 1, "timeout_ms": 90000, "steps": steps})
    script = root / "hold.menu.txt"
    # Cold module loads finish before the probe's framed input and saved-file checks.
    script.write_text(f"wait_file {probe.parent.as_posix()}/done.json {timeout - 10}\nwait_ms 250\nexit\n", encoding="utf-8")
    args = ["-menu-script", str(script), "-menu-script-out", str(root / "menu.json")]
    if guard:
        args += ["-scenario", "ConnectionGuard", "-seed", "42", "-max-ticks", "120", "-out", str(root / "trace.json")]
    run = make_run(repo, args, root / "engine", timeout, runtime=runtime,
                   env={"CCCP_HEADLESS": "1", "CC_TEST_NET_UI_SCRIPT": str(probe), "CC_TEST_LINK_QUALITY": LEVER})
    settings_path = Path(run.cwd) / "Userdata/Settings.ini"
    run.env["CCCP_SETTINGSPATH"] = str(settings_path.resolve())
    if runtime is None and not guard:
        text = settings_path.read_text(encoding="utf-8-sig")
        settings_path.write_text(re.sub(r"(?m)^[ \t]*NetworkConnectionIndicator\s*=[^\r\n]*\r?\n?", "", text), encoding="utf-8")
    if exe:
        run.argv[0] = str(exe.resolve())
    width, height = (int(part) for part in size.split("x"))
    seed_settings(run, {"ResolutionX": width, "ResolutionY": height, "ResolutionMultiplier": "1", "ShowAdvancedPerfStats": "0"})
    if guard:
        seed_settings(run, {"ShowPerformanceStats": "0"})
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
              "record": record, "probe": observed, "runtime": str(run.cwd), "settings_path": str(settings_path)}
    if guard:
        ready = Path(run.cwd) / "Userdata/UserScenes.rte/connection-guard-ready.json"
        result["guard_ready"] = json.loads(ready.read_text(encoding="utf-8")) if ready.is_file() else {}
        result["pass"] &= result["guard_ready"] == {"activity_preset": "ConnectionGuard", "tick": 4}
        if result["guard_ready"] != {"activity_preset": "ConnectionGuard", "tick": 4}:
            result["guard_error"] = "running activity identity differs: " + json.dumps(result["guard_ready"])
    if expected_saved is not None:
        result["saved_indicator"] = saved_indicator(run.cwd)
        result["pass"] &= result["saved_indicator"] is expected_saved
        if result["saved_indicator"] is not expected_saved:
            result["persistence_error"] = f"saved NetworkConnectionIndicator differs: expected {expected_saved}, observed {result['saved_indicator']}"
    write_json(root / "result.json", result)
    return result


def failure_line(result):
    return result.get("persistence_error") or result.get("guard_error") or result.get("probe", {}).get("reason", result.get("probe", {}).get("error", "probe incomplete"))


def guard_steps():
    return [{"op": "wait_file", "path": "Userdata/UserScenes.rte/connection-guard-ready.json"},
            {"op": "wait", "paused": False, "sim_at_least": 4},
            {"op": "wait", "renders": 4},
            {"op": "assert", "equals": {"service": "Idle", "screen": "Gameplay", "paused": False, "sim_frame": 4, "local_actor_alive": True}, "connections_absent": True},
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


def panel_steps(first, held=False):
    steps = [{"op": "key_down", "key": "F6"}, {"op": "wait", "renders": 2}, {"op": "key_up", "key": "F6"},
             {"op": "wait", "panel_open": True, "renders": 4}]
    ordered = [] if held else [first] + [state for state in STATES if state != first]
    for state in ordered:
        peer, _, _, text, rgb = STATES[state]
        others = ["NetworkSeatLink" + str(row[0]) for other, row in STATES.items() if other != state]
        steps.append(exact("NetworkSeatLink" + str(peer), text, inside="NetworkSeats", rgb=rgb, clear=others + ["NetworkSeatsRoster", "NetworkSeatsSummary"]))
    if held:
        step = exact("NetworkSeatLink", STATES["Lost"][3], inside="NetworkSeats", rgb=STATES["Lost"][4], clear=["NetworkSeatsRoster", "NetworkSeatsSummary"])
        step["seat_name"] = "Seat4"
        steps.append(step)
    names = {"op": "assert", "equals": {"panel_open": True}, "no_duplicate_name": True, "names": SEAT_NAMES}
    if held:
        names.update(held_peer="Seat4", held_name="Seat4")
    steps.append(names)
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
    elif surface == "held":
        steps += [{"op": "wait", "service": "Running", "held_peer": "Seat4"}]
        steps += panel_steps("Lost", held=True)
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
            observes = who == "host" or surface in ("Seats", "toggle") or (surface == "held" and who != "seat4")
            steps = match_steps(surface, state, indicator) if observes else [
                {"op": "wait", "service": "Running", "sim_at_least": 1}, {"op": "signal", "name": "done"}, {"op": "finish"}]
            if who != "host" and surface == "toggle":
                steps = match_steps("Seats", "Good")
            if surface == "held" and who == "seat4":
                steps = [{"op": "wait", "service": "Running", "lockstep_frame_at_least": 120},
                         {"op": "signal", "name": "done"}, {"op": "finish"}]
            steps.insert(1, menu("video_mark " + name))
            scripts[key + ".probe.json"] = json.dumps({"schema": 1, "timeout_ms": 90000, "steps": steps})
            lever = LEVER if surface != "HUD" else "1:" + state + ":" + str(STATES[state][1]) + ";2:Good:40;3:Good:40;4:Good:40"
            if surface == "held":
                lever = "1:Good:250;2:Lost:0;3:Lost:0;4:Lost:0"
            peer = {"name": who, "args": common + ["-net-player-name", SEAT_NAMES[index]] + (["-net-host"] if index == 0 else ["-net-join", "{HOST_ADDRESS}"]),
                    "probe": key + ".probe.json", "env": {"CC_TEST_LINK_QUALITY": lever, "CCCP_SETTINGSPATH": "Userdata/Settings.ini"},
                    "settings": {"NetworkDisplayName": SEAT_NAMES[index], "NetworkMatchStatusMode": "Auto", "NetworkShowDiagnostics": "0", "NetworkChatVisible": "0", "NetworkToastsEnabled": "0"}}
            if surface == "held" and who == "seat4":
                peer["kill_when"] = {"peer": "seat4", "probe_complete": True}
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
        add_match("Seats_held_" + size, size, "held", "Lost", False)
    previous = None
    for name, initial, pick in (("toggle-off", True, False), ("toggle-on", False, True), ("toggle-on-relaunch", True, None)):
        key = name + ".probe.json"
        scripts[key] = json.dumps({"schema": 1, "timeout_ms": 90000, "steps": settings_steps(initial, pick)})
        scripts[name + ".menu.txt"] = "wait_file {PROBE_DIR}/done.json 120\nwait_ms 250\nexit\n"
        peer = {"name": "host", "args": [], "probe": key, "menu_script": name + ".menu.txt", "env": {"CCCP_SETTINGSPATH": "Userdata/Settings.ini"}}
        if previous:
            peer["retained_runtime"] = previous
        runs.append({"name": name, "size": "640x360", "peers": [peer]})
        previous = {"run": name, "peer": "host"}
        if name == "toggle-off":
            add_match("toggle-off-match", "640x360", "toggle", "Good", False, previous)
            previous = {"run": "toggle-off-match", "peer": "host"}
    add_match("toggle-on-match", "640x360", "toggle", "Good", True, previous)
    return {"schema": 1, "name": "connection-indicator", "title": "Connection feedback follows presses at every supported size",
            "timeout_s": 240, "scripts": scripts, "runs": runs, "checklist": checklist}


def frame_colours(native, path, size):
    rows = [row for row in native.get("steps", []) if row["op"] == "assert_control" and row["observed"].get("control", {}).get("ink_pixels")]
    if not rows:
        return []
    if not path.is_file():
        return [False]
    with Image.open(path) as frame:
        frame = frame.convert("RGB")
        if frame.size != tuple(int(part) for part in size.split("x")):
            return [False]
        checks = []
        for row in rows:
            step = native["script"]["steps"][row["index"]]
            x, y, width, height = row["observed"]["control"]["rect"]
            pixels = frame.crop((x, y, x + width, y + height)).getdata()
            checks.append(sum(pixel == tuple(step["ink_rgb"]) for pixel in pixels) >= 3)
        return checks


class SeatDrop:
    def __init__(self, run, marker, timeout):
        import ctypes as C
        from ctypes import wintypes as W

        if sys.platform != "win32":
            raise ValueError("the local held-seat detector needs the Windows hidden runner")
        kernel = C.WinDLL("kernel32", use_last_error=True)
        kernel.FindFirstChangeNotificationW.argtypes = [W.LPCWSTR, W.BOOL, W.DWORD]
        kernel.FindFirstChangeNotificationW.restype = W.HANDLE
        kernel.FindNextChangeNotification.argtypes = [W.HANDLE]
        kernel.FindCloseChangeNotification.argtypes = [W.HANDLE]
        kernel.CreateEventW.argtypes = [C.c_void_p, W.BOOL, W.BOOL, W.LPCWSTR]
        kernel.CreateEventW.restype = W.HANDLE
        kernel.SetEvent.argtypes = [W.HANDLE]
        kernel.CloseHandle.argtypes = [W.HANDLE]
        kernel.WaitForMultipleObjects.argtypes = [W.DWORD, C.POINTER(W.HANDLE), W.BOOL, W.DWORD]
        kernel.WaitForMultipleObjects.restype = W.DWORD
        changed = kernel.FindFirstChangeNotificationW(str(marker.parent), False, 0x10)
        if changed in (None, C.c_void_p(-1).value):
            raise C.WinError(C.get_last_error())
        cancelled = kernel.CreateEventW(None, True, False, None)
        if not cancelled:
            kernel.FindCloseChangeNotification(changed)
            raise C.WinError(C.get_last_error())
        handles = (W.HANDLE * 2)(cancelled, changed)
        self.evidence = {"requested_frame": 120, "dropped": False}

        def watch():
            try:
                # File-change notifications wait for the probe's signal without polling the engine.
                while True:
                    if marker.is_file():
                        try:
                            observed = json.loads(marker.read_text(encoding="utf-8"))
                        except json.JSONDecodeError:
                            pass
                        else:
                            break
                    event = kernel.WaitForMultipleObjects(2, handles, False, int(timeout * 1000))
                    if event == 0:
                        return
                    if event != 1:
                        raise RuntimeError("held-seat probe signal did not arrive")
                    if not kernel.FindNextChangeNotification(changed):
                        raise C.WinError(C.get_last_error())
                if observed.get("service") != "Running" or observed.get("lockstep_frame", 0) < 120:
                    raise ValueError("held-seat drop signal is outside recorded gameplay")
                run.terminate(0, "held-seat fixture after recorded gameplay frame 120")
                self.evidence.update(dropped=True, observed=observed)
            except (OSError, RuntimeError, ValueError) as error:
                self.evidence["error"] = str(error)

        self.thread = threading.Thread(target=watch, name="connection-seat-drop")
        self.close_handles = lambda: (kernel.SetEvent(cancelled), self.thread.join(), kernel.FindCloseChangeNotification(changed), kernel.CloseHandle(cancelled))
        self.thread.start()

    def close(self):
        self.close_handles()


def local_case(options, repo, root, definition, scripts, port, retained):
    root.mkdir(parents=True, exist_ok=False)
    probe = root / "host-probe/probe.json"
    document = json.loads(scripts[definition["peers"][0]["probe"]])
    document["steps"] = [step for step in document["steps"] if not step.get("command", "").startswith("video_mark ")]
    reference = next((index for index, step in enumerate(document["steps"]) if step.get("control", "").startswith(("LabelOwnConnection", "NetworkSeatLink"))), None)
    if reference is not None:
        document["steps"].insert(reference, {"op": "screenshot_pair", "name": "reference"})
    drop_marker = root / "seat4-probe/done.json"
    held = definition["name"].startswith("Seats_held_")
    if held:
        document["steps"].insert(1, {"op": "wait_file", "path": str(drop_marker)})
        if repo == options.baseline_repo:
            document["steps"].insert(2, {"op": "wait", "elapsed_ms": 6000})
    write_json(probe, document)
    runs, records, runtimes = [], {}, {}
    error, drop = None, None
    width, height = (int(part) for part in definition["size"].split("x"))
    try:
        for peer in definition["peers"]:
            who = peer["name"]
            tokens = {"{PORT}": str(port), "{HOST_ADDRESS}": "127.0.0.1", "{OUT}": root.as_posix(), "{PEER}": who}
            args = list(peer["args"])
            for token, value in tokens.items():
                args = [argument.replace(token, value) for argument in args]
            if who == "host":
                menu_path = root / "host.menu.txt"
                menu_path.write_text(f"wait_file {(probe.parent / 'done.json').as_posix()} {options.timeout - 10}\nwait_ms 250\nexit\n", encoding="utf-8")
                args += ["-menu-script", str(menu_path), "-menu-script-out", str(root / "host-menu.json")]
            environment = {**peer.get("env", {}), "CCCP_HEADLESS": "1", "CC_TEST_NET_MATCH_E2E_WAIT_PEERS": "1",
                           "CC_TEST_NET_UI_SCRIPT": str(probe) if who == "host" else ""}
            if held and who == "seat4":
                peer_probe = drop_marker.parent / "probe.json"
                peer_document = json.loads(scripts[peer["probe"]])
                peer_document["steps"] = [step for step in peer_document["steps"] if not step.get("command", "").startswith("video_mark ")]
                write_json(peer_probe, peer_document)
                environment["CC_TEST_NET_UI_SCRIPT"] = str(peer_probe)
            previous = peer.get("retained_runtime")
            runtime = retained[(previous["run"], previous["peer"])] if previous else None
            run = make_run(repo, args, root / who, options.timeout, runtime=runtime, env=environment)
            runs.append((who, run))
            if repo == options.baseline_repo and options.baseline_exe:
                run.argv[0] = str(options.baseline_exe.resolve())
            run.env["CCCP_SETTINGSPATH"] = str((Path(run.cwd) / "Userdata/Settings.ini").resolve())
            settings = {"ResolutionX": width, "ResolutionY": height, "ResolutionMultiplier": "1", "ShowAdvancedPerfStats": "0",
                        **peer.get("settings", {})}
            if definition["name"] == "toggle-off":
                settings["NetworkConnectionIndicator"] = "1"
            seed_settings(run, settings)
            runtimes[(definition["name"], who)] = Path(run.cwd)
            if held and who == "seat4":
                drop = SeatDrop(run, drop_marker, options.timeout - 10)
        # The existing wait-peers lever holds the host until all three joiners are ready.
        for _, run in runs:
            run.start()
        for who, run in runs:
            if who != "host" and run.poll() is None:
                run.terminate(0, "local layout host finished")
            records[who] = run.finish()
    except (OSError, RuntimeError, ValueError) as failure:
        error = str(failure)
    finally:
        if drop:
            drop.close()
        for who, run in runs:
            run.close()
            records.setdefault(who, run.record)
    path = probe.parent / "net-ui-result.json"
    native = json.loads(path.read_text(encoding="utf-8")) if path.is_file() else {}
    result = {"records": records, "probe": native, "error": error, "port": port, "size": definition["size"],
              "topology": "single-box", "proof": False, "capture": str(probe.parent / "connection.png"),
              "reference_capture": str(probe.parent / "reference.png"), "drop": drop.evidence if drop else None}
    write_json(root / "local-result.json", result)
    return result, runtimes


def run_layout_detector(options, out, repo, port):
    scenario = scene_document()
    write_json(out / "connection-indicator.json", scenario)
    baseline = repo == options.baseline_repo
    cases, retained = {}, {}
    for index, definition in enumerate(scenario["runs"]):
        name = definition["name"]
        if baseline and not name.startswith(("HUD_", "Seats_")):
            continue
        print("[connection-detector] " + ("base " if baseline else "tip ") + name + " port=" + str(port + index), flush=True)
        evidence, runtimes = local_case(options, repo, out / name, definition, scenario["scripts"], port + index, retained)
        retained.update(runtimes)
        native = evidence["probe"]
        checks = frame_colours(native, Path(evidence["capture"]), definition["size"])
        reason = evidence["error"] or native.get("error", "probe incomplete")
        if baseline:
            surface, state, _ = name.split("_")
            control = "LabelOwnConnection" if surface == "HUD" else "NetworkSeatLink"
            if surface == "Seats" and state != "held":
                control += str(STATES[state][0])
            passed = native.get("pass") is False and "unknown control: " + control in reason and native.get("failed_observation", {}).get("service") == "Running"
            if state == "held":
                passed &= bool(evidence["drop"] and evidence["drop"].get("dropped"))
            reference = Path(evidence["reference_capture"])
            passed &= reference.is_file()
            if reference.is_file():
                with Image.open(reference) as frame:
                    passed &= frame.size == tuple(int(part) for part in definition["size"].split("x"))
            line = ("RED " if passed else "FAIL ") + name + " " + reason
        else:
            host = evidence["records"].get("host", {})
            passed = evidence["error"] is None and host.get("exit_code") == 0 and not host.get("timed_out") and native.get("pass") is True and native.get("complete") is True
            passed &= len(evidence["records"]) == len(definition["peers"]) and all(row.get("started") and not row.get("timed_out") for row in evidence["records"].values())
            passed &= all(checks)
            if name.startswith("Seats_held_"):
                passed &= bool(evidence["drop"] and evidence["drop"].get("dropped"))
            if name.startswith(("HUD_", "Seats_")) or name.endswith("-match"):
                passed &= bool(checks)
            line = ("GREEN " if passed else "FAIL ") + name + (" " + reason if not passed else "")
        cases[name] = {"pass": bool(passed), "line": line, "saved_frame_colour_checks": len(checks),
                       "evidence": str(out / name / "local-result.json")}
        print(line, flush=True)
    passed = bool(cases) and all(row["pass"] for row in cases.values())
    scored = {"cases": cases, "pass": passed, "driver_exit_code": 0 if passed else 1, "topology": "single-box", "proof": False,
              "runner": "run_sim_test.make_run", "port_block": [port, port + len(scenario["runs"]) - 1]}
    write_json(out / "picture-results.json", scored)
    return scored


def run_multiplayer(options, out, repo):
    scenario = out / "connection-indicator.json"
    write_json(scenario, scene_document())
    command = [sys.executable, str(Path(__file__).parent / "e2e_video.py"), "--repo", str(repo), "--out", str(out / "capture"),
               "--scenario", str(scenario.with_suffix("")), "--peer-boxes", options.peer_boxes, "--fps", "1", "--capture-peer", "host",
               "--token", "HOST_ADDRESS=" + options.host_address, "--port", str(options.port), "--port-block", str(options.port) + "-" + str(options.port + 79)]
    if options.pool_registry:
        command += ["--pool-registry", str(options.pool_registry)]
    if options.layout_detector:
        command += ["--layout-detector"]
    code = subprocess.run(command, check=False, env=options.recording_env).returncode
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
            expected = "LabelOwnConnection" if surface == "HUD" else "NetworkSeatLink"
            if surface == "Seats" and state != "held":
                expected += str(STATES[state][0])
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
    passed = bool(cases) and all(row["pass"] for row in cases.values())
    scored = {"cases": cases, "driver_exit_code": code, "capture": capture.get("name"), "pass": passed,
              "topology": "single-box" if options.layout_detector else "spread", "proof": passed and not options.layout_detector,
              "requested_peer_boxes": options.peer_boxes}
    write_json(out / "picture-results.json", scored)
    return scored


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--baseline-repo", type=Path)
    parser.add_argument("--baseline-exe", type=Path)
    parser.add_argument("--single-engine", action="store_true")
    parser.add_argument("--layout-detector", action="store_true", help="four local hidden peers on loopback; no spread or ffmpeg; topology=single-box, proof=false")
    parser.add_argument("--peer-boxes")
    parser.add_argument("--pool-registry", type=Path)
    parser.add_argument("--host-address")
    parser.add_argument("--port", type=int, default=49840)
    parser.add_argument("--timeout", type=int, default=240, help="single-engine startup and probe budget, including cold module loads")
    options = parser.parse_args()
    if not options.single_engine and not options.layout_detector and (not options.peer_boxes or not options.host_address):
        parser.error("four-seat pictures need --peer-boxes and --host-address assigned by the lead")
    if options.layout_detector and options.single_engine:
        parser.error("--layout-detector runs four seats; it cannot be combined with --single-engine")
    if options.layout_detector and (options.peer_boxes or options.host_address):
        parser.error("--layout-detector starts only local peers; omit --peer-boxes and --host-address")
    if options.layout_detector and not 1024 <= options.port <= 65535 - 2 * len(scene_document()["runs"]):
        parser.error("--port must leave a lane block for both trees' local cases")
    if options.timeout < 120:
        parser.error("--timeout must leave at least 120 seconds for cold loads and framed gestures")
    if options.baseline_repo:
        sha = subprocess.check_output(["git", "-C", str(options.baseline_repo), "rev-parse", "HEAD"], text=True).strip()
        if sha != BASE:
            parser.error("baseline checkout must be " + BASE)
        baseline_exe = options.baseline_exe or options.baseline_repo / "Cortex Command.exe"
        baseline_digest = hashlib.sha256(baseline_exe.read_bytes()).hexdigest()
    options.out.mkdir(parents=True, exist_ok=False)
    result = {"base": BASE, "cases": {}, "needs_testing": [], "pass": False,
              "topology": "single-box" if options.layout_detector or options.single_engine else "spread", "proof": False,
              "mode": "layout-detector" if options.layout_detector else "single-engine" if options.single_engine else "four-box-proof"}
    if not options.single_engine and not options.layout_detector:
        try:
            if not Path(__file__).with_name("box_load.py").is_file():
                raise ValueError("four-box proof cannot start: missing tools/box_load.py, required by the spread tool; --layout-detector runs locally without it")
            options.recording_env = recording_environment(options.pool_registry)
        except (OSError, ValueError) as error:
            result["error"] = str(error)
            write_json(options.out / "result.json", result)
            print("[connection-pictures] " + str(error), flush=True)
            return 1
    if options.baseline_repo:
        before = run_single(options.baseline_repo, options.out / "RED-toggle", settings_steps(True, False), "640x360", exe=options.baseline_exe, timeout=options.timeout)
        red = not before["pass"] and "LabelNetworkConnectionIndicator" in failure_line(before)
        result["cases"]["toggle_RED"] = {"pass": red, "line": failure_line(before)}
        print(("RED" if red else "FAIL") + " toggle 640x360 " + failure_line(before), flush=True)
    else:
        result["needs_testing"].append("exact baseline executable and checkout for RED and whole-frame guards")
    runtime = None
    for name, initial, pick in (("default-on", True, False), ("off-relaunch", False, True), ("on-relaunch", True, None)):
        current = run_single(options.repo, options.out / name, settings_steps(initial, pick), "640x360", runtime=runtime,
                             timeout=options.timeout, expected_saved=pick if pick is not None else initial)
        result["cases"][name] = {"pass": current["pass"], "line": "GREEN " + name if current["pass"] else failure_line(current)}
        runtime = Path(current["runtime"])
        print(result["cases"][name]["line"], flush=True)
    for size in SIZES:
        after_root = options.out / ("guard-tip-" + size)
        after = run_single(options.repo, after_root, guard_steps(), size, guard=True, timeout=options.timeout)
        guard = {"pass": after["pass"], "compared": False}
        if options.baseline_repo:
            before_root = options.out / ("guard-base-" + size)
            before = run_single(options.baseline_repo, before_root, guard_steps(), size, exe=options.baseline_exe, guard=True, timeout=options.timeout)
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
        tip = run_layout_detector(options, options.out / "tip", options.repo, options.port) if options.layout_detector else run_multiplayer(options, options.out / "tip", options.repo)
        result["cases"].update(tip["cases"])
        result["cases"]["scene_driver_tip"] = {"pass": tip["driver_exit_code"] == 0}
        if options.baseline_repo:
            base = run_layout_detector(options, options.out / "base", options.baseline_repo, options.port + len(scene_document()["runs"])) if options.layout_detector else run_multiplayer(options, options.out / "base", options.baseline_repo)
            result["cases"].update({"RED_" + name: row for name, row in base["cases"].items()})
    result["pass"] = not result["needs_testing"] and all(row.get("pass", False) for row in result["cases"].values())
    if options.baseline_repo:
        baseline_head = subprocess.check_output(["git", "-C", str(options.baseline_repo), "rev-parse", "HEAD"], text=True).strip()
        result["baseline_exe_sha256"] = baseline_digest
        result["baseline_unchanged"] = baseline_head == BASE and hashlib.sha256(baseline_exe.read_bytes()).hexdigest() == baseline_digest
        result["pass"] &= result["baseline_unchanged"]
    result["proof"] = result["pass"] and not options.layout_detector and not options.single_engine
    write_json(options.out / "result.json", result)
    print(json.dumps({"pass": result["pass"], "needs_testing": result["needs_testing"]}), flush=True)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
