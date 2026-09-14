"""Drives the Multiplayer settings tab's five pages through the menu script + probe.

RED (--failing, run against the base executable): the script's first
``assert_visible TabMultiplayerSettings 1`` fails because the control does not
exist; the run must exit non-zero with a ``[menu-script] FAILED`` line.

GREEN (tip): per size (640x360 and 960x540) and both callers:

- ``main``: opens Options from the main menu, walks Player / Chat / Recovery /
  Files / Internet, asserts every control sits inside its page box and its text
  fits, and dumps each page (JSON + PNG). Then: three invalid display names
  (empty, 25 characters, a control character) each keep the page open with the
  inline error; Restore Defaults resets only the draft; a valid Apply persists;
  Back returns to the exact caller screen.
- ``persist``: a second engine run sharing the ``main`` run's private
  Settings.ini reads the applied values back.
- ``pause``: a paired e2e match where the probe opens Settings from the pause
  menu, walks every page, and dumps it.

Usage:
    python tools/test_settings_multiplayer.py --case pages --out D:/mx/<lane>/green-1
    python tools/test_settings_multiplayer.py --case pages --failing --out D:/mx/<lane>/red-base
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_sim_test import make_run
from test_telemetry_bundle import set_visual_resolution

SIZES = ("640x360", "960x540")
PORT = 48370

# Every control that must sit inside its page box and fit its text, per page.
PAGE_CONTROLS = {
    "Player": (
        "LabelMpDisplayName", "TextMpDisplayName", "LabelMpStatusWidget", "ComboMatchStatusWidget",
        "CheckboxMpNotifications", "CheckboxMpPrediction", "LabelMpPlayerError",
        "ButtonMpPlayerDefaults", "LabelMpPlayerFooter",
    ),
    "Chat": (
        "CheckboxMpChatVisible", "CheckboxMpChatSound", "LabelMpChatScope", "ComboMpChatScope",
        "CheckboxMpChatNotify", "LabelMpChatTextSize", "ComboMpChatTextSize", "LabelMpChatError",
        "ButtonMpMutedPlayers", "LabelMpMutedReason", "ButtonMpChatDefaults", "LabelMpChatFooter",
    ),
    "Recovery": (
        "CheckboxMpAutoReconnect", "CheckboxMpOfferRejoin", "LabelMpLastHostTitle", "LabelMpLastHost",
        "LabelMpRecoveryTitle", "LabelMpRecoveryRecord", "LabelMpRecoveryStatus", "LabelMpRecoveryError",
        "ButtonMpRejoin", "ButtonMpCancelRecovery", "LabelMpRecoveryFooter",
    ),
    "Files": (
        "LabelMpAutosaveTitle", "LabelMpAutosave", "LabelMpAutosaveIntTitle", "LabelMpAutosaveInterval",
        "ButtonMpOpenAutosaves", "LabelMpAutosaveInfo", "LabelMpDiagDirTitle", "TextMpDiagDir",
        "ButtonMpSaveDiagnostics", "ButtonMpOpenDiagDir", "ButtonMpCopyDiagPath",
        "CheckboxMpRecordReplays", "ButtonMpReplays", "LabelMpFilesMessage", "LabelMpFilesFooter",
    ),
    "Internet": (
        "LabelMpDirUrl", "TextMpDirUrl", "LabelMpDirPin", "TextMpDirPin", "LabelMpDirStatusTitle",
        "LabelMpDirStatus", "ButtonMpConnDetails", "ButtonMpNatRelay", "LabelMpInternetReason",
        "LabelMpInternetError", "ButtonMpInternetDefaults", "LabelMpInternetFooter",
    ),
}

# P-row defaults read back out of each page's dump JSON.
PAGE_DEFAULTS = {
    "Player": {
        "TextMpDisplayName": {"text": "Player"},
        "ComboMatchStatusWidget": {"text": "Auto"},
        "CheckboxMpNotifications": {"checked": True},
        "CheckboxMpPrediction": {"checked": True},
    },
    "Chat": {
        "CheckboxMpChatVisible": {"checked": True},
        "CheckboxMpChatSound": {"checked": False},
        "CheckboxMpChatNotify": {"checked": True},
        "ComboMpChatScope": {"text": "All"},
        "ComboMpChatTextSize": {"text": "Small"},
        "ButtonMpMutedPlayers": {"enabled": False},
    },
    "Recovery": {
        "CheckboxMpAutoReconnect": {"checked": True},
        "CheckboxMpOfferRejoin": {"checked": True},
    },
    "Files": {
        "TextMpDiagDir": {"text": ""},
        "CheckboxMpRecordReplays": {"checked": True},
        "ButtonMpReplays": {"enabled": False},
    },
    "Internet": {
        "TextMpDirUrl": {"text": ""},
        "TextMpDirPin": {"text": ""},
        "ButtonMpConnDetails": {"enabled": False},
        "ButtonMpNatRelay": {"enabled": False},
    },
}


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def menu_step(command: str) -> dict:
    return {"op": "menu", "command": command}


def page_asserts(page: str, controls: tuple) -> str:
    text = f"activate TabMpPage{page}\nwait 2\nassert_visible CollectionBoxMpPage{page} 1\n"
    for control in controls:
        text += f"assert_rect_inside {control} parent\nassert_text_fits {control}\n"
    return text


def main_script() -> str:
    text = (
        "wait 40\nactivate ButtonMainToOptions\nwait 8\nassert_screen SettingsScreen\n"
        "assert_visible TabMultiplayerSettings 1\nassert_text_fits TabMultiplayerSettings\n"
        "assert_rect_inside TabMultiplayerSettings CollectionBoxSettingsBase\n"
        "activate TabMultiplayerSettings\nwait 3\nassert_visible CollectionBoxMultiplayerSettings 1\n"
        "assert_visible ButtonMultiplayerApply 1\nassert_enabled ButtonMultiplayerApply 0\n"
    )
    for page, controls in PAGE_CONTROLS.items():
        text += page_asserts(page, controls) + "dump_player_options\n"
    # Back on the Player page for the draft/apply flows.
    text += "activate TabMpPagePlayer\nwait 2\n"
    # Invalid names keep the page open with the inline error and write nothing.
    # settext/assert_label take the rest of the line verbatim — no quoting.
    for invalid in ("", "ABCDEFGHIJKLMNOPQRSTUVWXY", "Bad\x07Name"):
        text += (
            f"settext TextMpDisplayName {invalid}\npost_command ButtonMultiplayerApply\nwait 2\n"
            "assert_visible CollectionBoxMpPagePlayer 1\n"
            "assert_label LabelMpPlayerError Display name\n"
        )
    # A valid apply persists, then Restore Defaults resets only the draft:
    # the textbox returns to "Player" and Apply re-arms because the persisted
    # value still holds "AcePilot".
    text += (
        "settext TextMpDisplayName AcePilot\npost_command ButtonMultiplayerApply\nwait 2\n"
        "settext TextMpDisplayName ChangedAgain\npost_command ButtonMpPlayerDefaults\nwait 2\n"
        "assert_label TextMpDisplayName Player\nassert_enabled ButtonMultiplayerApply 1\n"
        "dump_player_options\n"
    )
    # Back returns to the exact caller.
    text += "post_command ButtonBackToMainMenu\nwait 5\nassert_screen MainScreen\nexit\n"
    return text


def persist_script() -> str:
    return (
        "wait 40\nactivate ButtonMainToOptions\nwait 8\nassert_screen SettingsScreen\n"
        "activate TabMultiplayerSettings\nwait 3\nassert_visible CollectionBoxMpPagePlayer 1\n"
        "assert_label TextMpDisplayName AcePilot\ndump_player_options\nexit\n"
    )


def pause_probe(root: Path) -> dict:
    steps = [
        {"op": "wait", "screen": "Pause"},
        menu_step("assert_visible ButtonSettings 1"),
        menu_step("activate ButtonSettings"),
        {"op": "wait", "screen": "PauseSettings"},
        menu_step("assert_visible TabMultiplayerSettings 1"),
        menu_step("activate TabMultiplayerSettings"),
        {"op": "wait", "renders": 3},
        menu_step("assert_visible CollectionBoxMultiplayerSettings 1"),
    ]
    for page, controls in PAGE_CONTROLS.items():
        steps += [menu_step(f"activate TabMpPage{page}"), {"op": "wait", "renders": 2},
                  menu_step(f"assert_visible CollectionBoxMpPage{page} 1")]
        for control in controls:
            steps += [menu_step(f"assert_rect_inside {control} parent"),
                      menu_step(f"assert_text_fits {control}")]
        steps += [menu_step("dump_player_options")]
    steps += [
        menu_step("post_command ButtonBackToMainMenu"),
        {"op": "wait", "screen": "Pause"},
        {"op": "signal", "name": "done"},
        {"op": "finish"},
    ]
    return {"schema": 1, "timeout_ms": 120000, "steps": steps}


def collect_dumps(runtime: Path, prefix: str, tag: str) -> list:
    rows = []
    for path in sorted(runtime.glob(f"ScreenShots/{prefix}_*.json"),
                       key=lambda path: int(path.stem.rsplit("_", 1)[1])):
        value = json.loads(path.read_text(encoding="utf-8"))
        rows.append({"dump": path.stem, "tag": tag, "json": str(path), "json_sha256": sha(path),
                     "png": str(path.with_suffix(".png")), "png_sha256": sha(path.with_suffix(".png"))
                     if path.with_suffix(".png").exists() else None,
                     "controls": {control["name"]: control for control in value["controls"]}})
    return rows


def check_defaults(dumps: list, tag: str, result: dict) -> None:
    # One dump per page, in PAGE_CONTROLS order, then the applied-state dump last.
    for index, (page, expected) in enumerate(PAGE_DEFAULTS.items()):
        dump = dumps[index]
        for name, want in expected.items():
            control = dump["controls"].get(name)
            assert control is not None, f"{tag}: {name} missing from {dump['dump']}"
            for key, value in want.items():
                actual = control.get(key)
                assert actual == value, (
                    f"{tag}: {page}/{name}.{key} expected {value!r} got {actual!r} in {dump['dump']}")


def run_menu_case(options, root: Path, size: str) -> dict:
    root.mkdir(parents=True, exist_ok=False)
    width, height = map(int, size.split("x"))
    script = root / "menu-main.txt"
    script.write_text(main_script(), encoding="utf-8")
    second = root / "menu-persist.txt"
    second.write_text(persist_script(), encoding="utf-8")
    result = {"case": "menu", "size": size, "runs": {}, "dumps": [], "checks": []}

    run = make_run(options.repo, ["-menu-script", str(script)], root / "main", 240,
                   env={"CCCP_HEADLESS": "1"})
    set_visual_resolution(run, width, height)
    record = run.start().finish()
    result["runs"]["main"] = record
    assert record.get("exit_code") == 0, record

    logs = "\n".join((run.out / leaf).read_text(encoding="utf-8", errors="replace")
                     for leaf in ("stdout.log", "stderr.log") if (run.out / leaf).exists())
    assert "[menu-script] FAILED:" not in logs, logs[-3000:]
    assert "unknown command" not in logs, logs[-3000:]

    # The private settings file proves the invalid applies wrote nothing and the
    # valid apply persisted.
    settings = (run.cwd / "Userdata/Settings.ini").read_text(encoding="utf-8")
    assert "NetworkDisplayName = AcePilot" in settings, settings[-2000:]
    assert "ABCDEFGHIJKLMNOPQRSTUVWXY" not in settings
    assert "Bad\x07Name" not in settings

    dumps = collect_dumps(run.cwd, "dump_player_options", "main")
    assert len(dumps) >= 6, f"{size}: expected >=6 dumps, got {len(dumps)}"
    check_defaults(dumps, f"{size}/main", result)
    result["dumps"] += dumps

    # A second engine run reads the persisted values back from the shared ini.
    persist = make_run(options.repo, ["-menu-script", str(second)], root / "persist", 180,
                       env={"CCCP_HEADLESS": "1"})
    (persist.cwd / "Userdata/Settings.ini").write_text(settings, encoding="utf-8")
    set_visual_resolution(persist, width, height)
    record = persist.start().finish()
    result["runs"]["persist"] = record
    assert record.get("exit_code") == 0, record
    persist_logs = "\n".join((persist.out / leaf).read_text(encoding="utf-8", errors="replace")
                             for leaf in ("stdout.log", "stderr.log") if (persist.out / leaf).exists())
    assert "[menu-script] FAILED:" not in persist_logs, persist_logs[-3000:]
    result["dumps"] += collect_dumps(persist.cwd, "dump_player_options", "persist")
    return result


def run_pause_case(options, root: Path, size: str) -> dict:
    root.mkdir(parents=True, exist_ok=False)
    width, height = map(int, size.split("x"))
    done = root / "done.json"
    host_script = root / "menu-pause-host.txt"
    host_script.write_text(
        f"wait 12\nassert_screen Pause\nwait_file {done} 120\nexit\n", encoding="utf-8")
    client_script = root / "menu-pause-client.txt"
    client_script.write_text(f"wait_file {done} 120\nexit\n", encoding="utf-8")
    inputs = root / "input.txt"
    inputs.write_text("100 100 START\n", encoding="utf-8")
    probe = pause_probe(root)
    (root / "probe.json").write_text(json.dumps(probe, indent=2) + "\n", encoding="utf-8")
    result = {"case": "pause", "size": size, "runs": {}, "dumps": []}

    runs, argv = {}, {}
    for who in ("host", "client"):
        args = ["-menu-script", str(host_script if who == "host" else client_script),
                "-net-match-service-e2e", "-net-port", str(PORT), "-net-match-peers", "2",
                "-net-match-ticks", "600", "-net-match-input-delay", "3", "-net-autosave-seconds", "0",
                "-input-script", str(inputs), "-net-match-report", str(root / f"{who}-match.json")]
        args += ["-net-host"] if who == "host" else ["-net-join", "127.0.0.1"]
        env = {"CCCP_HEADLESS": "1"}
        if who == "host":
            env["CC_TEST_NET_UI_SCRIPT"] = str(root / "probe.json")
        argv[who] = args
        runs[who] = make_run(options.repo, args, root / who, 240, env=env)
        set_visual_resolution(runs[who], width, height)

    records = {}

    def drive(who):
        try:
            records[who] = runs[who].start().finish()
        except Exception as error:
            records[who] = {"error": repr(error)}

    threads = [threading.Thread(target=drive, args=(who,)) for who in runs]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()

    for who, record in records.items():
        result["runs"][who] = record
        assert not record.get("timed_out"), record
        assert record.get("exit_code") == 0, record
        logs = "\n".join((runs[who].out / leaf).read_text(encoding="utf-8", errors="replace")
                         for leaf in ("stdout.log", "stderr.log") if (runs[who].out / leaf).exists())
        assert "[menu-script] FAILED:" not in logs, logs[-3000:]
        assert "unknown command" not in logs, logs[-3000:]
    result["dumps"] += collect_dumps(runs["host"].cwd, "dump_player_options", "pause")
    assert len(result["dumps"]) >= 5, f"{size}: expected >=5 pause dumps, got {len(result['dumps'])}"
    check_defaults(result["dumps"], f"{size}/pause", result)
    return result


def run_failing(options, root: Path, size: str) -> dict:
    root.mkdir(parents=True, exist_ok=False)
    script = root / "menu-red.txt"
    script.write_text(
        "wait 40\nactivate ButtonMainToOptions\nwait 8\nassert_screen SettingsScreen\n"
        "assert_visible TabMultiplayerSettings 1\nexit\n", encoding="utf-8")
    run = make_run(options.repo, ["-menu-script", str(script)], root / "red", 180,
                   env={"CCCP_HEADLESS": "1"})
    set_visual_resolution(run, *map(int, size.split("x")))
    record = run.start().finish()
    logs = "\n".join((run.out / leaf).read_text(encoding="utf-8", errors="replace")
                     for leaf in ("stdout.log", "stderr.log") if (run.out / leaf).exists())
    assert record.get("exit_code") not in (0, None), record
    assert "[menu-script] FAILED: assert_visible" in logs, logs[-3000:]
    return {"case": "red", "size": size, "record": record}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", choices=["pages"], default="pages")
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--size", choices=[*SIZES, "all"], default="all")
    parser.add_argument("--failing", action="store_true",
                        help="RED arm: expect the tab assert to fail (base executable).")
    parser.add_argument("--exe-sha256", default=None)
    options = parser.parse_args()

    options.out.mkdir(parents=True, exist_ok=True)
    exe = options.repo / "Cortex Command.exe"
    exe_sha = sha(exe)
    if options.exe_sha256:
        assert exe_sha == options.exe_sha256, f"exe hash changed: {exe_sha} != {options.exe_sha256}"
    result = {"pass": True, "exe_sha256": exe_sha, "sizes": {}}

    sizes = SIZES if options.size == "all" else (options.size,)
    for size in sizes:
        if options.failing:
            result["sizes"][size] = {"red": run_failing(options, options.out / size / "red", size)}
        else:
            result["sizes"][size] = {
                "menu": run_menu_case(options, options.out / size / "menu", size),
                "pause": run_pause_case(options, options.out / size / "pause", size),
            }
    assert sha(exe) == exe_sha, "exe changed during the run"
    (options.out / "result.json").write_text(json.dumps(result, indent=2, default=str) + "\n",
                                            encoding="utf-8")
    print(json.dumps({"pass": True, "exe_sha256": exe_sha,
                      "sizes": list(result["sizes"])}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
