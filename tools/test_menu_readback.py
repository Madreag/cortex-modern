"""Read real menu controls and drive scoped input through private engine runs."""

import argparse
import hashlib
import json
import os
import re
import subprocess
import threading
from pathlib import Path

from PIL import Image

from run_sim_test import make_run
from test_telemetry_bundle import set_visual_resolution


CASES = ("landing", "settings", "pages", "combo-fit", "lobby", "pause", "live", "input", "input-parity", "disabled",
         "scope-off", "network", "net-chat", "net-recovery", "net-files", "net-internet", "misc-page",
         "lobby-name", "net-options", "net-activity", "net-host-left", "net-resume", "host-defaults", "host-stun", "host-stun-empty", "host-relay", "net-connection", "world-open-seat", "repair", "local-end-match", "prehost-visibility", "oracles")
LANDING = "wait 40\nactivate ButtonMainToMultiplayer\nwait 12\nassert_substate Landing\n"
OPTIONS = "wait 40\nactivate ButtonMainToOptions\nwait 8\nassert_screen SettingsScreen\n"
PAGES = ("Video", "Audio", "Input", "Gameplay", "Misc", "Network")
LAYOUT_PAIRS = {
    "Gameplay": (("CheckboxShowForeignItems", "CheckboxEnemyHUD"),),
    "Misc": (("CheckboxShowLoadingScreenProgressReport", "CheckboxShowAdvancedPerfStats"),),
}


def layout_readback(capture):
    rows = {control["name"]: control for control in capture["controls"]}
    results = []
    for left, right in LAYOUT_PAIRS.get(capture.get("settings_page"), ()):
        a, b = rows[left]["rect"], rows[right]["rect"]
        overlap = a[0] < b[0] + b[2] and b[0] < a[0] + a[2] and a[1] < b[1] + b[3] and b[1] < a[1] + a[3]
        results.append({"controls": [left, right], "rects": [a, b], "pitch": [b[0] - a[0], b[1] - a[1]], "pass": not overlap})
    for name in ("LabelHostSeatDlgActionHint", "TextNetworkStunServers", "LabelLobbyPlayer0", "LabelLobbyPlayer1", "LabelLobbyPlayer2"):
        if name in rows:
            results.append({"control": name, "rect": rows[name]["rect"], "text": rows[name].get("text"),
                            "measurement": rows[name].get("text_measure"), "pass": rows[name].get("text_fits") is True})
    hint = rows.get("LabelHostSeatDlgActionHint", {})
    if rows.get("ButtonHostSeatDlgKick", {}).get("enabled"):
        results.append({"control": "LabelHostSeatDlgActionHint", "kind": "selected-seat-hint", "text": hint.get("text"),
                        "pass": "host's own seat" not in hint.get("text", "")})
    return results


# The network page's own selector names its sub-pages; a script reaches one as "Network:<page>"
# once the network page is up - the same reach a player's tab clicks take.
NETWORK_PAGES = ("Player", "Chat", "Recovery", "Files", "Internet", "Connection")
NETWORK_TABS = tuple("TabNetPage" + page for page in NETWORK_PAGES)
NETWORK_BOXES = tuple("CollectionBoxNetPage" + page for page in NETWORK_PAGES)
NETWORK_PAGE_BOX = NETWORK_BOXES[0]
# Every control the player page owns, measured where it is drawn. The fixed-delay row belongs to
# the fixed policy, so it is drawn only there - the cue the video page's resolution rows already use.
NETWORK_ROWS = ("LabelNetworkDisplayName", "TextNetworkDisplayName", "LabelNetworkDelayPolicy",
                "RadioNetworkDelayAuto", "RadioNetworkDelayFixed", "LabelNetworkIdleWait",
                "TextNetworkIdleWait", "LabelNetworkIdleWaitHint", "LabelNetworkPathHorizon",
                "TextNetworkPathHorizon", "LabelNetworkPathHorizonHint", "CheckboxNetworkAutoRepair",
                "CheckboxNetworkToasts", "CheckboxNetworkPrediction", "CheckboxNetworkDiagnostics",
                "LabelMatchStatusWidget", "ComboMatchStatusWidget")
NETWORK_FIXED_ROWS = ("LabelNetworkFixedDelay", "TextNetworkFixedDelay", "LabelNetworkFixedDelayHint")
MAX_INPUT_DELAY_FRAMES = 60  # NetMatchConfigUtil::c_MaxInputDelayFrames, which the hint states.
# The saved preferences a case starts from, and what the page must have written when it ends.
NETWORK_SEED = {"NetworkDisplayName": "ScoutLead", "NetworkHostDelayPolicy": "Auto",
                "NetworkHostIdleWaitMinutes": "10", "NetworkHostAutoRepair": "1", "NetworkInputDelayFrames": "0",
                "NetworkPathHorizonTicks": "30",
                "NetworkToastsEnabled": "0", "LocalPrediction": "0", "NetworkShowDiagnostics": "0", "NetworkMatchStatusMode": "Auto"}
NETWORK_SAVED = {"NetworkDisplayName": "WingCmd", "NetworkHostDelayPolicy": "Fixed",
                 "NetworkHostIdleWaitMinutes": "25", "NetworkHostAutoRepair": "0", "NetworkInputDelayFrames": "7",
                 "NetworkPathHorizonTicks": "45",
                 "NetworkToastsEnabled": "1", "LocalPrediction": "1", "NetworkShowDiagnostics": "1"}
# The Misc page's own rows; the match-status combo moved to the player page, so a dump of this box
# must not name it - the absence is asserted against the whole capture, not a missing-control assert.
MISC_ROWS = ("CheckboxSkipIntro", "CheckboxShowToolTips", "CheckboxShowLoadingScreenProgressReport",
             "CheckboxShowAdvancedPerfStats", "CheckboxMeasureLoadingTime", "CheckboxUseMonospaceConsoleFont",
             "CheckboxDisableFactionBuyMenuThemes", "CheckboxDisableFactionBuyMenuThemeCursors",
             "LabelSceneBackgroundAutoScale", "LabelSceneBackgroundAutoScaleSetting", "SliderSceneBackgroundAutoScale")
MISC_GONE = ("LabelMatchStatusWidget", "ComboMatchStatusWidget")
CHAT_SEED = {"NetworkChatVisible": "1", "NetworkChatSound": "0", "NetworkChatNotify": "1",
             "NetworkChatDefaultScope": "Team", "NetworkChatTextSize": "Large", "NetworkChatKey": "T"}
CHAT_SAVED = {"NetworkChatVisible": "0", "NetworkChatNotify": "0"}
RECOVERY_SEED = {"NetworkAutoReconnect": "1", "NetworkOfferStoredRejoin": "1"}
RECOVERY_SAVED = {"NetworkAutoReconnect": "0", "NetworkOfferStoredRejoin": "0"}
FILES_SEED = {"AutosaveSeconds": "45", "NetworkRecordReplays": "0", "NetworkAutosavesKept": "3"}
FILES_SAVED = {"NetworkDiagnosticsDirectory": "D:/diag-lane", "NetworkRecordReplays": "1", "NetworkAutosavesKept": "5"}
# AutosaveStore::c_MinRetainedAutosaves-c_MaxRetainedAutosaves, as the page writes the hint from them.
AUTOSAVES_KEPT_HINT = "1-10, this machine"
# The settings reader cuts values at "//", so the directory url persists scheme-less;
# NetDirectoryClient puts https:// back on (NetDirectoryClient.cpp:117).
INTERNET_SEED = {"SessionDirectoryUrl": "dir.example.test/serve",
                 "SessionDirectoryCertSha256": "a" * 64}
INTERNET_SAVED = {"SessionDirectoryUrl": "newdir.example.test/serve",
                  "SessionDirectoryCertSha256": "b" * 64}
STUN_DEFAULT = "stun.l.google.com:19302,stun.cloudflare.com:3478,stun.nextcloud.com:443"
NAT_KEYS = ("NetworkIceEnable", "NetworkStunServers", "NetworkTurnServers", "NetworkTurnUser", "NetworkTurnPass")
NAT_LABEL = "Internet: NAT traversal (STUN)"
NAT_STATES = ("Automatic", "Off (LAN or port-forwarded only)")
NAT_HINT = "Players behind home routers connect directly. Off means they need your port forwarded."
RELAY_KEYS = (*NAT_KEYS, "NetworkHostRelayMode", "NetworkConnectionMode", "NetworkPlayerTurnServers", "NetworkPlayerTurnUser", "NetworkPlayerTurnPass")
CONNECTION_ROWS = ("LabelNetworkConnection", "ComboNetworkConnection", "LabelNetworkConnectionHint",
                   "LabelNetworkStunServers", "TextNetworkStunServers", "LabelNetworkStunHint", "LabelNetworkOwnRelay",
                   "LabelNetworkRelayAddress", "TextNetworkRelayAddress", "LabelNetworkRelayUser", "TextNetworkRelayUser",
                   "LabelNetworkRelayPass", "TextNetworkRelayPass", "LabelNetworkRelayHint")
# One value column across the five network pages: every page's value/second-column control starts at
# this offset from its page box, and every row rides the Misc page's 20px pitch.
NETWORK_VALUE_COLUMN = 190
# The same page-relative column on every settings page (Video through Network).
SETTINGS_VALUE_COLUMN = 190
PAGE_FIRST_VALUE = {
    "Video": "ComboPresetResolution",
    "Audio": "SliderMasterVolume",
    "Input": "LabelP1SelectedDevice",
    "Gameplay": "CheckboxBlipOnRevealUnseen",
    "Misc": "CheckboxShowToolTips",
    "Network": "TextNetworkDisplayName",
}
FILES_BUTTONS = ("ButtonNetOpenAutosaves", "ButtonNetCopyAutosavesPath", "ButtonNetOpenDiagnostics",
                 "ButtonNetCopyDiagPath", "ButtonNetSaveDiagnostics")
# Video and Input rows that must stand 20 px tall.
VIDEO_INPUT_FIT = (
    "ComboPresetResolution",
    "LabelP1DeviceType", "LabelP1SelectedDevice",
    "LabelP2DeviceType", "LabelP2SelectedDevice",
    "LabelP3DeviceType", "LabelP3SelectedDevice",
    "LabelP4DeviceType", "LabelP4SelectedDevice",
)
PAUSE_PAGES = ("Video", "Audio", "Input", "Gameplay", "Misc")
PAUSE_PAGE_FIRST_VALUE = {
    "Video": "ComboPresetResolution",
    "Audio": "SliderMasterVolume",
    "Input": "LabelP1SelectedDevice",
    "Gameplay": "CheckboxBlipOnRevealUnseen",
    "Misc": "CheckboxShowToolTips",
}
SIZE_GATES = (
    *((case, size) for case in ("lobby", "host-defaults", "host-stun", "host-stun-empty", "host-relay", "net-connection", "world-open-seat", "repair", "pause", "network", "net-host-left")
      for size in ("640x360", "960x540", "1280x720")),
    ("net-chat", "960x540"),
    ("net-chat", "1280x720"),
    ("lobby-name", "640x360"),
    ("lobby-name", "960x540"),
    ("lobby-name", "1280x720"),
    ("lobby-name", "1920x1080"),
)
# CalculateWidth adds each printable glyph's m_Width (GUIFont.cpp:333). FontSmall's
# thinnest printable cell is 2 px, so 139 characters exceed the 276 px status row.
FONT_SMALL_MIN_GLYPH = 2
SHARE_STATUS_ROW = 276
WIDE_SHARE_HOST = "2001:" + "0" * 140
assert len(WIDE_SHARE_HOST) * FONT_SMALL_MIN_GLYPH > SHARE_STATUS_ROW, (
    len(WIDE_SHARE_HOST), FONT_SMALL_MIN_GLYPH, SHARE_STATUS_ROW)


def page_value_columns(captures, first_value):
    rows = []
    for capture in captures:
        page = capture["settings_page"].split(":")[0]
        if page not in first_value:
            continue
        box = next(c for c in capture["controls"] if c["name"] == f"CollectionBox{page}Settings")
        value = next(c for c in capture["controls"] if c["name"] == first_value[page])
        rel_x = value["rect"][0] - box["rect"][0]
        rows.append([page, value["name"], rel_x])
        expected = 245 if page in ("Gameplay", "Misc") else SETTINGS_VALUE_COLUMN
        assert rel_x == expected, (page, value["name"], rel_x, expected)
    return rows


def video_input_fit_rows(captures, required):
    rows = [[capture["settings_page"], control["name"], control["rect"][3], control.get("text_fits"), control.get("text")]
            for capture in captures for control in capture["controls"] if control["name"] in VIDEO_INPUT_FIT]
    names = {row[1] for row in rows}
    assert required <= names, (required - names, rows)
    assert rows and all(row[2] == 20 and row[3] for row in rows), rows
    return rows


def share_status_row(capture, port, host=None):
    status = next(c for c in capture["controls"] if c["name"] == "LabelMultiplayerStatus")
    text = status["text"]
    assert f":{port}" in text, status
    assert "\n" in text, status
    address = text.split("\n", 1)[1]
    assert address.endswith(f":{port}"), status
    if host is not None:
        assert address.startswith(host + ":") or address == f"{host}:{port}", status
    row = status["row_width"]
    word = status["word_width"]
    scroll = status["overflow_scroll"]
    assert row == status["rect"][2], status
    assert scroll == (word > row), status
    return status
NETWORK_ACTION_COLUMN = 330
INTERNET_HINT = "host[:port][/path] - https:// is implied"
INTERNET_REASON = "Connection sets your route. Host Options > Network sets the match's relay."
# The wire's display-name cap; the landing name box and -net-player-name refuse past it.
DISPLAY_NAME_MAX_BYTES = 64
# The host's saved session options steer the match; the client's own copy differs and must not.
HOST_OPTIONS = {"NetworkSlowPlayerBoundTicks": "7", "NetworkSlowPlayerPolicy": "Pause", "NetworkHostDelayPolicy": "Fixed", "NetworkHostIdleWaitMinutes": "25", "NetworkHostAutoRepair": "0",
                "NetworkPathHorizonTicks": "45"}
CLIENT_OPTIONS = {"NetworkSlowPlayerBoundTicks": "3", "NetworkSlowPlayerPolicy": "Substitute", "NetworkHostDelayPolicy": "Auto", "NetworkHostIdleWaitMinutes": "5", "NetworkHostAutoRepair": "1",
                  "NetworkPathHorizonTicks": "15"}
MATCH_RULES = {"delay_policy": 2, "idle_wait_minutes": 25, "automatic_repair": False, "path_horizon_ticks": 45,
               "slow_player_bound_ticks": 7, "slow_player_policy": 2}
# A combo box draws its selected item left of the drop-down button, so its text budget is narrower than its rect.
COMBO_BUTTON = 17
FIT_LINE = re.compile(r"assert_text_fits (\w+).*?rect=\[(-?\d+),(-?\d+),(-?\d+),(-?\d+)\].*?available=\[(-?\d+),(-?\d+)\]")
WATCHED = ("ComboBrainlessHumansSpectate", "ComboMatchStatusWidget")
# The landing panel's focus ring, in the (y, x) order MenuAutomation builds it in: the three action
# buttons of the first row, then Resume on the second, then the footer.
ORDER = ("TextMultiplayerName", "ButtonMultiplayerHostGame", "ButtonMultiplayerJoinGame",
         "ButtonMultiplayerReplays", "ButtonMultiplayerResumeGame", "ButtonBackToMain", "ButtonSaveDiagnostics")
# The resume screen's own rows, measured where they are drawn.
RESUME_ROWS = ("LabelResumeTitle", "LabelResumeBlurb", "ListResumeMatches", "LabelResumeSelected",
               "LabelResumeStatus", "ButtonResumeStart", "ButtonResumeBack")
RESET_INPUT = ("wait 40\nactivate ButtonMainToOptions\nwait 5\nassert_visible TabInputSettings 1\n"
               "activate TabInputSettings\nwait 3\npost_command ButtonP2Clear\npost_command ButtonP2Clear\n"
               "wait 3\npost_command ButtonP3Clear\npost_command ButtonP3Clear\nwait 3\n"
               "post_command ButtonBackToMainMenu\nwait 5\n")
# The match pause menu's own rows, and the single-player rows it must not carry on any peer.
MATCH_ROWS = ("ButtonLeaveMatch", "ButtonMatchOptions", "ButtonPauseMatch", "ButtonSettings",
              "ButtonSaveDiagnostics", "ButtonEndMatch", "ButtonResume")
SINGLE_PLAYER_ROWS = ("ButtonBackToMain", "ButtonSaveOrLoadGame", "ButtonModManager")
# No scripted press: a scripted element reads as pressed on every render frame of its tick, so a START
# range opens the match pause menu and asks it for the way back out on alternate frames. The probe's
# key is one real edge instead.
INPUT_SCRIPT = "# the probe opens the match pause menu with a real key edge\n"


def sha(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def checks(control, parent):
    return (f"assert_visible {control} 1\nassert_rect_inside {control} {parent}\n"
            f"assert_rect_inside {control} viewport\nassert_text_fits {control}\n")


def seed_settings(path, values):
    """Put saved preferences in front of a run the way a player's own Settings.ini would."""
    text = path.read_text(encoding="utf-8-sig")
    for name, value in values.items():
        text, count = re.subn(rf"(?m)^([ \t]*{name}[ \t]*=[ \t]*)[^\r\n]*", lambda match: match[1] + value, text)
        if count == 0:
            text += f"\n\t{name} = {value}\n"
    path.write_text(text, encoding="utf-8")


def read_settings(path, names):
    values = {}
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        key, sep, value = line.partition("=")
        if sep and key.strip() in names:
            values[key.strip()] = value.strip()
    return values


def seeds(case):
    if case == "prehost-visibility":
        return {"host": {"SessionDirectoryUrl": "https://127.0.0.1:49479"}}
    if case in ("host-relay", "net-connection"):
        return {"host": {"SessionDirectoryUrl": ""}}
    if case in ("host-stun", "host-stun-empty"):
        values = {"SessionDirectoryUrl": ""}
        if case == "host-stun-empty":
            values.update({"NetworkIceEnable": "1", "NetworkStunServers": ""})
        return {"host": values}
    if case in ("network", "lobby-name"):
        return {"host": NETWORK_SEED}
    if case == "net-options":
        return {"host": HOST_OPTIONS, "client": CLIENT_OPTIONS}
    if case == "net-chat":
        return {"host": CHAT_SEED}
    if case == "net-recovery":
        return {"host": RECOVERY_SEED}
    if case == "net-files":
        return {"host": FILES_SEED}
    if case == "net-internet":
        return {"host": INTERNET_SEED}
    if case == "live":
        return {"host": {"NetworkRecordReplays": "1"}, "client": {"NetworkRecordReplays": "1"}}
    return {}


def host_lobby(port):
    return (LANDING + "activate ButtonMultiplayerHostGame\nwait 5\n"
            f"settext TextHostPort {port}\nsettext TextHostPlayers 2\n"
            "activate ButtonMultiplayerCreate\nwait 15\nassert_substate Lobby\n")


def net_page(page):
    """The reach a script has into the network page's own selector: the tab name it would click."""
    return (f"select_settings_page Network\nwait 3\nassert_settings_page Network\n"
            f"select_settings_page Network:{page}\nwait 3\nassert_settings_page Network:{page}\n"
            f"assert_visible CollectionBoxNetPage{page} 1\n")


def menu_step(command):
    return {"op": "menu", "command": command}


def probe_root(root, who):
    """One directory per peer's probe: the engine writes its result beside the script it was handed."""
    return root / f"{who}_probe"


def row_checks(control, parent):
    """The layout checks of `checks`, driven against a menu the probe reads inside a running match."""
    return [menu_step(f"assert_visible {control} 1"), menu_step(f"assert_rect_inside {control} {parent}"),
            menu_step(f"assert_rect_inside {control} viewport"), menu_step(f"assert_text_fits {control}")]


def pause_rows():
    return ([step for row in MATCH_ROWS for step in row_checks(row, "PauseScreen")] +
            [menu_step(f"assert_visible {row} 0") for row in SINGLE_PLAYER_ROWS])


def pause_probe(who, root):
    """One peer's local match pause menu: its rows, its settings pages, and a match that keeps running."""
    running = {"op": "assert", "equals": {"service": "Running", "paused": False}, "sim_at_least": 150}
    steps = [
        {"op": "wait", "sim_at_least": 150},
        {"op": "key_down", "key": "Escape"}, {"op": "key_up", "key": "Escape"},
        {"op": "wait", "screen": "Pause"}, running, *pause_rows(), menu_step("dump_host_options"),
        # H33's end and the third host-options origin: the adopted config mid-match, read-only on
        # every peer; only the host's row lights, and the round keeps running under the dialog.
        menu_step("activate ButtonMatchOptions"), {"op": "wait", "screen": "PauseMatchOptions"},
        menu_step("assert_rect_inside MatchOptionsBox viewport"),
        *row_checks("LabelMatchOptionsTitle", "MatchOptionsBox"),
        *row_checks("LabelMatchOptions", "MatchOptionsBox"),
        *row_checks("ButtonMatchOptionsClose", "MatchOptionsBox"),
        *row_checks("LabelMatchRepairHint", "MatchOptionsBox"),
        menu_step(f"assert_visible ButtonMatchRepairNow {1 if who == 'host' else 0}"),
        menu_step(f"assert_enabled ButtonMatchRepairNow {1 if who == 'host' else 0}"),
        {"op": "assert_control", "scope": "menu", "control": "LabelMatchOptions",
         "equals": {}, "text_contains": "When every human brain is lost"},
        {"op": "assert_control", "scope": "menu", "control": "LabelMatchOptions",
         "equals": {}, "text_contains": "Frame redundancy: 4 ticks"},
        menu_step("dump_host_options"),
        menu_step("activate ButtonMatchOptionsClose"), {"op": "wait", "screen": "Pause"},
        menu_step(f"assert_enabled ButtonEndMatch {1 if who == 'host' else 0}"),
        menu_step("activate ButtonSettings"), {"op": "wait", "screen": "PauseSettings"}]
    for page in PAUSE_PAGES:
        steps += [menu_step(f"select_settings_page {page}"), {"op": "wait", "renders": 3},
                  menu_step(f"assert_settings_page {page}"),
                  menu_step(f"assert_visible CollectionBox{page}Settings 1"),
                  menu_step("dump_player_options")]
        if page == "Gameplay":
            steps += row_checks("TabGameplaySettings", "CollectionBoxSettingsBase")
    steps += [
        menu_step("post_command ButtonBackToMainMenu"), {"op": "wait", "screen": "Pause"},
        *pause_rows(), menu_step("dump_host_options"), running]
    if who == "client":
        # The leaver's drive starts only once the host's checks are done, so the clean leave cannot
        # beat the running-match assertions. Gate the leave on a later sim tick so it still has
        # margin before the 400-tick cap. The menu pump that follows the leave still serves the
        # probe's menu-scope steps, so its finish lands there.
        steps += [{"op": "wait_file", "path": str(probe_root(root, "host") / "done.json")},
                  {"op": "wait", "sim_at_least": 220},
                  {"op": "signal", "name": "done"},
                  menu_step("activate ButtonLeaveMatch"), {"op": "wait", "screen": "PauseLeaveConfirm"},
                  menu_step("assert_rect_inside LeaveConfirmBox viewport"),
                  *row_checks("LabelLeaveConfirm", "LeaveConfirmBox"),
                  *row_checks("ButtonLeaveConfirm", "LeaveConfirmBox"),
                  *row_checks("ButtonLeaveCancel", "LeaveConfirmBox"),
                  {"op": "assert_control", "scope": "menu", "control": "LabelLeaveConfirm",
                   "equals": {}, "text_contains": "Leave the match"},
                  menu_step("dump_host_options"), menu_step("activate ButtonLeaveConfirm"),
                  {"op": "wait", "elapsed_ms": 1500}, {"op": "signal", "name": "left"}]
    else:
        steps += [{"op": "signal", "name": "done"}]
    steps += [{"op": "finish"}]
    return {"schema": 1, "timeout_ms": 90000, "steps": steps}


def host_activity_label(row):
    module = row.get("module") or ""
    return row["preset"] + (f" - {module}" if module else "")


def repair_probe(who, root):
    steps = [{"op": "wait", "service": "Running", "sim_at_least": 150},
             {"op": "key_down", "key": "F6"}, {"op": "key_up", "key": "F6"},
             {"op": "wait", "panel_open": True},
             {"op": "mouse_down", "control": "NetworkSeatsOptions"},
             {"op": "mouse_up", "control": "NetworkSeatsOptions"},
             {"op": "wait", "control": "NetworkSeatsOptionsText", "equals": {"visible": True}},
             {"op": "assert_control", "control": "NetworkSeatsOptionsText", "fits": True,
              "text_contains": "Repair match: Ready - pause menu > Match Options" if who == "host" else "Frame redundancy:"},
             {"op": "key_down", "key": "F6"}, {"op": "key_up", "key": "F6"},
             {"op": "wait", "panel_open": False},
             {"op": "key_down", "key": "Escape"}, {"op": "key_up", "key": "Escape"},
             {"op": "wait", "screen": "Pause"}, menu_step("activate ButtonMatchOptions"),
             {"op": "wait", "screen": "PauseMatchOptions"},
             {"op": "assert", "equals": {"service": "Running"}},
             menu_step("assert_rect_inside MatchOptionsBox viewport"),
             *row_checks("LabelMatchOptions", "MatchOptionsBox"),
             *row_checks("LabelMatchRepairHint", "MatchOptionsBox"),
             menu_step(f"assert_enabled ButtonMatchRepairNow {1 if who == 'host' else 0}")]
    if who == "host":
        steps += [*row_checks("ButtonMatchRepairNow", "MatchOptionsBox"),
                  menu_step("activate ButtonMatchRepairNow"), {"op": "wait", "renders": 2},
                  menu_step("assert_label LabelMatchRepairHint Every peer pauses and reloads the host's snapshot - press again"),
                  menu_step("assert_enabled ButtonMatchRepairNow 1"),
                  {"op": "assert", "equals": {"service": "Running"}},
                  menu_step("dump_host_options"),
                  {"op": "wait_file", "path": str(probe_root(root, "client") / "ready.json")},
                  menu_step("activate ButtonMatchRepairNow"), {"op": "wait", "renders": 2},
                  {"op": "signal", "name": "done"}]
    else:
        steps += [menu_step("assert_visible ButtonMatchRepairNow 0"),
                  {"op": "menu", "command": "activate ButtonMatchRepairNow", "accepted": False},
                  menu_step("assert_label LabelMatchRepairHint Repair is the host's call"),
                  menu_step("dump_host_options"), {"op": "signal", "name": "ready"},
                  {"op": "wait_file", "path": str(probe_root(root, "host") / "done.json")},
                  {"op": "signal", "name": "done"}]
    return {"schema": 1, "timeout_ms": 90000, "steps": steps + [{"op": "finish"}]}


def combo_drawn_follows(control):
    """The closed box's own line is the selected item, or the fit's shortening of it."""
    selected, drawn = control.get("text", ""), control.get("drawn", "")
    head = selected.split(" - ")[0]
    if drawn in (selected, head):
        return True
    trimmed = drawn[:-3] if drawn.endswith("...") else drawn
    return bool(trimmed) and (selected.startswith(trimmed) or head.startswith(trimmed))


def combo_name(text):
    return text.rsplit(" - ", 1)[0] if " - " in (text or "") else (text or "")


def host_scenes(dump):
    show = dump.get("show_metascenes")
    scenes = []
    for scene in dump.get("loaded_scenes") or []:
        if scene.get("location_zero") or scene.get("metagame_internal") or scene.get("saved_game_internal"):
            continue
        if scene.get("metascene_parent") and not show:
            continue
        scenes.append(scene)
    return scenes


def scene_is_compatible(activity, scene, teams=-1):
    if not scene:
        return False
    if teams > int(activity.get("min_teams") or 0):
        return False
    if (activity.get("activity_type") or "") == "GATutorial":
        return scene.get("name") == "Tutorial Bunker"
    areas = set(scene.get("areas") or [])
    return all(area in areas for area in (activity.get("required_areas") or []))


def allowed_host_activities(dump):
    # Missing census keys stay empty so a base-tip combo fails as an extra row.
    scenes = host_scenes(dump)
    return [row for row in (dump.get("game_activities") or [])
            if any(scene_is_compatible(row, scene) for scene in scenes)]


def combo_item_names(picker):
    if "items" not in picker:
        name = picker.get("text") or "unknown"
        return [name] * max(1, int(picker.get("item_count") or 1))
    # The dump writes a row per item with its drawn measurements; the name is the row's text.
    return [item["text"] if isinstance(item, dict) else item for item in (picker.get("items") or [])]


def assert_combo_matches_loaded_activities(picker, dump):
    allowed = [host_activity_label(row) for row in allowed_host_activities(dump)]
    items = combo_item_names(picker)
    extras = [item for item in items if item not in allowed]
    missing = [label for label in allowed if label not in items]
    if extras:
        raise AssertionError("extra combo row: " + extras[0])
    if missing:
        raise AssertionError("missing combo row: " + missing[0])
    if picker.get("item_count") != len(allowed):
        raise AssertionError(("item_count", picker.get("item_count"), len(allowed)))


def world_open_seat_readback(port):
    return (LANDING + f"host_world_lobby {port}\nwait_state Starting 60\nwait 15\n"
            "assert_substate Lobby\nactivate ButtonLobbyOptions\nwait 5\n"
            "assert_substate HostOptions\nactivate TabHostPageSeats\nwait 3\n"
            "assert_label LabelHostSeatName0 Open\nassert_label LabelHostSeatName1 CPU\n"
            "combo_select ComboHostSeatType2 Open\nwait 3\n"
            "assert_label LabelHostOptStatus Unsaved changes\n"
            "assert_label LabelHostSeatName2 Open\ndump_host_options\n"
            "combo_select ComboHostSeatType1 Open\nwait 3\n"
            "assert_label LabelHostSeatName1 Open\ndump_host_options\nexit\n")


def host_stun_readback(port):
    row_checks = "assert_label LabelHostNetIce " + NAT_LABEL + "\n"
    for control in ("LabelHostNetIce", "ComboHostNetIce", "LabelHostNetIceHint"):
        row_checks += checks(control, "CollectionBoxHostPageNetwork")
    reopen = ("activate ButtonHostOptBack\nwait 3\nactivate ButtonHostOptions\nwait 3\n"
              "activate TabHostPageNetwork\nwait 3\n")
    text = (LANDING + "activate ButtonMultiplayerHostGame\nwait 5\n"
            f"settext TextHostPort {port}\nactivate ButtonHostOptions\nwait 3\n"
            "activate TabHostPageNetwork\nwait 3\n" + row_checks +
            "assert_label ComboHostNetIce Automatic\ndump_host_options\n")
    for state in (NAT_STATES[1], NAT_STATES[0]):
        text += (f"combo_select ComboHostNetIce {state}\nwait 3\n" + reopen + row_checks +
                 f"assert_label ComboHostNetIce {state}\ndump_host_options\n")
    text += (f"combo_select ComboHostNetIce {NAT_STATES[1]}\nwait 3\n"
             "activate ButtonHostOptBack\nwait 3\nactivate ButtonMultiplayerCreate\nwait 15\n"
             "activate ButtonLobbyOptions\nwait 3\nactivate TabHostPageNetwork\nwait 3\n"
             "combo_select ComboHostNetIce Automatic\nwait 3\n"
             "assert_label LabelHostOptStatus End this session to change NAT traversal.\n" + row_checks +
             f"assert_label ComboHostNetIce {NAT_STATES[1]}\ndump_host_options\nexit\n")
    return text


def host_relay_readback(port):
    text = (LANDING + "activate ButtonMultiplayerHostGame\nwait 5\n"
            f"settext TextHostPort {port}\nactivate ButtonHostOptions\nwait 3\n"
            "activate TabHostPageNetwork\nwait 3\nactivate TabHostNetRouting\nwait 3\n"
            "assert_label LabelHostNetRelay Relay (TURN)\nassert_label ComboHostNetRelay Directory\n"
            "assert_label ComboHostNetIce Automatic\ndump_host_options\n")
    for state in ("Off", "Fixed", "Directory"):
        text += f"combo_select ComboHostNetRelay {state}\nwait 3\n"
        text += checks("LabelHostNetRelay", "CollectionBoxHostNetworkRouting")
        text += checks("ComboHostNetRelay", "CollectionBoxHostNetworkRouting")
        text += checks("LabelHostRelayHint", "CollectionBoxHostNetworkRouting")
        if state == "Fixed":
            for suffix, value in (("Address", "relay.example:3478"), ("User", "fixed-user"), ("Pass", "fixed-password")):
                text += f"set_text TextHostRelay{suffix} {value}\nwait 3\n"
                text += checks("TextHostRelay" + suffix, "CollectionBoxHostNetworkRouting")
            text += "assert_label TextHostRelayPass **************\n"
        text += ("activate ButtonHostOptBack\nwait 3\nactivate ButtonHostOptions\nwait 3\n"
                 "activate TabHostPageNetwork\nwait 3\nactivate TabHostNetRouting\nwait 3\n"
                 f"assert_label ComboHostNetRelay {state}\ndump_host_options\n")
    return text + "activate ButtonHostOptBack\nwait 3\nexit\n"


def connection_readback():
    text = OPTIONS + net_page("Connection")
    text += "assert_label LabelNetworkConnection Connection\nassert_label ComboNetworkConnection Automatic\n"
    text += "assert_label LabelNetworkStunServers STUN server list\n"
    text += f"assert_label TextNetworkStunServers {STUN_DEFAULT}\n"
    for control in CONNECTION_ROWS:
        text += f"assert_visible {control} 1\nassert_rect_inside {control} CollectionBoxNetPageConnection\nassert_rect_inside {control} viewport\n"
        if control != "TextNetworkStunServers":
            text += f"assert_text_fits {control}\n"
    text += "dump_player_options\n"
    for state, hint in (("Direct only", "lowest latency"), ("Relay only", "every packet"), ("Automatic", "Direct first")):
        text += f"combo_select ComboNetworkConnection {state}\nwait 3\n"
        text += net_page("Player") + net_page("Connection")
        text += f"assert_label ComboNetworkConnection {state}\nassert_label LabelNetworkConnectionHint {hint}\ndump_player_options\n"
    text += ("set_text TextNetworkStunServers stun.example:3478\n"
             "set_text TextNetworkRelayAddress personal.example:3478\n"
             "set_text TextNetworkRelayUser personal-user\n"
             "set_text TextNetworkRelayPass personal-password\nwait 3\n"
             "assert_label TextNetworkRelayPass *****************\n")
    text += net_page("Player") + net_page("Connection")
    text += ("assert_label TextNetworkStunServers stun.example:3478\n"
             "assert_label TextNetworkRelayAddress personal.example:3478\n"
             "assert_label TextNetworkRelayUser personal-user\n"
             "assert_label TextNetworkRelayPass *****************\ndump_player_options\n"
             "post_command ButtonBackToMainMenu\nwait 5\nexit\n")
    return text


def scripts(case, port, root):
    if case == "local-end-match":
        host = (LANDING + "activate ButtonMultiplayerHostGame\nwait_ms 400\n"
                "combo_select ComboHostActivity P4 Alpha Duel - Base.rte\nwait_ms 400\n"
                f"settext TextHostPort {port}\nsettext TextHostPlayers 2\n"
                "activate ButtonMultiplayerCreate\nwait_connected 2 60\nwait_remote_ready 60\n"
                "activate ButtonMultiplayerStart\n")
        client = (LANDING + "activate ButtonMultiplayerJoinGame\nwait_ms 400\n"
                  f"settext TextJoinAddress 127.0.0.1\nsettext TextJoinPort {port}\n"
                  "activate ButtonMultiplayerConnect\nwait_connected 2 60\nactivate ButtonMultiplayerReady\n")
        probes = {}
        for who in ("host", "client"):
            steps = [{"op": "wait", "service": "Running"}, {"op": "wait", "elapsed_ms": 2000}]
            if who == "host":
                steps += [{"op": "key_down", "key": "Escape"}, {"op": "key_up", "key": "Escape"},
                          {"op": "wait", "screen": "Pause", "elapsed_ms": 400},
                          menu_step("assert_enabled ButtonEndMatch 1"), menu_step("activate ButtonEndMatch")]
            steps += [{"op": "wait", "service": "Starting", "scope": "menu"},
                      {"op": "assert", "equals": {"service": "Starting"}, "scope": "menu"},
                      {"op": "signal", "name": "done", "scope": "menu"}, {"op": "finish"}]
            probes[who] = {"schema": 1, "timeout_ms": 45000, "steps": steps}
        return ({who: text + f"wait_file {probe_root(root, who) / 'done.json'} 60\nassert_substate Lobby\nexit\n"
                 for who, text in (("host", host), ("client", client))}, probes)
    if case == "prehost-visibility":
        text = (LANDING + "activate ButtonMultiplayerHostGame\nwait_ms 400\n"
                "activate ButtonHostOptions\nwait_ms 400\nactivate TabHostPageNetwork\n"
                "activate TabHostNetTuning\nwait_ms 400\n")
        for label in ("Internet: Unlisted", "Internet: Listed", "LAN only"):
            text += f"combo_select ComboHostNetVisibility {label}\nwait_ms 500\nassert_label ComboHostNetVisibility {label}\n"
        return {"host": text + "exit\n"}, {}
    if case == "repair":
        return ({who: f"wait_file {probe_root(root, who) / 'done.json'} 90\nexit\n" for who in ("host", "client")},
                {who: repair_probe(who, root) for who in ("host", "client")})
    if case == "pause":
        # Each peer's match pause menu is its own local surface, so each peer drives its own probe.
        # Both peers wait for the leaver's own signal: a peer that quits on its checks records the
        # e2e completion instead of the leave, on its own side and on its peer's.
        return ({who: f"wait_file {probe_root(root, 'client') / 'left.json'} 90\nwait_ms 2000\nexit\n"
                 for who in ("host", "client")},
                {who: pause_probe(who, root) for who in ("host", "client")})
    probe = None
    if case == "host-relay":
        text = host_relay_readback(port)
    elif case == "net-connection":
        text = connection_readback()
    elif case in ("host-stun", "host-stun-empty"):
        text = host_stun_readback(port)
    elif case == "world-open-seat":
        text = world_open_seat_readback(port)
    elif case == "host-defaults":
        text = LANDING + "activate ButtonMultiplayerHostGame\nwait 5\n"
        text += (f"settext TextHostPort {port}\nactivate ButtonHostOptions\nwait 5\n"
                 "activate TabHostPageNetwork\nwait 3\nactivate TabHostNetTuning\nwait 3\n"
                 "assert_label ComboHostNetRedundancy 6 ticks\n"
                 "assert_label TextHostNetSlowBound 3\n"
                 "assert_label ComboHostNetSlowPolicy Hand the seat to the AI and let them rejoin\n"
                 "activate ButtonHostOptApply\nwait 3\nassert_enabled ButtonHostOptApply 0\n"
                 "combo_select ComboHostNetRedundancy 7 ticks\nwait 3\n"
                 "set_text TextHostNetSlowBound 7\nwait 3\n"
                 "assert_enabled ButtonHostOptApply 1\nactivate ButtonHostOptApply\nwait 3\n"
                 "assert_enabled ButtonHostOptApply 0\ndump_host_options\n"
                 "activate ButtonHostOptBack\nwait 3\nactivate ButtonMultiplayerCreate\nwait 15\n"
                 "activate ButtonLobbyOptions\nwait 3\nactivate TabHostPageNetwork\nwait 3\nactivate TabHostNetTuning\nwait 3\n"
                 "assert_label ComboHostNetRedundancy 7 ticks\nassert_label TextHostNetSlowBound 7\n"
                 "combo_select ComboHostNetSlowPolicy Pause for them (up to 20 s)\nwait 3\n"
                 "assert_enabled TextHostNetSlowBound 0\n"
                 "combo_select ComboHostNetSlowPolicy Hand the seat to the AI and let them rejoin\nwait 3\n"
                 "assert_enabled TextHostNetSlowBound 1\ndump_host_options\nexit\n")
    elif case == "landing":
        text = LANDING + "assert_label LabelMultiplayerNamePrompt Multiplayer name:\n"
        text += checks("ButtonMultiplayerHostGame", "MultiplayerLandingPanel")
        text += "assert_visible ButtonMultiplayerCreate 0\n"
        for name in (*ORDER, ORDER[0]):
            text += f"focus_next\nassert_focus {name}\n"
        text += f"focus_previous\nassert_focus {ORDER[-1]}\ndump_host_options\n"
        # A name past the wire's 64-byte cap would die silently in the hello encode, so the host
        # button's create refuses it and the landing's status line says why.
        text += ("settext TextMultiplayerName " + "N" * (DISPLAY_NAME_MAX_BYTES + 1) + "\n"
                 "activate ButtonMultiplayerHostGame\nwait 5\nassert_substate HostSetup\n"
                 "activate ButtonMultiplayerCreate\nwait 5\ndump_host_options\n"
                 "assert_substate Landing\n"
                 "assert_label LabelMultiplayerLandingStatus limited to 64 bytes\n"
                 "exit\n")
    elif case == "net-resume":
        # The resume entry sits on the landing panel and opens a screen of its own. With no resumable
        # match on this private runtime the list is empty, the status says so in its own words and
        # Resume cannot be pressed - the state a player meets before any match has been checkpointed.
        text = LANDING + checks("ButtonMultiplayerResumeGame", "MultiplayerLandingPanel")
        text += "assert_label ButtonMultiplayerResumeGame Resume Match\n"
        text += "activate ButtonMultiplayerResumeGame\nwait 5\nassert_substate ResumeSetup\n"
        for name in RESUME_ROWS:
            text += checks(name, "MultiplayerResumePanel") if name != "ListResumeMatches" else \
                (f"assert_visible {name} 1\nassert_rect_inside {name} MultiplayerResumePanel\n"
                 f"assert_rect_inside {name} viewport\n")
        text += "assert_label LabelResumeTitle R E S U M E   M A T C H\n"
        text += "assert_label LabelResumeSelected Select a match to restart it\n"
        text += "assert_label LabelResumeStatus No match here can be restarted\n"
        text += "assert_enabled ButtonResumeStart 0\n"
        text += "assert_enabled ButtonResumeBack 1\n"
        # The rejoin prompt is not an offer here: this runtime has no ticket, so it stays off the screen.
        text += "activate ButtonResumeBack\nwait 5\nassert_substate Landing\n"
        text += "assert_visible ButtonMultiplayerReconnect 0\nassert_visible ButtonMultiplayerCancelReconnect 0\n"
        text += "exit\n"
    elif case == "settings":
        text = "wait 40\nactivate ButtonMainToOptions\nwait 8\nassert_screen SettingsScreen\nassert_visible TabVideoSettings 1\n"
        for tab in PAGES:
            text += f"activate Tab{tab}Settings\nwait 3\nassert_visible CollectionBox{tab}Settings 1\n"
            text += checks(f"Tab{tab}Settings", "CollectionBoxSettingsBase") + "dump_player_options\n"
        text += "post_command ButtonBackToMainMenu\nwait 5\nassert_screen MainScreen\nexit\n"
    elif case == "pages":
        # Every settings page reached by its own name, so a review sees the rows the options program adds.
        text = OPTIONS
        for page in PAGES:
            text += f"select_settings_page {page}\nwait 3\nassert_settings_page {page}\n"
            text += f"assert_visible CollectionBox{page}Settings 1\n"
            text += checks(f"Tab{page}Settings", "CollectionBoxSettingsBase")
            if page == "Gameplay":
                # Enter drops, Down moves, Enter commits: the same pad/keyboard path the host combos use.
                text += ("focus ComboBrainlessHumansSpectate\n"
                         "key_down Return\nwait 2\nkey_up Return\nwait 3\n"
                         "key_down Down\nwait 2\nkey_up Down\nwait 3\n"
                         "key_down Return\nwait 2\nkey_up Return\nwait 3\n"
                         "assert_label ComboBrainlessHumansSpectate End the match\n")
            text += "dump_player_options\n"
        text += "post_command ButtonBackToMainMenu\nwait 5\nassert_screen MainScreen\nexit\n"
    elif case == "network":
        # The saved preferences reach the player page, an edit on the page reaches the settings, and the
        # lobby's own name box shows what the page saved. A page switch is a page leave: the typed
        # name commits when the selector moves to chat and back.
        text = OPTIONS + "select_settings_page Network\nwait 3\nassert_settings_page Network\nassert_settings_page Network:Player\n"
        text += "assert_visible CollectionBoxNetworkSettings 1\n"
        text += checks("TabNetworkSettings", "CollectionBoxSettingsBase")
        for tab in NETWORK_TABS:
            text += checks(tab, "CollectionBoxNetworkSettings")
        for index, box in enumerate(NETWORK_BOXES):
            text += f"assert_visible {box} {1 if index == 0 else 0}\n"
        for control in NETWORK_ROWS:
            text += checks(control, NETWORK_PAGE_BOX)
        text += ("assert_label TextNetworkDisplayName " + NETWORK_SEED["NetworkDisplayName"] + "\n"
                 "assert_label TextNetworkIdleWait " + NETWORK_SEED["NetworkHostIdleWaitMinutes"] + "\n"
                 "assert_label TextNetworkPathHorizon " + NETWORK_SEED["NetworkPathHorizonTicks"] + "\n"
                 "assert_label ComboMatchStatusWidget " + NETWORK_SEED["NetworkMatchStatusMode"] + "\n")
        # The saved policy is automatic here, so the fixed-delay row is not on the page at all.
        for control in NETWORK_FIXED_ROWS:
            text += f"assert_visible {control} 0\n"
        text += "dump_player_options\n"
        text += "set_text TextNetworkDisplayName " + NETWORK_SAVED["NetworkDisplayName"] + "\n"
        text += "select_settings_page Network:Chat\nwait 3\nassert_settings_page Network:Chat\n"
        text += "assert_visible CollectionBoxNetPagePlayer 0\nselect_settings_page Network:Player\nwait 3\n"
        text += "assert_label TextNetworkDisplayName " + NETWORK_SAVED["NetworkDisplayName"] + "\n"
        text += "post_command RadioNetworkDelayFixed\nwait 3\n"
        for control in NETWORK_FIXED_ROWS:
            text += checks(control, NETWORK_PAGE_BOX)
        text += (f"assert_label LabelNetworkFixedDelayHint frames, 0-{MAX_INPUT_DELAY_FRAMES}\n"
                 "set_text TextNetworkFixedDelay " + NETWORK_SAVED["NetworkInputDelayFrames"] + "\n"
                 "set_text TextNetworkIdleWait " + NETWORK_SAVED["NetworkHostIdleWaitMinutes"] + "\n"
                 "set_text TextNetworkPathHorizon " + NETWORK_SAVED["NetworkPathHorizonTicks"] + "\n"
                 "post_command CheckboxNetworkAutoRepair\npost_command CheckboxNetworkToasts\n"
                 "post_command CheckboxNetworkPrediction\npost_command CheckboxNetworkDiagnostics\nwait 3\ndump_player_options\n"
                 "post_command ButtonBackToMainMenu\nwait 5\nassert_screen MainScreen\n")
        text += LANDING + "assert_label TextMultiplayerName " + NETWORK_SAVED["NetworkDisplayName"] + "\n"
        text += checks("LabelMultiplayerNamePrompt", "MultiplayerLandingPanel")
        text += checks("TextMultiplayerName", "MultiplayerLandingPanel")
        text += "dump_host_options\n"
        # The host screen names the saved policy beside its delay box; the page flipped it to Fixed
        # with 7 frames, so the box pre-fills the override it would send.
        text += ("activate ButtonMultiplayerHostGame\nwait 5\nassert_substate HostSetup\n"
                 "assert_visible LabelHostInputDelayPolicy 1\ndump_host_options\n"
                 "assert_label LabelHostInputDelayPolicy (fixed, 7)\n"
                 "assert_label TextHostInputDelay 7\nassert_enabled TextHostInputDelay 1\n"
                 + checks("LabelHostInputDelayPolicy", "MultiplayerHostPanel") +
                 "post_command ButtonHostBack\nwait 4\nassert_substate Landing\ndump_host_options\nexit\n")
    elif case == "net-chat":
        # Every chat row is read where it is drawn; the muted-players button stays disabled with its
        # reason until a muted-players store exists.
        text = OPTIONS + net_page("Chat")
        for control in ("CheckboxNetworkChatVisible", "CheckboxNetworkChatSound", "LabelNetworkChatScope",
                        "ComboNetworkChatScope", "CheckboxNetworkChatNotify", "LabelNetworkChatTextSize",
                        "ComboNetworkChatTextSize", "LabelNetworkChatKey", "TextNetworkChatKey",
                        "ButtonNetMutedPlayers", "LabelNetMutedReason"):
            text += checks(control, "CollectionBoxNetPageChat")
        text += ("assert_enabled ButtonNetMutedPlayers 0\n"
                 "assert_label LabelNetMutedReason managed in the match\n"
                 "assert_label ComboNetworkChatScope " + CHAT_SEED["NetworkChatDefaultScope"] + "\n"
                 "assert_label ComboNetworkChatTextSize " + CHAT_SEED["NetworkChatTextSize"] + "\n"
                 "dump_player_options\n"
                 "post_command CheckboxNetworkChatVisible\npost_command CheckboxNetworkChatNotify\nwait 3\n"
                 "post_command ButtonBackToMainMenu\nwait 5\nassert_screen MainScreen\nexit\n")
    elif case == "net-recovery":
        # No recovery record survives a fresh run: the landing visit runs the ticket scan, the rejoin
        # row says what it found and both actions stay off.
        text = LANDING + "post_command ButtonBackToMain\nwait 5\n" + OPTIONS + net_page("Recovery")
        for control in ("CheckboxNetworkAutoReconnect", "CheckboxNetworkOfferRejoin", "LabelNetLastHostTitle",
                        "LabelNetLastHost", "LabelNetRecoveryTitle", "LabelNetRecoveryRecord",
                        "LabelNetRecoveryStatusTitle", "LabelNetRecoveryStatus",
                        "ButtonNetRejoin", "ButtonNetCancelRecovery"):
            text += checks(control, "CollectionBoxNetPageRecovery")
        text += ("assert_enabled ButtonNetRejoin 0\nassert_enabled ButtonNetCancelRecovery 0\n"
                 "assert_label LabelNetRecoveryRecord No recovery record\n"
                 "assert_label LabelNetRecoveryStatusTitle Status:\n"
                 "dump_player_options\n"
                 "post_command CheckboxNetworkAutoReconnect\npost_command CheckboxNetworkOfferRejoin\nwait 3\n"
                 "post_command ButtonBackToMainMenu\nwait 5\nassert_screen MainScreen\nexit\n")
    elif case == "net-files":
        # The autosave rows mirror the stored host option; the diagnostics box commits a folder and
        # the replay pref toggles. The folder buttons are layout-checked, not clicked - they own real
        # OS side effects (a shell window, a clipboard write) a readback run must not take.
        text = OPTIONS + net_page("Files")
        for control in ("LabelNetAutosaveTitle", "LabelNetAutosave", "LabelNetAutosaveHost",
                        "LabelNetAutosaveIntTitle", "LabelNetAutosaveInterval", "LabelNetAutosaveIntervalHost",
                        "LabelNetAutosavesKeptTitle", "TextNetworkAutosavesKept", "LabelNetAutosavesKeptHint",
                        "LabelNetAutosaveInfo", "ButtonNetOpenAutosaves", "ButtonNetCopyAutosavesPath",
                        "LabelNetDiagDirTitle", "TextNetworkDiagDir", "ButtonNetOpenDiagnostics",
                        "ButtonNetCopyDiagPath", "ButtonNetSaveDiagnostics", "CheckboxNetworkRecordReplays"):
            text += checks(control, "CollectionBoxNetPageFiles")
        text += ("assert_label LabelNetAutosave Enabled\n"
                 "assert_label LabelNetAutosaveInterval 45 s\n"
                 "assert_label LabelNetAutosaveHost Set by the host\n"
                 "assert_label LabelNetAutosaveIntervalHost Set by the host\n"
                 "assert_label LabelNetAutosavesKeptTitle Autosaves kept:\n"
                 "assert_label LabelNetAutosavesKeptHint " + AUTOSAVES_KEPT_HINT + "\n"
                 "assert_label TextNetworkAutosavesKept " + FILES_SEED["NetworkAutosavesKept"] + "\n"
                 "assert_enabled ButtonNetOpenAutosaves 1\nassert_enabled ButtonNetCopyAutosavesPath 1\n"
                 "assert_enabled ButtonNetOpenDiagnostics 1\nassert_enabled ButtonNetCopyDiagPath 1\n"
                 "assert_enabled ButtonNetSaveDiagnostics 1\n"
                 # A count outside the bounds keeps the stored one; the page reverts the box to it.
                 "set_text TextNetworkAutosavesKept 11\n"
                 "assert_label TextNetworkAutosavesKept " + FILES_SEED["NetworkAutosavesKept"] + "\n"
                 "set_text TextNetworkAutosavesKept " + FILES_SAVED["NetworkAutosavesKept"] + "\n"
                 "assert_label TextNetworkAutosavesKept " + FILES_SAVED["NetworkAutosavesKept"] + "\n"
                 "set_text TextNetworkDiagDir " + FILES_SAVED["NetworkDiagnosticsDirectory"] + "\n"
                 "post_command CheckboxNetworkRecordReplays\nwait 3\ndump_player_options\n"
                 "post_command ButtonBackToMainMenu\nwait 5\nassert_screen MainScreen\nexit\n")
    elif case == "net-internet":
        # A refused pin or URL keeps the stored value and says why on the page; a valid pair saves.
        text = OPTIONS + net_page("Internet")
        for control in ("LabelNetDirUrl", "TextNetworkDirUrl", "LabelNetDirUrlHint", "LabelNetDirPin",
                        "TextNetworkDirPin", "LabelNetDirStatusTitle", "LabelNetDirStatus",
                        "ButtonNetReplays", "ButtonNetConnDetails", "ButtonNetNatRelay",
                        "LabelNetInternetReason"):
            text += checks(control, "CollectionBoxNetPageInternet")
        text += ("assert_enabled ButtonNetReplays 0\nassert_enabled ButtonNetConnDetails 0\n"
                 "assert_enabled ButtonNetNatRelay 0\nassert_label LabelNetDirStatus Configured\n"
                 "assert_label LabelNetDirUrlHint " + INTERNET_HINT + "\n"
                 "assert_label ButtonNetNatRelay NAT setup\n"
                 "assert_label LabelNetInternetReason " + INTERNET_REASON + "\n"
                 "set_text TextNetworkDirPin nothex\n"
                 "assert_label LabelNetInternetError 64 hexadecimal\n"
                 "assert_label TextNetworkDirPin aaaa\n"
                 "set_text TextNetworkDirUrl ftp://bogus\n"
                 "assert_label LabelNetInternetError host[:port]\n"
                 "assert_label TextNetworkDirUrl " + INTERNET_SEED["SessionDirectoryUrl"] + "\n"
                 "set_text TextNetworkDirUrl " + INTERNET_SAVED["SessionDirectoryUrl"] + "\n"
                 "assert_label TextNetworkDirUrl " + INTERNET_SAVED["SessionDirectoryUrl"] + "\n"
                 "set_text TextNetworkDirPin " + INTERNET_SAVED["SessionDirectoryCertSha256"] + "\n"
                 "assert_label TextNetworkDirPin " + INTERNET_SAVED["SessionDirectoryCertSha256"] + "\n"
                 "dump_player_options\n"
                 "post_command ButtonBackToMainMenu\nwait 5\nassert_screen MainScreen\nexit\n")
    elif case == "misc-page":
        # The Misc box keeps its own rows; the match-status rows moved to the network player page.
        text = OPTIONS + "select_settings_page Misc\nwait 3\nassert_settings_page Misc\n"
        text += "assert_visible CollectionBoxMiscSettings 1\n"
        for control in MISC_ROWS:
            # A slider carries no caption, so it answers the placement checks only.
            text += checks(control, "CollectionBoxMiscSettings") if control != "SliderSceneBackgroundAutoScale" else \
                f"assert_visible {control} 1\nassert_rect_inside {control} CollectionBoxMiscSettings\n"
        text += "dump_player_options\npost_command ButtonBackToMainMenu\nwait 5\nassert_screen MainScreen\nexit\n"
    elif case == "lobby-name":
        # The name box starts from the saved name, and the name it is hosted with is saved again.
        text = LANDING + "assert_label TextMultiplayerName " + NETWORK_SEED["NetworkDisplayName"] + "\n"
        text += ("dump_host_options\nsettext TextMultiplayerName Recon7\n"
                 "activate ButtonMultiplayerHostGame\nwait 5\n"
                 "assert_visible LabelHostInputDelayPolicy 1\nassert_label LabelHostInputDelayPolicy (auto)\n"
                 "dump_host_options\n"
                 # The seeded policy is automatic: the box reads the policy, not a stale frame count.
                 "assert_label TextHostInputDelay auto\nassert_enabled TextHostInputDelay 0\n"
                 f"settext TextHostPort {port}\nsettext TextHostPlayers 2\n"
                 "activate ButtonMultiplayerCreate\nwait 15\nassert_substate Lobby\n"
                 "dump_host_options\n"
                 f"set_share_address {WIDE_SHARE_HOST}\nwait 5\n"
                 "dump_host_options\nexit\n")
    elif case == "net-host-left":
        done = probe_root(root, "host") / "done.json"
        host = (LANDING + "activate ButtonMultiplayerHostGame\nwait 5\n"
                "combo_select ComboHostActivity P4 Alpha Duel - Base.rte\nwait 5\n"
                f"settext TextHostPort {port}\nsettext TextHostPlayers 2\n"
                "activate ButtonMultiplayerCreate\nwait_connected 2 60\nwait_remote_ready 60\n"
                "activate ButtonMultiplayerStart\n"
                f"wait_file {done} 90\nwait_ms 500\ndump_lobby\ndump_host_options\nexit\n")
        client = (LANDING + "activate ButtonMultiplayerJoinGame\nwait 5\n"
                  f"settext TextJoinAddress 127.0.0.1\nsettext TextJoinPort {port}\n"
                  "activate ButtonMultiplayerConnect\nwait_connected 2 60\nwait 5\n"
                  "activate ButtonMultiplayerReady\n"
                  f"wait_file {done} 90\nwait_ms 1000\n"
                  "assert_status The host left the match\n"
                  "assert_substate Landing\nassert_visible LabelMultiplayerLandingStatus 1\n"
                  "assert_label LabelMultiplayerLandingStatus The host left the match\n"
                  "assert_text_fits LabelMultiplayerLandingStatus\ndump_lobby\ndump_host_options\nexit\n")
        return {"host": host, "client": client}, {"host": {"schema": 1, "timeout_ms": 90000, "steps": [
            {"op": "wait", "service": "Running", "screen": "Gameplay", "sim_at_least": 150},
            {"op": "wait", "elapsed_ms": 500},
            {"op": "assert", "equals": {"paused": False, "service": "Running"}},
            {"op": "key_down", "key": "Escape"}, {"op": "key_up", "key": "Escape"},
            {"op": "wait", "screen": "Pause"}, menu_step("activate ButtonLeaveMatch"),
            {"op": "wait", "screen": "PauseLeaveConfirm"}, menu_step("activate ButtonLeaveConfirm"),
            {"op": "wait", "scope": "menu", "elapsed_ms": 500},
            {"op": "signal", "name": "done", "scope": "menu"}, {"op": "finish"}]}}
    elif case == "net-activity":
        # Fresh host setup uses Skirmish Defense; the keyboard anchor is the row above the picked one.
        # A vanished pick needs a module unload the menu harness cannot drive; the native
        # host_request_fallback row covers the empty-list Base.rte request fields instead.
        # The keyboard commits the activity and scene combos and the mouse the mode combo; Create
        # uses those picks, not a later mouse Select.
        host = (LANDING + "activate ButtonMultiplayerHostGame\nwait 5\n"
                "assert_visible LabelHostActivity 1\nassert_label LabelHostActivity Activity\n"
                "assert_label ComboHostActivity Skirmish Defense - Base.rte\n"
                "assert_text_fits ComboHostActivity\n"
                "dump_host_options\n"
                "assert_visible LabelHostScene 1\nassert_label LabelHostScene Scene\n"
                "assert_visible ComboHostScene 1\n"
                "assert_text_fits ComboHostScene\n"
                "assert_visible ComboHostMode 1\nassert_label ComboHostMode PvP\n"
                "assert_label LabelHostInfo Grasslands - PvP\n"
                "combo_select ComboHostActivity Persistent World - Base.rte\nwait 3\n"
                "focus ComboHostActivity\n"
                "key_down Return\nwait 2\nkey_up Return\nwait 3\ndump_host_options\n"
                "key_down Down\nwait 2\nkey_up Down\nwait 3\n"
                "key_down Return\nwait 2\nkey_up Return\nwait 3\n"
                "assert_label ComboHostActivity Brain vs Brain - Base.rte\ndump_host_options\n"
                "focus ComboHostScene\n"
                "key_down Return\nwait 2\nkey_up Return\nwait 3\ndump_host_options\n"
                "key_down Down\nwait 2\nkey_up Down\nwait 3\n"
                "key_down Return\nwait 2\nkey_up Return\nwait 3\n"
                "assert_label ComboHostScene Fredeleig Bunkers\ndump_host_options\n"
                # combo_drop is the same panel-level click a user makes; the dumped capture shows the
                # list open. combo_select picks the row by its text the way a click on it would.
                "combo_drop ComboHostMode\nwait 3\ndump_host_options\n"
                "combo_select ComboHostMode Co-op PvE\nwait 3\n"
                "assert_label ComboHostMode Co-op PvE\n"
                "assert_label LabelHostInfo Fredeleig Bunkers - Co-op PvE\ndump_host_options\n"
                f"settext TextHostPort {port}\nsettext TextHostPlayers 2\n"
                "activate ButtonMultiplayerCreate\nwait 15\nassert_substate Lobby\n"
                "wait_connected 2\nwait 12\n"
                "assert_label LabelLobbyMatch Brain vs Brain - Base.rte\n"
                "assert_label LabelLobbyMatchMode Fredeleig Bunkers - Co-op PvE\n"
                "assert_text_fits LabelLobbyMatch\nassert_text_fits LabelLobbyMatchMode\n"
                "assert_text_fits LabelLobbyPlayersHeader\n"
                "assert_text_fits LabelLobbyPlayer0\nassert_text_fits LabelLobbyPlayer1\n"
                # The host's options panel edits the adopted config; its Rules page carries the
                # picked activity/mode and the L33 row the ledger names.
                "activate ButtonLobbyOptions\nwait 5\nassert_substate HostOptions\n"
                "assert_label LabelHostOptionsTitle H O S T   O P T I O N S\n"
                # H09/H10: the host's own seat is never kickable, whoever else is in the lobby.
                "activate ButtonHostSeatDetails0\nwait 3\nassert_visible HostSeatDialog 1\n"
                "assert_enabled ButtonHostSeatDlgKick 0\nassert_enabled ButtonHostSeatDlgBan 0\n"
                "assert_label LabelHostSeatDlgActionHint The host's own seat is never kicked or banned.\n"
                "activate ButtonHostSeatDlgClose\nwait 3\nassert_visible HostSeatDialog 0\n"
                # H34 on the two-peer fixture: the adopted config names both seated humans, the
                # lobby sits LAN only, and the bound port refuses the edit mid-session.
                "activate TabHostPageNetwork\nwait 3\nactivate TabHostNetTuning\nwait 3\nassert_visible CollectionBoxHostPageNetwork 1\n"
                "assert_label LabelHostNetMode Host mode: Playing - capacity 2 - humans seated 2\n"
                "assert_label ComboHostNetVisibility LAN only\n"
                "set_text TextHostNetPort 40000\nwait 3\n"
                "assert_label LabelHostOptStatus End the session to change the port\n"
                f"assert_label TextHostNetPort {port}\n"
                # H25: a hosted lobby is still Starting, so even on a connected session the repair
                # button stays off with the live-session reason until a match is Running.
                "activate TabHostPageRecovery\nwait 3\nassert_visible CollectionBoxHostPageRecovery 1\n"
                "assert_enabled ButtonHostRecRepairNow 0\n"
                "assert_label LabelHostRecRepairHint Repair needs a live match session\n"
                "activate TabHostPageRules\nwait 3\nassert_visible CollectionBoxHostPageRules 1\n"
                "assert_label LabelHostRulesBrainless When every human brain is lost\n"
                "assert_label ComboHostRulesBrainless Keep playing, humans spectate\n"
                "assert_enabled ComboHostRulesBrainless 1\n"
                "assert_label ComboHostRulesActivity Brain vs Brain - Base.rte\n"
                "assert_label ComboHostRulesMode Co-op PvE\n"
                "dump_host_options\n"
                # The live republish: the host's Apply moves every peer's adopted config to the
                # next revision. The status line reads the acknowledge, then the client's Details
                # below reads the new value off its own mirror.
                "combo_select ComboHostRulesBrainless End the match\nwait 3\n"
                "assert_label ComboHostRulesBrainless End the match\n"
                "activate ButtonHostOptApply\nwait 5\ndump_host_options\n"
                "activate ButtonHostOptBack\nwait 5\nassert_substate Lobby\n"
                "dump_lobby\ndump_host_options\n"
                # H09/H10: the lobby publishes the seat's admission row, so the press queues a real
                # selection and the setup worker's pump applies it - the status line carries the
                # queued word first and then the drained result. It runs last in the script because
                # the peer it removes has to finish its own read-only pass first.
                "wait_ms 16000\n"
                "activate ButtonLobbyOptions\nwait 5\nassert_substate HostOptions\n"
                "activate ButtonHostSeatDetails1\nwait 3\nassert_visible HostSeatDialog 1\n"
                "assert_enabled ButtonHostSeatDlgKick 1\nassert_enabled ButtonHostSeatDlgBan 1\n"
                "activate ButtonHostSeatDlgKick\nwait 10\n"
                "assert_visible HostSeatDialog 0\n"
                "assert_label LabelHostOptStatus Kick: Ok\n"
                # The open seat is published as a new config revision, so the panel re-seeds its draft
                # and the seat's name column reads the unseated name instead of the removed player's.
                "wait 10\nassert_label LabelHostSeatName1 Client 2\n"
                "dump_host_options\n"
                "activate ButtonHostOptBack\nwait 5\nassert_substate Lobby\n"
                # The kicked seat is open again, so its lobby row carries the unseated name the
                # Seats page shows for an open seat, never the removed player's.
                "dump_lobby\nassert_label LabelLobbyPlayer1 Client 2\nwait 600\nexit\n")
        client = (LANDING + "settext TextMultiplayerName Joiner\n"
                  "activate ButtonMultiplayerJoinGame\nwait 10\n"
                  "settext TextJoinAddress 127.0.0.1\n"
                  f"settext TextJoinPort {port}\nactivate ButtonMultiplayerConnect\n"
                  "wait_connected 2\nwait_activity Brain vs Brain\nwait 12\n"
                  "assert_substate Lobby\n"
                  "assert_label LabelLobbyMatch Brain vs Brain - Base.rte\n"
                  "assert_label LabelLobbyMatchMode Fredeleig Bunkers - Co-op PvE\n"
                  "assert_text_fits LabelLobbyMatch\nassert_text_fits LabelLobbyMatchMode\n"
                  "assert_text_fits LabelLobbyPlayersHeader\n"
                  "assert_text_fits LabelLobbyPlayer0\nassert_text_fits LabelLobbyPlayer1\n"
                  # The client's Options button opens the same adopted config as a read-only
                  # details view: every edit control is disabled, the L33 row reads identically,
                  # and the title names what it is.
                  "activate ButtonLobbyOptions\nwait 5\nassert_substate HostOptions\n"
                  "assert_label LabelHostOptionsTitle M A T C H   D E T A I L S\n"
                  "activate TabHostPageRules\nwait 3\nassert_visible CollectionBoxHostPageRules 1\n"
                  "assert_label LabelHostRulesBrainless When every human brain is lost\n"
                  "assert_label ComboHostRulesBrainless Keep playing, humans spectate\n"
                  "assert_enabled ComboHostRulesBrainless 0\n"
                  "assert_enabled ComboHostRulesActivity 0\n"
                  "assert_label ComboHostRulesActivity Brain vs Brain - Base.rte\n"
                  "assert_label ComboHostRulesMode Co-op PvE\n"
                  "assert_enabled ButtonHostOptApply 0\n"
                  "dump_host_options\n"
                  "activate ButtonHostOptBack\nwait 5\nassert_substate Lobby\n"
                  # The host applies its L33 edit near the end of its own script; this wait covers
                  # the republish so the re-opened Details reads revision N+1's value off the
                  # client's adopted mirror.
                  "wait_ms 8000\n"
                  "activate ButtonLobbyOptions\nwait 5\nassert_substate HostOptions\n"
                  "assert_label LabelHostOptionsTitle M A T C H   D E T A I L S\n"
                  "activate TabHostPageRules\nwait 3\nassert_visible CollectionBoxHostPageRules 1\n"
                  "assert_label ComboHostRulesBrainless End the match\n"
                  "dump_host_options\n"
                  # A client's Details dialog is read-only: the host's own seat and every other
                  # seat keep Kick and Ban off - moderation is never the client's call.
                  "activate ButtonHostSeatDetails1\nwait 3\nassert_visible HostSeatDialog 1\n"
                  "assert_enabled ButtonHostSeatDlgKick 0\nassert_enabled ButtonHostSeatDlgBan 0\n"
                  "activate ButtonHostSeatDlgClose\nwait 3\nassert_visible HostSeatDialog 0\n"
                  "activate ButtonHostOptBack\nwait 5\nassert_substate Lobby\n"
                  "dump_lobby\ndump_host_options\n"
                  # The host kicks this peer once its own pass is done: the lobby that vanishes has
                  # to name the removal instead of reading as a network fault.
                  "wait_state Failed 60\n"
                  "assert_status The host removed you from this session\n"
                  "dump_lobby\nexit\n")
        return {"host": host, "client": client}, {}
    elif case == "net-options":
        # Two real peers: the host's saved session options ride the lobby config onto both rosters. A
        # match end quits e2e peers outright, so the host's own pause-menu leave is what pauses the
        # activity and lets its menu loop run the dump while the roster is still up.
        return ({"host": f"dump_lobby\nwait_file {probe_root(root, 'host') / 'left.json'} 90\nwait_ms 2000\nexit\n",
                 "client": f"wait_file {probe_root(root, 'host') / 'left.json'} 90\nwait_ms 2000\nexit\n"},
                {"host": {"schema": 1, "timeout_ms": 90000, "steps": [
                    {"op": "wait", "sim_at_least": 150},
                    {"op": "assert", "equals": {"service": "Running", "paused": False}, "sim_at_least": 150},
                    {"op": "key_down", "key": "Escape"}, {"op": "key_up", "key": "Escape"},
                    {"op": "wait", "screen": "Pause"}, menu_step("dump_host_options"),
                    {"op": "signal", "name": "done"},
                    menu_step("activate ButtonLeaveMatch"), {"op": "wait", "screen": "PauseLeaveConfirm"},
                    menu_step("activate ButtonLeaveConfirm"), {"op": "wait", "elapsed_ms": 1500},
                    {"op": "signal", "name": "left"}, {"op": "finish"}]}})
    elif case == "combo-fit":
        # Reached by the tab control so the measurement runs on a build that has no page op yet.
        text = (OPTIONS + "activate TabVideoSettings\nwait 3\nassert_visible ComboPresetResolution 1\n"
                "assert_text_fits ComboPresetResolution\ndump_player_options\nexit\n")
    elif case == "lobby":
        # The host starts the match, so its lobby hides the ready button the joining peers get.
        text = host_lobby(port) + "assert_visible ButtonMultiplayerReady 0\n"
        text += checks("ButtonMultiplayerLeave", "MultiplayerLobbyPanel")
        # The Seats row is the host's own disabled control: it must read disabled and sit in the
        # panel. The dump lands before the header and readiness asserts so the capture exists either way.
        text += checks("ButtonMultiplayerModerate", "MultiplayerLobbyPanel")
        text += "assert_enabled ButtonMultiplayerModerate 0\n"
        text += "dump_host_options\n"
        text += checks("LabelLobbyPlayersHeader", "MultiplayerLobbyPanel")
        text += "assert_enabled ButtonMultiplayerStart 0\n"
        # H01-H35: the six host-options pages behind the lobby's Options button. Each tab shows its
        # own collection box, every visited control answers the fit checks, and the dump rows land
        # on each page so the capture names the page's whole surface at this size.
        text += "activate ButtonLobbyOptions\nwait 5\nassert_substate HostOptions\n"
        text += "assert_visible MultiplayerHostOptionsPanel 1\n"
        text += "assert_label LabelHostOptionsTitle H O S T   O P T I O N S\n"
        text += checks("TabHostPageSeats", "MultiplayerHostOptionsPanel")
        text += checks("TabHostPageRules", "MultiplayerHostOptionsPanel")
        text += checks("TabHostPageNetwork", "MultiplayerHostOptionsPanel")
        text += checks("TabHostPageRecovery", "MultiplayerHostOptionsPanel")
        text += checks("TabHostPageFiles", "MultiplayerHostOptionsPanel")
        text += checks("TabHostPageSession", "MultiplayerHostOptionsPanel")
        # H01-H06 Seats: the header, the seated row, and the footer the apply row sits in.
        text += "assert_visible CollectionBoxHostPageSeats 1\n"
        text += checks("ComboHostSeatPlayers", "CollectionBoxHostPageSeats")
        text += checks("LabelHostSeatHeader", "CollectionBoxHostPageSeats")
        text += checks("LabelHostSeatName0", "CollectionBoxHostPageSeats")
        text += checks("ComboHostSeatType0", "CollectionBoxHostPageSeats")
        text += checks("ComboHostSeatTeam0", "CollectionBoxHostPageSeats")
        text += checks("LabelHostSeatDelay0", "CollectionBoxHostPageSeats")
        text += checks("LabelHostSeatState0", "CollectionBoxHostPageSeats")
        text += checks("ButtonHostSeatDetails0", "CollectionBoxHostPageSeats")
        # The lobby seats two human slots, so the second row is an open seat and the third is the
        # closed tail the host reopens from; rows past the roster stay hidden.
        text += checks("ComboHostSeatType1", "CollectionBoxHostPageSeats")
        text += checks("ComboHostSeatType2", "CollectionBoxHostPageSeats")
        text += "assert_visible ComboHostSeatType6 0\n"
        # A seated human's kind never moves: the host's own row is locked, and the open row's edit
        # is refused with the reason in the status line instead of silently dropping the seat.
        text += "assert_enabled ComboHostSeatType0 0\n"
        text += ("combo_select ComboHostSeatType1 CPU\nwait 3\n"
                 "assert_label LabelHostOptStatus A seated player is never dropped by an options edit\n")
        # H03: a two-peer lobby has no free peer id, so the closed tail refuses a human seat with
        # the reason in the status line, then accepts the peerless CPU seat the same row offers.
        text += ("combo_select ComboHostSeatType2 Open\nwait 3\n"
                 "assert_label LabelHostOptStatus No free peer seat\n")
        text += ("combo_select ComboHostSeatType2 CPU\nwait 3\n"
                 "assert_label LabelHostOptStatus Unsaved changes\n"
                 "assert_label LabelHostSeatState2 CPU / Skill\n")
        text += "dump_host_options\n"
        # H04-H11: the seat's Details dialog - the seat's identity, the reclaim clock's line, the
        # applicant row and the moderation actions, Kick and Ban among them.
        text += ("activate ButtonHostSeatDetails0\nwait 3\nassert_visible HostSeatDialog 1\n")
        for control in ("LabelHostSeatDlgName", "LabelHostSeatDlgSeat", "LabelHostSeatDlgTeam",
                        "LabelHostSeatDlgState", "LabelHostSeatDlgReclaim", "LabelHostSeatDlgApplicants",
                        "ButtonHostSeatDlgApplicant", "ButtonHostSeatDlgWait", "ButtonHostSeatDlgApprove",
                        "ButtonHostSeatDlgCancel", "ButtonHostSeatDlgKick", "ButtonHostSeatDlgBan",
                        "LabelHostSeatDlgActionHint", "LabelHostSeatDlgStatus", "ButtonHostSeatDlgClose"):
            text += checks(control, "HostSeatDialog")
        # H09/H10: the host's own seat is never kickable - the row stays pressable-looking but off.
        text += ("assert_enabled ButtonHostSeatDlgKick 0\nassert_enabled ButtonHostSeatDlgBan 0\n"
                 "dump_host_options\nactivate ButtonHostSeatDlgClose\nwait 3\n"
                 "assert_visible HostSeatDialog 0\n")
        # H07-H20 Rules: the L33 row keeps the ledger's exact label and pair of answers.
        text += "activate TabHostPageRules\nwait 3\nassert_visible CollectionBoxHostPageRules 1\n"
        text += "assert_label LabelHostOptionsTitle M A T C H   R U L E S\n"
        text += checks("ComboHostRulesActivity", "CollectionBoxHostPageRules")
        text += checks("ComboHostRulesScene", "CollectionBoxHostPageRules")
        text += checks("ComboHostRulesMode", "CollectionBoxHostPageRules")
        text += "assert_visible LabelHostRulesBrainless 1\n"
        text += "assert_rect_inside LabelHostRulesBrainless CollectionBoxHostPageRules\n"
        text += "assert_label LabelHostRulesBrainless When every human brain is lost\n"
        text += "assert_label ComboHostRulesBrainless Keep playing, humans spectate\n"
        text += ("combo_select ComboHostRulesBrainless End the match\nwait_ms 500\n"
                 "assert_label ComboHostRulesBrainless End the match\n"
                 "combo_select ComboHostRulesBrainless Keep playing, humans spectate\nwait_ms 500\n"
                 "assert_label ComboHostRulesBrainless Keep playing, humans spectate\n")
        text += checks("ComboHostRulesBrainless", "CollectionBoxHostPageRules")
        text += "dump_host_options\n"
        # H21-H24 Network.
        text += "activate TabHostPageNetwork\nwait 3\nactivate TabHostNetTuning\nwait 3\nassert_visible CollectionBoxHostPageNetwork 1\n"
        text += "assert_label LabelHostOptionsTitle N E T W O R K   O P T I O N S\n"
        text += checks("ComboHostNetPolicy", "CollectionBoxHostPageNetwork")
        text += checks("LabelHostNetRedundancy", "CollectionBoxHostPageNetwork")
        text += checks("ComboHostNetRedundancy", "CollectionBoxHostPageNetwork")
        text += "assert_label ComboHostNetRedundancy 6 ticks\n"
        text += checks("TextHostNetMinDelay", "CollectionBoxHostPageNetwork")
        for control in ("LabelHostNetSlowBound", "TextHostNetSlowBound", "LabelHostNetSlowBoundHint", "LabelHostNetSlowPolicy", "ComboHostNetSlowPolicy"):
            text += checks(control, "CollectionBoxHostPageNetwork")
        text += "assert_label TextHostNetSlowBound 3\n"
        text += "assert_label ComboHostNetSlowPolicy Hand the seat to the AI and let them rejoin\n"
        text += checks("LabelHostNetEffective", "CollectionBoxHostPageNetwork")
        text += checks("ButtonHostNetRecalc", "CollectionBoxHostPageNetwork")
        # H34: the host row names mode/capacity/seated humans off the adopted config; the three
        # visibility states are explicit and a LAN lobby sits on LAN only.
        text += checks("LabelHostNetMode", "CollectionBoxHostPageNetwork")
        text += "assert_label LabelHostNetMode Host mode: Playing - capacity 2 - humans seated 1\n"
        text += checks("ComboHostNetVisibility", "CollectionBoxHostPageNetwork")
        text += "assert_label ComboHostNetVisibility LAN only\n"
        text += checks("TextHostNetPort", "CollectionBoxHostPageNetwork")
        text += f"assert_label TextHostNetPort {port}\n"
        # An Internet pick needs a configured directory URL the fixture lacks: it refuses with the
        # reason and the combo snaps back to the live state.
        text += ("combo_select ComboHostNetVisibility Internet: Listed\nwait 3\n"
                 "assert_label LabelHostOptStatus Internet needs a session directory URL\n"
                 "assert_label ComboHostNetVisibility LAN only\n"
                 "combo_select ComboHostNetVisibility Internet: Unlisted\nwait 3\n"
                 "assert_label LabelHostOptStatus Internet needs a session directory URL\n"
                 "assert_label ComboHostNetVisibility LAN only\n")
        # A hosted session owns its bound port: the edit refuses, names the end-session path, and
        # the field resets to the live value.
        text += (f"set_text TextHostNetPort 40000\nwait 3\n"
                 "assert_label LabelHostOptStatus End the session to change the port\n"
                 f"assert_label TextHostNetPort {port}\n")
        text += "dump_host_options\n"
        # H25-H28 Recovery.
        text += "activate TabHostPageRecovery\nwait 3\nassert_visible CollectionBoxHostPageRecovery 1\n"
        text += "assert_label LabelHostOptionsTitle M A T C H   R E C O V E R Y\n"
        text += checks("CheckHostRecRepair", "CollectionBoxHostPageRecovery")
        text += checks("CheckHostRecAutosave", "CollectionBoxHostPageRecovery")
        text += ("setcheck CheckHostRecAutosave 1\nwait_ms 500\nassert_checked CheckHostRecAutosave 1\n"
                 "assert_enabled TextHostRecAutosaveInterval 1\n"
                 "setcheck CheckHostRecAutosave 0\nwait_ms 500\nassert_checked CheckHostRecAutosave 0\n"
                 "assert_enabled TextHostRecAutosaveInterval 0\n")
        text += checks("TextHostRecAutosaveInterval", "CollectionBoxHostPageRecovery")
        # H25: a lobby is not a live match, so the button is off and the hint says why.
        text += checks("ButtonHostRecRepairNow", "CollectionBoxHostPageRecovery")
        text += checks("LabelHostRecRepairHint", "CollectionBoxHostPageRecovery")
        text += ("assert_enabled ButtonHostRecRepairNow 0\n"
                 "assert_label LabelHostRecRepairHint Repair needs a live match session\n")
        text += "dump_host_options\n"
        # H29-H31 Files and status.
        text += "activate TabHostPageFiles\nwait 3\nassert_visible CollectionBoxHostPageFiles 1\n"
        text += "assert_label LabelHostOptionsTitle F I L E S   A N D   S T A T U S\n"
        text += checks("ButtonHostFilesSaveDiag", "CollectionBoxHostPageFiles")
        text += checks("ComboHostFilesWidget", "CollectionBoxHostPageFiles")
        text += "dump_host_options\n"
        # H32-H35 Session.
        text += "activate TabHostPageSession\nwait 3\nassert_visible CollectionBoxHostPageSession 1\n"
        text += "assert_label LabelHostOptionsTitle S E S S I O N\n"
        text += checks("LabelHostSessHosting", "CollectionBoxHostPageSession")
        text += checks("ComboHostSessIdle", "CollectionBoxHostPageSession")
        text += ("combo_select ComboHostSessIdle 5 minutes\nwait_ms 500\n"
                 "assert_label ComboHostSessIdle 5 minutes\n"
                 "combo_select ComboHostSessIdle 10 minutes\nwait_ms 500\n"
                 "assert_label ComboHostSessIdle 10 minutes\n")
        text += checks("LabelHostSessIdleState", "CollectionBoxHostPageSession")
        text += checks("LabelHostSessBanned", "CollectionBoxHostPageSession")
        text += checks("ButtonHostSessBanned", "CollectionBoxHostPageSession")
        text += checks("ButtonHostSessEnd", "CollectionBoxHostPageSession")
        # H10's count is the ban store's own rows: none yet, so the session row reads zero.
        text += "assert_label LabelHostSessBanned 0 banned this session\n"
        # H11: the banned-player dialog reads the store through GetBanRecords - empty here, so
        # the pick combo has no row to land Remove on and the button stays off.
        text += ("activate ButtonHostSessBanned\nwait 3\nassert_visible HostBannedDialog 1\n")
        for control in ("ComboHostBannedPick", "LabelHostBannedList", "LabelHostBannedStatus",
                        "ButtonHostBannedRemove", "ButtonHostBannedClose"):
            text += checks(control, "HostBannedDialog")
        text += ("assert_label LabelHostBannedList (no banned players)\n"
                 "assert_enabled ButtonHostBannedRemove 0\n"
                 "dump_host_options\nactivate ButtonHostBannedClose\nwait 3\n"
                 "assert_visible HostBannedDialog 0\n")
        # H31: the seating wait is the host's own policy and editable while the lobby is open - it is
        # the value a live Apply republishes, and Never leaves the lobby open instead of taking a day.
        text += "assert_enabled ComboHostSessIdle 1\n"
        text += checks("LabelHostSessIdle", "CollectionBoxHostPageSession")
        # H35: Save As Host Defaults is the one origin of the versioned defaults template.
        text += checks("ButtonHostOptDefaults", "MultiplayerHostOptionsPanel")
        text += "assert_enabled ButtonHostOptDefaults 1\n"
        text += "dump_host_options\n"
        # Back is local navigation: the lobby is still open under the panel when it leaves.
        text += "activate ButtonHostOptBack\nwait 5\nassert_substate Lobby\nexit\n"
    elif case == "input":
        text = (RESET_INPUT + "activate ButtonMainToMultiplayer\nwait 5\n"
                "assert_visible TextMultiplayerName 1\nfocus_next\nassert_focus TextMultiplayerName\n"
                "settext TextMultiplayerName abc\nkey End down\nkey End up\nkey Backspace down\n"
                "key Backspace up\nassert_label TextMultiplayerName ab\ndump_player_options\n"
                "focus_next\nassert_focus ButtonMultiplayerHostGame\nkey KP1 down\nkey KP1 up\nwait 3\n"
                "assert_substate HostSetup\nactivate ButtonHostBack\nwait 4\n"
                "focus_previous\nassert_focus TextMultiplayerName\nfocus_next\n"
                "pad south down\npad south up\nwait 3\nassert_substate HostSetup\n"
                "dump_host_options\nexit\n")
    elif case in ("live", "disabled", "scope-off", "input-parity"):
        text = ("wait 12\nassert_screen Pause\nassert_visible ButtonSettings 1\n" if case == "live"
                else RESET_INPUT + host_lobby(port) if case == "disabled"
                else RESET_INPUT + LANDING if case == "input-parity" else LANDING)
        text += "assert_visible root 1\n"
        if case in ("scope-off", "input-parity"):
            text += "focus_next\nassert_focus TextMultiplayerName\n"
        text += f"wait_file {probe_root(root, 'host') / 'done.json'} 90\nexit\n"
        steps = ([{"op": "wait", "sim_at_least": 150}, {"op": "key_down", "key": "Escape"},
                  {"op": "key_up", "key": "Escape"}, {"op": "wait", "screen": "Pause"}] if case == "live"
                 else [{"op": "wait", "screen": "MultiplayerScreen"}])
        if case == "live":
            # The match's pause menu opens without pausing the shared sim (L03): the menu is a local
            # surface, the synchronized pause is its own row. Both peers keep running while it is open.
            steps += [{"op": "assert", "equals": {"service": "Running", "paused": False}, "sim_at_least": 100},
                      menu_step("assert_visible ButtonSettings 1"), menu_step("dump_host_options"),
                      menu_step("activate ButtonSettings"), {"op": "wait", "screen": "PauseSettings"},
                      menu_step("assert_visible CollectionBoxGameplaySettings 1"), menu_step("dump_player_options"),
                      menu_step("post_command ButtonBackToMainMenu"), {"op": "wait", "screen": "Pause"},
                      menu_step("assert_visible ButtonSettings 1"), menu_step("dump_host_options"),
                      {"op": "assert", "equals": {"service": "Running", "paused": False}, "sim_at_least": 100}]
        elif case == "input-parity":
            steps += [{"op": "wait", "scope": "menu", "control": "TextMultiplayerName", "equals": {"focus": True}}]
            for route in ("key", "pad", "mouse", "post_command"):
                if route == "post_command":
                    steps += [menu_step("post_command ButtonMultiplayerHostGame")]
                else:
                    steps += [{"op": "mouse_move", "scope": "menu", "control": "ButtonMultiplayerHostGame"}]
                    for edge in ("down", "up"):
                        steps += ([{"op": f"mouse_{edge}", "scope": "menu", "control": "ButtonMultiplayerHostGame"}]
                                  if route == "mouse" else [menu_step(f"{route} {'KP1' if route == 'key' else 'south'} {edge}")])
                        if edge == "down":
                            steps += [menu_step("assert_focus ButtonMultiplayerHostGame")]
                steps += [menu_step("assert_visible TextHostPort 1"), menu_step("dump_host_options"),
                          menu_step("post_command ButtonHostBack"), menu_step("assert_visible ButtonMultiplayerHostGame 1")]
                if route != "post_command":
                    steps += [menu_step("focus_previous")]
                steps += [menu_step("assert_focus TextMultiplayerName")]
        elif case == "disabled":
            steps += [{"op": "wait", "scope": "menu", "control": "ButtonMultiplayerStart",
                       "equals": {"visible": True, "enabled": False}},
                      {"op": "mouse_move", "scope": "menu", "control": "ButtonMultiplayerStart"},
                      menu_step("dump_host_options"), menu_step("key KP1 down"), menu_step("key KP1 up"),
                      menu_step("pad south down"), menu_step("pad south up"),
                      menu_step("assert_enabled ButtonMultiplayerStart 0"), menu_step("dump_host_options")]
        else:
            steps += [{"op": "wait", "scope": "menu", "control": "TextMultiplayerName",
                       "equals": {"focus": True}}, menu_step("dump_player_options"),
                      {"op": "mouse_move", "scope": "menu", "control": "ButtonMultiplayerHostGame"},
                      {"op": "input_scope", "enabled": False},
                      {"op": "menu", "command": "key KP1 down", "accepted": False},
                      {"op": "menu", "command": "key KP1 up", "accepted": False},
                      {"op": "menu", "command": "pad south down", "accepted": False},
                      {"op": "menu", "command": "pad south up", "accepted": False},
                      {"op": "wait", "renders": 3}, menu_step("assert_focus TextMultiplayerName"),
                      menu_step("dump_player_options"), {"op": "input_scope", "enabled": True}]
        steps += [{"op": "signal", "name": "done"}, {"op": "finish"}]
        probe = {"schema": 1, "timeout_ms": 90000, "steps": steps}
    else:
        raise ValueError(case)
    texts = {"host": text}
    if case == "live":
        texts["client"] = f"wait_file {probe_root(root, 'host') / 'done.json'} 90\nexit\n"
    return texts, {"host": probe} if probe else {}


def pixel_luma(rgb):
    r, g, b = rgb[:3]
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


# TabBlue.png on 6447c4c2e3: Base and MouseOver RGB slice identity. Selected
# fill was panel grey (59, 65, 83), luma 65.024; the floor sits above that.
TABBLUE_SIZE = (63, 59)
TABBLUE_BASE_SHA256 = "4f8ea8767d5c1762dcc9e2adea0f0cffeeee72ca770211ed855095ab2be14a4b"
TABBLUE_MOUSEOVER_SHA256 = "d832a67b1bebd77870ba53071ec36af04c4a2261b387950d1265630d4571752a"
PANEL_GREY_FILL_LUMA = 65.024
SELECTED_FILL_LUMA_FLOOR = 80.0
TABBLUE_CHROME = (24, 28, 55)
TABBLUE_COLOR_KEY = (0, 0, 0)


def tabblue_slice_sha256(image, box):
    return hashlib.sha256(image.crop(box).convert("RGB").tobytes()).hexdigest()


def settings_tab_selected_fill_luma(repo):
    """Mean luma of the Selected slice interior fill, not chrome or the key."""
    path = Path(repo) / "Data/Base.rte/GUIs/Skins/Menus/TabBlue.png"
    with Image.open(path) as source:
        assert source.size == TABBLUE_SIZE, source.size
        image = source.convert("RGB")
    pixels = []
    for y in range(42, 57):
        for x in range(2, 61):
            rgb = image.getpixel((x, y))[:3]
            if rgb in (TABBLUE_CHROME, TABBLUE_COLOR_KEY):
                continue
            pixels.append(rgb)
    return (sum(pixel_luma(rgb) for rgb in pixels) / len(pixels)) if pixels else 0.0


def assert_tabblue_selected_fill(repo):
    """Base TabBlue fails (fill luma 65.024); the lightened Selected fill passes."""
    path = Path(repo) / "Data/Base.rte/GUIs/Skins/Menus/TabBlue.png"
    with Image.open(path) as source:
        assert source.size == TABBLUE_SIZE, source.size
        image = source.convert("RGB")
    base = tabblue_slice_sha256(image, (0, 0, 63, 19))
    mouseover = tabblue_slice_sha256(image, (0, 20, 63, 39))
    assert base == TABBLUE_BASE_SHA256, (base, TABBLUE_BASE_SHA256)
    assert mouseover == TABBLUE_MOUSEOVER_SHA256, (mouseover, TABBLUE_MOUSEOVER_SHA256)
    skin = (Path(repo) / "Data/Base.rte/GUIs/Skins/Menus/MainMenuSubMenuSkin.ini").read_text(encoding="utf-8")
    assert "ColorKeyIndex = 0" in skin.split("[Tab]", 1)[1].split("[", 1)[0]
    fill = settings_tab_selected_fill_luma(repo)
    assert fill > PANEL_GREY_FILL_LUMA and fill >= SELECTED_FILL_LUMA_FLOOR, (
        fill, PANEL_GREY_FILL_LUMA, SELECTED_FILL_LUMA_FLOOR)
    return fill


def cell_ink_signature(cell, red, bg):
    return tuple((x, y) for y, row in enumerate(cell.rows)
                 for x, pixel in enumerate(row) if pixel != red and pixel != bg)


def assert_fontsmall_latin1_ink(repo):
    """Menus FontSmall 0xC0 and 0xD7 have ink; À/É/Ñ have distinct signatures.

    The base atlas paints those letters as one placeholder blob, so the
    signature set has size 1 and this fails. The extended atlas passes.
    """
    import sys
    fonts = str(Path(__file__).resolve().parent / "fonts")
    if fonts not in sys.path:
        sys.path.insert(0, fonts)
    from extend_font import ink_count, parse_font

    path = Path(repo) / "Data/Base.rte/GUIs/Skins/Menus/FontSmall.png"
    font = parse_font(path)
    red, bg = font["red"], font["bg"]
    for code in (0xC0, 0xD7):
        assert ink_count(font["cells"][code], red, bg) > 0, (hex(code), "no ink")
    sigs = [cell_ink_signature(font["cells"][code], red, bg) for code in (0xC0, 0xC9, 0xD1)]
    assert all(sigs) and len(set(sigs)) == 3, ("À/É/Ñ signatures collide", [len(s) for s in sigs])
    return {
        "ink_0xC0": ink_count(font["cells"][0xC0], red, bg),
        "ink_0xD7": ink_count(font["cells"][0xD7], red, bg),
    }


def frame_luma(png, rect, border=2):
    """Mean luma of a control's 2-px frame, the compare_luma.py shape from the disabled-state lane."""
    with Image.open(png) as source:
        image = source.convert("RGB")
    x, y, w, h = rect
    pixels = []
    for i in range(max(0, w)):
        for t in range(border):
            if 0 <= y + t < image.size[1] and 0 <= x + i < image.size[0]:
                pixels.append(image.getpixel((x + i, y + t)))
            if 0 <= y + h - 1 - t < image.size[1] and 0 <= x + i < image.size[0]:
                pixels.append(image.getpixel((x + i, y + h - 1 - t)))
    for j in range(border, max(border, h - border)):
        for t in range(border):
            if 0 <= y + j < image.size[1] and 0 <= x + t < image.size[0]:
                pixels.append(image.getpixel((x + t, y + j)))
            if 0 <= y + j < image.size[1] and 0 <= x + w - 1 - t < image.size[0]:
                pixels.append(image.getpixel((x + w - 1 - t, y + j)))
    if not pixels:
        return 0.0
    return sum(0.2126 * r + 0.7152 * g + 0.0722 * b for r, g, b in pixels) / len(pixels)


def inside(rect, parent):
    x, y, w, h = rect
    px, py, pw, ph = parent
    return w > 0 and h > 0 and px <= x and py <= y and x + w <= px + pw and y + h <= py + ph


def captures(runtime, metadata):
    rows = []
    for path in sorted((runtime / "ScreenShots").glob("dump_*_*.json"), key=lambda path: int(path.stem.rsplit("_", 1)[1])):
        value = json.loads(path.read_text(encoding="utf-8"))
        png = path.with_suffix(".png")
        with Image.open(png) as source:
            source.load()
            width, height = source.size
            assert [0, 0, width, height] == value["viewport"], (path, source.size, value["viewport"])
            assert source.getbbox(), f"empty PNG: {png}"
            names = {control["name"]: control for control in value["controls"]}
            assert len(names) == len(value["controls"]), f"duplicate control: {path}"
            for control in value["controls"]:
                hidden_preset = control["name"] == "ComboPresetResolution" and control.get("visible") is False
                assert control["visible"] is True or hidden_preset, (path, control)
                assert inside(control["rect"], value["viewport"]), (path, control)
                assert inside(control["rect"], control["parent_rect"]), (path, control)
                if control["parent"] and not hidden_preset:
                    assert control["parent"] in names, (path, control)
                    assert names[control["parent"]]["rect"] == control["parent_rect"], (path, control)
                x, y, w, h = control["rect"]
                assert source.crop((x, y, x + w, y + h)).size == (w, h), (path, control)
                assert {"name", "rect", "text", "enabled", "visible", "focus"} <= control.keys()
        rows.append({**metadata, **value, "png": str(png), "png_sha256": sha(png),
                     "json": str(path), "json_sha256": sha(path), "dimensions": [width, height]})
    return rows


def host_options_geometry(images):
    measured = {}
    for capture in images:
        controls = {row["name"]: row for row in capture["controls"]}
        panel = controls.get("MultiplayerHostOptionsPanel")
        back = controls.get("ButtonHostOptBack")
        if not panel or not back:
            continue
        for name, page in controls.items():
            if name.startswith("CollectionBoxHostPage"):
                bottom = page["rect"][1] + page["rect"][3]
                assert bottom + 4 <= back["rect"][1], (name, page["rect"], back["rect"])
                measured[name] = page["rect"]
        visibility = controls.get("ComboHostNetVisibility")
        if visibility:
            x, y, width, height = visibility["rect"]
            drop_bottom = y + height + 56
            assert drop_bottom <= panel["rect"][1] + panel["rect"][3], (visibility, panel, drop_bottom)
            measured["visibility_drop_bottom"] = drop_bottom
    assert all("CollectionBoxHostPage" + page in measured for page in
               ("Seats", "Rules", "Network", "Recovery", "Files", "Session")), measured
    return measured


def timing_options_geometry(images):
    measured = []
    # Rows sit in the Tuning box, 24 px below the page's tab strip.
    expected = {'LabelHostNetSlowBound': (8, 68, 104, 18), 'TextHostNetSlowBound': (120, 68, 44, 18),
                'LabelHostNetSlowBoundHint': (172, 68, 100, 18), 'LabelHostNetSlowPolicy': (8, 88, 172, 18),
                'ComboHostNetSlowPolicy': (184, 88, 329, 18)}
    for capture in images:
        rows = {row['name']: row for row in capture['controls']}
        if 'CollectionBoxHostPageNetwork' not in rows:
            continue
        page = rows['CollectionBoxHostPageNetwork']['rect']
        for name, rectangle in expected.items():
            row = rows[name]
            actual = (row['rect'][0] - page[0], row['rect'][1] - page[1], *row['rect'][2:])
            assert actual == rectangle and row['text_fits'], (name, actual, rectangle, row)
        combo = rows['ComboHostNetSlowPolicy']['rect']
        assert combo[1] + combo[3] + 40 <= page[1] + page[3], combo
        measured.append({'page': page, 'bound': rows['TextHostNetSlowBound']['rect'], 'policy': combo})
    assert measured, 'timing option rows were never read back'
    return measured


def run_case(options, case, root, failing=None):
    root.mkdir(parents=True, exist_ok=False)
    texts, probes = scripts(case, options.port, root)
    if failing:
        prelude, setup, assertion = failing
        texts, probes = {"host": prelude + setup + assertion + "\nexit\n"}, {}
    inputs = root / "input.txt"
    inputs.write_text(INPUT_SCRIPT, encoding="utf-8")
    paired = case in ("pause", "repair", "live", "net-options", "net-activity", "local-end-match", "net-host-left")
    # A menu-driven pair joins through the real UI, so it carries no service-e2e flags.
    menu_driven = case in ("net-activity", "local-end-match", "net-host-left")
    seeded = {} if failing else seeds(case)
    runs, records, argv, images = {}, {}, {}, []
    result = {"pass": False, "case": case, "scripts": {}, "records": records, "probes": {}, "seeds": seeded}
    try:
        for who in (("host", "client") if paired else ("host",)):
            script = root / f"{who}-menu.txt"
            script.write_text(texts[who], encoding="utf-8")
            result["scripts"][str(script)] = sha(script)
            args = ["-menu-script", str(script)]
            if case == "net-host-left":
                args += ["-input-script", str(inputs)]
            if case == "landing":
                # The flag takes the same over-cap name the box gets below: one console refusal.
                args += ["-net-player-name", "F" * (DISPLAY_NAME_MAX_BYTES + 1)]
            if paired and not menu_driven:
                args += ["-net-match-service-e2e", "-net-port", str(options.port), "-net-match-peers", "2",
                         "-net-match-ticks", "400", "-net-match-input-delay", "3", "-net-autosave-seconds", "0",
                         "-input-script", str(inputs), "-net-match-report", str(root / f"{who}-match.json")]
                args += ["-net-host"] if who == "host" else ["-net-join", "127.0.0.1"]
                if case == "repair":
                    args += ["-net-match-e2e-resync"]
            env = {"CCCP_HEADLESS": "1"}
            if who in probes:
                directory = probe_root(root, who)
                directory.mkdir()
                path = directory / "probe.json"
                path.write_text(json.dumps(probes[who], indent=2) + "\n", encoding="utf-8")
                result["scripts"][str(path)] = sha(path)
                env["CC_TEST_NET_UI_SCRIPT"] = str(path)
            argv[who] = args
            runs[who] = make_run(options.repo, args, root / who, 180, env=env)
            set_visual_resolution(runs[who], *map(int, options.size.split("x")))
            if case in ("lobby", "host-defaults"):
                (runs[who].cwd / "Userdata/NetworkHostDefaults.ini").write_text(
                    "Version = 2\nFrameRedundancyTicks = 6\nSlowPlayerBoundTicks = 3\nSlowPlayerPolicy = substitute\n", encoding="utf-8")
            if case in ("host-stun", "host-stun-empty", "host-relay", "net-connection"):
                settings_path = runs[who].cwd / "Userdata/Settings.ini"
                settings = settings_path.read_text(encoding="utf-8-sig")
                settings = re.sub(r"(?m)^[ \t]*(?:" + "|".join(RELAY_KEYS) + r")[ \t]*=[^\r\n]*", "", settings)
                settings_path.write_text(settings, encoding="utf-8")
            if who in seeded:
                seed_settings(runs[who].cwd / "Userdata/Settings.ini", seeded[who])

        def drive(who):
            try:
                records[who] = runs[who].start().finish()
            except Exception as error:
                records[who] = {"error": repr(error)}

        threads = [threading.Thread(target=drive, args=(who,)) for who in runs]
        for index, thread in enumerate(threads):
            thread.start()
            if menu_driven and index == 0:
                # The joining peer's menu must find the host's lobby already listening.
                threading.Event().wait(2.0)
        for thread in threads:
            thread.join()
        logs = {who: "\n".join((run.out / leaf).read_text(encoding="utf-8", errors="replace")
                               for leaf in ("stdout.log", "stderr.log") if (run.out / leaf).exists())
                for who, run in runs.items()}
        result["unknown_commands"] = [line for log in logs.values() for line in log.splitlines()
                                      if "[menu-script] FAILED: unknown command:" in line]
        for who, run in runs.items():
            record = records[who]
            assert not record.get("timed_out"), record
            if failing:
                assert record.get("exit_code") != 0 and record.get("exit_code") is not None, record
                assert f"[menu-script] FAILED: {assertion.split()[0]}" in logs[who], logs[who][-3000:]
                assert "unknown command" not in logs[who], logs[who][-3000:]
            else:
                images += captures(run.cwd, {"source_revision": options.revision,
                    "executable": str(options.repo / "Cortex Command.exe"), "exe_sha256": options.exe_sha,
                    "os": os.name, "configuration": "Final", "argv": record.get("argv", argv[who]),
                    "peer": who, "case": case, "logical_size": options.size})
                if case == "net-activity" and who == "host":
                    host_setup = [image for image in images if image["peer"] == "host"
                                  and any(c["name"] == "ComboHostActivity" for c in image["controls"])]
                    if host_setup:
                        picker = next(c for c in host_setup[0]["controls"] if c["name"] == "ComboHostActivity")
                        assert_combo_matches_loaded_activities(picker, host_setup[0])
                assert record.get("exit_code") == 0, record
                assert "[menu-script] FAILED:" not in logs[who], logs[who][-3000:]
        if not failing:
            if case == "net-activity":
                drawn = [(image["json"], control) for image in images for control in image["controls"]
                         if control["name"] in ("ComboHostActivity", "ComboHostScene", "ComboHostMode")
                         and "drawn" in control and not combo_drawn_follows(control)]
                assert not drawn, drawn
                host_setup = [image for image in images if image["peer"] == "host"
                              and any(c["name"] == "ComboHostActivity" for c in image["controls"])]
                assert host_setup, "ComboHostActivity missing from host dumps"
                picker = next(c for c in host_setup[0]["controls"] if c["name"] == "ComboHostActivity")
                assert_combo_matches_loaded_activities(picker, host_setup[0])
            # A peer scripted to dump must have written one: the global count passed on its peer's captures.
            scripted = {who: texts[who] + json.dumps(probes.get(who, {})) for who in runs}
            assert images or not any("dump_" in script for script in scripted.values()), "no paired dumps/PNGs"
            silent = [who for who, script in scripted.items()
                      if "dump_" in script and not any(image["peer"] == who for image in images)]
            assert not silent, f"no readback capture from {silent}"
        if case == "live":
            # The host's own option records the round under its session and round, in the directory
            # the replay browser lists; a client records nothing.
            recorded = sorted((runs["host"].cwd / "Userdata/Replays").glob("match-*-r*.ccreplay"))
            assert len(recorded) == 1 and recorded[0].stat().st_size > 0, recorded
            assert recorded[0].name in logs["host"], recorded[0].name
            assert not list((runs["client"].cwd / "Userdata/Replays").glob("*.ccreplay")) if (runs["client"].cwd / "Userdata/Replays").exists() else True
            result["replay_recorded"] = recorded[0].name
        for who in probes:
            observation = json.loads((probe_root(root, who) / "net-ui-result.json").read_text(encoding="utf-8"))
            result["probes"][who] = observation
            assert observation["pass"] and observation["complete"], (who, observation)
        if case in ("host-stun", "host-stun-empty"):
            pages = [{c["name"]: c for c in capture["controls"]} for capture in images
                     if any(c["name"] == "ComboHostNetIce" for c in capture["controls"])]
            assert [page["ComboHostNetIce"]["text"] for page in pages] == [*NAT_STATES, *NAT_STATES], pages
            for page in pages:
                label, combo, hint = (page[name] for name in ("LabelHostNetIce", "ComboHostNetIce", "LabelHostNetIceHint"))
                assert label["text"] == NAT_LABEL and hint["text"].startswith(NAT_HINT + "\n"), (label, hint)
                assert combo_item_names(combo) == list(NAT_STATES), combo
                assert all(row["text_fits"] for row in (label, combo, hint)), (label, combo, hint)
                assert label["rect"][1] == combo["rect"][1], (label, combo)
                assert label["rect"][0] + label["rect"][2] + 8 <= combo["rect"][0], (label, combo)
                page_rect = page["CollectionBoxHostPageNetwork"]["rect"]
                assert page_rect[1] + page_rect[3] + 4 <= page["ButtonHostOptBack"]["rect"][1], page
            for page in (pages[0], pages[2]):
                expected = "STUN list empty: direct candidates are LAN-only." if case == "host-stun-empty" else "STUN finds direct routes."
                assert expected in page["LabelHostNetIceHint"]["text"], page
                assert "session directory URL" in page["LabelHostNetIceHint"]["text"], page
            expected_saved = dict.fromkeys(NAT_KEYS, "")
            expected_saved.update({"NetworkIceEnable": "0", "NetworkStunServers": "" if case == "host-stun-empty" else STUN_DEFAULT})
            result["saved"] = read_settings(runs["host"].cwd / "Userdata/Settings.ini", set(NAT_KEYS))
            assert result["saved"] == expected_saved, result["saved"]
            result["nat_rows"] = [{name: page[name]["rect"] for name in ("LabelHostNetIce", "ComboHostNetIce", "LabelHostNetIceHint")}
                                  for page in pages]
        if case == "host-relay":
            pages = [{row["name"]: row for row in capture["controls"]} for capture in images]
            assert [page["ComboHostNetRelay"]["text"] for page in pages] == ["Directory", "Off", "Fixed", "Directory"], pages
            assert all(combo_item_names(page["ComboHostNetRelay"]) == ["Off", "Directory", "Fixed"] for page in pages)
            assert pages[2]["TextHostRelayPass"]["text"] == "*" * len("fixed-password"), pages[2]
            assert "fixed-password" not in json.dumps(images), "host password escaped the masked readback"
            result["saved"] = read_settings(runs["host"].cwd / "Userdata/Settings.ini", set(RELAY_KEYS))
            assert result["saved"]["NetworkHostRelayMode"] == "Directory", result["saved"]
            assert [result["saved"][key] for key in ("NetworkTurnServers", "NetworkTurnUser", "NetworkTurnPass")] == ["relay.example:3478", "fixed-user", "fixed-password"], result["saved"]
        if case == "net-connection":
            pages = [{row["name"]: row for row in capture["controls"]} for capture in images]
            assert [page["ComboNetworkConnection"]["text"] for page in pages] == ["Automatic", "Direct only", "Relay only", "Automatic", "Automatic"], pages
            assert all(combo_item_names(page["ComboNetworkConnection"]) == ["Automatic", "Direct only", "Relay only"] for page in pages)
            assert all(page["LabelNetworkConnection"]["text"] == "Connection" and page["LabelNetworkStunServers"]["text"] == "STUN server list" for page in pages)
            assert pages[0]["TextNetworkStunServers"]["text"] == STUN_DEFAULT, pages[0]
            assert pages[-1]["TextNetworkRelayPass"]["text"] == "*" * len("personal-password"), pages[-1]
            assert "personal-password" not in json.dumps(images), "player password escaped the masked readback"
            wanted = {"NetworkConnectionMode": "Automatic", "NetworkStunServers": "stun.example:3478",
                      "NetworkPlayerTurnServers": "personal.example:3478", "NetworkPlayerTurnUser": "personal-user", "NetworkPlayerTurnPass": "personal-password"}
            result["saved"] = read_settings(runs["host"].cwd / "Userdata/Settings.ini", set(wanted))
            assert result["saved"] == wanted, result["saved"]
            result["connection_rects"] = {name: pages[-1][name]["rect"] for name in CONNECTION_ROWS}
        if case == "repair":
            snapshots = re.findall(r"\[net-match\] resync snapshot at tick (\d+)", logs["host"])
            assert len(snapshots) == 1 and int(snapshots[0]) >= 150, logs["host"][-6000:]
            for who in ("host", "client"):
                assert logs[who].count("[net-match] resync: match relaunched from the snapshot") == 1, logs[who][-6000:]
            result["repair_snapshot_tick"] = int(snapshots[0])
        if case in ("disabled", "scope-off"):
            first, last = images[0], images[-1]
            focused = [[c["name"] for c in capture["controls"] if c["focus"]] for capture in (first, last)]
            # Each case claims something about one named control, so equal focus lists alone prove nothing:
            # scope-off keeps its textbox focused through the ignored input, and the greyed-out Start button
            # must never take focus. Either way the control has to be drawn in both captures.
            watched = "TextMultiplayerName" if case == "scope-off" else "ButtonMultiplayerStart"
            for capture in (first, last):
                assert any(c["name"] == watched for c in capture["controls"]), f"{watched} is not in {capture['json']}"
            if case == "scope-off":
                assert focused[0] == [watched], f"{watched} is not the focused control: {focused}"
            else:
                assert watched not in focused[0] + focused[1], f"the disabled {watched} took focus: {focused}"
            assert focused[0] == focused[1], f"the focused control changed across the run: {focused}"
            assert first["screen"] == last["screen"] == "MultiplayerScreen"
            assert first["service"] == last["service"], (first["service"], last["service"])
        if case == "pause":
            # Both peers read the same menu: the match rows, no single-player row, and the two settings
            # pages. The client goes on to drive the leave-confirm surface its match rows open.
            expected_screens = {"host": ["Pause", "PauseMatchOptions"] + ["PauseSettings"] * len(PAUSE_PAGES) + ["Pause"],
                                "client": ["Pause", "PauseMatchOptions"] + ["PauseSettings"] * len(PAUSE_PAGES) + ["Pause", "PauseLeaveConfirm"]}
            for who in ("host", "client"):
                peer = [capture for capture in images if capture["peer"] == who]
                assert [capture["screen"] for capture in peer] == expected_screens[who], (who, peer)
                pause_settings = [capture for capture in peer if capture["screen"] == "PauseSettings"]
                assert [capture["settings_page"].split(":")[0] for capture in pause_settings] == list(PAUSE_PAGES), (who, pause_settings)
                result.setdefault("pause_page_value_columns", {})[who] = page_value_columns(pause_settings, PAUSE_PAGE_FIRST_VALUE)
                result.setdefault("pause_layout", {})[who] = [row for capture in pause_settings for row in layout_readback(capture)]
                assert all(row["pass"] for row in result["pause_layout"][who]), result["pause_layout"][who]
                result.setdefault("pause_video_input_fit", {})[who] = video_input_fit_rows(
                    pause_settings, set(VIDEO_INPUT_FIT))
                pauses = [capture for capture in peer if capture["screen"] == "Pause"]
                for capture in (pauses[0], pauses[-1]):
                    drawn = {control["name"] for control in capture["controls"]}
                    assert set(MATCH_ROWS) <= drawn, (who, sorted(drawn))
                    assert not set(SINGLE_PLAYER_ROWS) & drawn, (who, sorted(drawn))
                    resume = next(control for control in capture["controls"] if control["name"] == "ButtonResume")
                    assert "back to game" in resume["text"].lower(), resume["text"]
            confirm = [capture for capture in images if capture["screen"] == "PauseLeaveConfirm"]
            assert len(confirm) == 1 and confirm[0]["peer"] == "client", confirm
            drawn = {control["name"] for control in confirm[0]["controls"]}
            assert {"LeaveConfirmBox", "LabelLeaveConfirm", "ButtonLeaveConfirm", "ButtonLeaveCancel"} <= drawn, sorted(drawn)
            # The clean leave, read from each side's own match report: the survivor's match ends
            # complete, not broken, and the leaver's records its leave, not a crash.
            reports = {who: json.loads((root / f"{who}-match.json").read_text(encoding="utf-8"))
                       for who in ("host", "client")}
            assert reports["host"]["service"]["status"] == "The other player left the match", reports["host"]["service"]["status"]
            assert reports["client"]["service"]["status"] == "Match left", reports["client"]["service"]["status"]
        if case == "pages":
            # The network page reports its selector's page ("Network:Player"); the top name is the prefix.
            assert [capture["settings_page"].split(":")[0] for capture in images] == list(PAGES), [c["settings_page"] for c in images]
            captioned = [control for capture in images for control in capture["controls"] if control["text"]]
            assert captioned and all("text_fits" in control for control in captioned), "a caption carries no fit measurement"
            # The rows the options program adds land on these pages; the arm measures them the run they appear.
            result["watched"] = [[capture["settings_page"], control["name"], control["text_fits"], control["text_measure"]]
                                 for capture in images for control in capture["controls"] if control["name"] in WATCHED]
            assert all(row[2] for row in result["watched"]), result["watched"]
            result["text_overflow"] = [[capture["settings_page"], control["name"], control["text_measure"]]
                                       for capture in images for control in capture["controls"]
                                       if control.get("text_fits") is False]
            assert not result["text_overflow"], result["text_overflow"]
            result["video_input_fit"] = video_input_fit_rows(images, set(VIDEO_INPUT_FIT))
            result["page_value_columns"] = page_value_columns(images, PAGE_FIRST_VALUE)
            result["layout"] = [row for capture in images for row in layout_readback(capture)]
            assert all(row["pass"] for row in result["layout"]), result["layout"]
            result["fontsmall_latin1"] = assert_fontsmall_latin1_ink(options.repo)
            fill = assert_tabblue_selected_fill(options.repo)
            result["tab_luma"] = {
                "selected_fill": fill,
                "panel_grey": PANEL_GREY_FILL_LUMA,
                "floor": SELECTED_FILL_LUMA_FLOOR,
            }
            gameplay = next((capture for capture in images if capture["settings_page"] == "Gameplay"), None)
            assert gameplay, [capture["settings_page"] for capture in images]
            rows = {control["name"]: control for control in gameplay["controls"]}
            label, combo = rows["LabelBrainlessHumansSpectate"], rows["ComboBrainlessHumansSpectate"]
            smart, unheld = rows["CheckboxSmartBuyMenuNavigation"], rows["LabelMaxUnheldItems"]
            assert combo["text"] == "End the match", combo
            assert combo["rect"][1] == label["rect"][1], (combo["rect"], label["rect"])
            assert label["rect"][1] - smart["rect"][1] == 20, (smart["rect"], label["rect"])
            assert unheld["rect"][1] - label["rect"][1] == 20, (label["rect"], unheld["rect"])
            def overlap(left, right):
                ax, ay, aw, ah = left["rect"]
                bx, by, bw, bh = right["rect"]
                return ax < bx + bw and bx < ax + aw and ay < by + bh and by < ay + ah
            assert not overlap(label, smart) and not overlap(label, unheld), (label["rect"], smart["rect"], unheld["rect"])
            assert not overlap(combo, smart) and not overlap(combo, unheld), (combo["rect"], smart["rect"], unheld["rect"])
        if case == "combo-fit":
            result["combo_fit"] = [[match[0], [int(v) for v in match[1:5]], [int(v) for v in match[5:7]]]
                                   for log in logs.values() for match in FIT_LINE.findall(log)]
            assert result["combo_fit"], "no assert_text_fits observation in the log"
            for name, rect, available in result["combo_fit"]:
                assert available[0] <= rect[2] - COMBO_BUTTON, (name, rect, available)
        if case == "network":
            # The page shows the saved name, the page's own edits are saved, and the lobby box starts from them.
            page = [capture for capture in images if capture["settings_page"] == "Network:Player"]
            assert len(page) == 2, [capture["settings_page"] for capture in images]
            rows = {control["name"]: control for control in page[0]["controls"]}
            assert set(NETWORK_ROWS) <= rows.keys(), sorted(rows)
            # The settings dialog's base box centres on the viewport at every size, not only at 640.
            base = rows["CollectionBoxSettingsBase"]["rect"]
            res_y = int(options.size.split("x")[1])
            assert base[1] * 2 + base[3] == res_y, (base, res_y)
            # The player page shares the other pages' value column.
            column = rows["CollectionBoxNetPagePlayer"]["rect"][0] + NETWORK_VALUE_COLUMN
            assert rows["CheckboxNetworkPrediction"]["rect"][0] == column, rows["CheckboxNetworkPrediction"]["rect"]
            assert not set(NETWORK_FIXED_ROWS) & rows.keys(), sorted(rows)
            assert all(rows[name]["text_fits"] for name in NETWORK_ROWS if rows[name]["text"]), rows
            after = {control["name"]: control for control in page[1]["controls"]}
            assert set(NETWORK_ROWS) | set(NETWORK_FIXED_ROWS) <= after.keys(), sorted(after)
            assert all(after[name]["text_fits"] for name in NETWORK_FIXED_ROWS if after[name]["text"]), after
            # The selector row sits on the network box; only the player page's box is drawn.
            tabs = {control["name"]: control for control in page[0]["controls"] if control["name"] in NETWORK_TABS}
            assert len(tabs) == len(NETWORK_TABS) and all(tab["text_fits"] for tab in tabs.values()), tabs
            assert not any(box in rows or box in after for box in NETWORK_BOXES[1:]), sorted(rows)
            result["page_text"] = {name: [rows[name]["text"], after[name]["text"]] for name in NETWORK_ROWS}
            assert result["page_text"]["TextNetworkDisplayName"] == [NETWORK_SEED["NetworkDisplayName"], NETWORK_SAVED["NetworkDisplayName"]], result["page_text"]
            result["saved"] = read_settings(runs["host"].cwd / "Userdata/Settings.ini", set(NETWORK_SAVED))
            assert result["saved"] == NETWORK_SAVED, result["saved"]
            # The page sits on the Misc page's grid: a 20px row pitch, and under the automatic policy
            # the hidden fixed row leaves no gap behind it.
            pitch = ("LabelNetworkDisplayName", "LabelNetworkDelayPolicy", "LabelNetworkFixedDelay",
                     "LabelNetworkIdleWait", "LabelNetworkPathHorizon", "CheckboxNetworkAutoRepair",
                     "CheckboxNetworkToasts", "LabelMatchStatusWidget", "CheckboxNetworkDiagnostics")
            deltas = [after[b]["rect"][1] - after[a]["rect"][1] for a, b in zip(pitch, pitch[1:])]
            assert deltas == [20] * 8, deltas
            without_fixed = pitch[:2] + pitch[3:]
            closed = [rows[b]["rect"][1] - rows[a]["rect"][1] for a, b in zip(without_fixed, without_fixed[1:])]
            assert closed == [20] * 7, closed
            # The landing's name row shares the Host/Join block's centre line; doubled centres avoid halves.
            landing = {c["name"]: c for c in images[-1]["controls"]}
            prompt, box = landing["LabelMultiplayerNamePrompt"]["rect"], landing["TextMultiplayerName"]["rect"]
            host_button = landing["ButtonMultiplayerHostGame"]["rect"]
            row = [c["rect"] for c in landing.values() if c["rect"][1] == host_button[1] and c["rect"][3] == host_button[3]]
            last_button = max(row, key=lambda rect: rect[0])
            assert prompt[0] + box[0] + box[2] == host_button[0] + last_button[0] + last_button[2], (prompt, box, host_button, last_button)
            assert next(c["text"] for c in images[-1]["controls"] if c["name"] == "TextMultiplayerName") == NETWORK_SAVED["NetworkDisplayName"]
            # The host screen's own readback: the label sits beside the delay box and names the saved
            # policy with the frames it would send - the box pre-fills that same count.
            host_rows = {c["name"]: c for c in images[-2]["controls"]}
            policy = host_rows.get("LabelHostInputDelayPolicy")
            assert policy and policy["text"] == "(fixed, " + NETWORK_SAVED["NetworkInputDelayFrames"] + ")", policy
            assert host_rows["TextHostInputDelay"]["text"] == NETWORK_SAVED["NetworkInputDelayFrames"], host_rows["TextHostInputDelay"]
            assert host_rows["TextHostInputDelay"]["enabled"] is True, host_rows["TextHostInputDelay"]
        if case in ("net-chat", "net-recovery", "net-files", "net-internet", "misc-page"):
            # One capture per page case: the sub-page's own rows, all fitted, and nothing the case
            # names as disabled enabled in the draw.
            sub_page = {"net-chat": "Network:Chat", "net-recovery": "Network:Recovery",
                        "net-files": "Network:Files", "net-internet": "Network:Internet",
                        "misc-page": "Misc"}[case]
            assert [capture["settings_page"] for capture in images] == [sub_page], [c["settings_page"] for c in images]
            if case == "net-chat":
                result["size_gates"] = [list(row) for row in SIZE_GATES]
                result["net_chat_size"] = options.size
            rows = {control["name"]: control for control in images[0]["controls"]}
            expected = {"net-chat": ("CheckboxNetworkChatVisible", "CheckboxNetworkChatSound", "ComboNetworkChatScope",
                                     "CheckboxNetworkChatNotify", "ComboNetworkChatTextSize",
                                     "LabelNetworkChatKey", "TextNetworkChatKey",
                                     "ButtonNetMutedPlayers", "LabelNetMutedReason"),
                        "net-recovery": ("CheckboxNetworkAutoReconnect", "CheckboxNetworkOfferRejoin",
                                         "LabelNetLastHost", "LabelNetRecoveryRecord",
                                         "LabelNetRecoveryStatusTitle", "LabelNetRecoveryStatus",
                                         "ButtonNetRejoin", "ButtonNetCancelRecovery"),
                        "net-files": ("LabelNetAutosave", "LabelNetAutosaveInterval", "LabelNetAutosaveHost",
                                      "LabelNetAutosaveIntervalHost", "LabelNetAutosavesKeptTitle",
                                      "TextNetworkAutosavesKept", "LabelNetAutosavesKeptHint", "LabelNetAutosaveInfo",
                                      "ButtonNetOpenAutosaves", "ButtonNetCopyAutosavesPath",
                                      "TextNetworkDiagDir", "ButtonNetOpenDiagnostics",
                                      "ButtonNetCopyDiagPath", "ButtonNetSaveDiagnostics",
                                      "CheckboxNetworkRecordReplays"),
                        "net-internet": ("TextNetworkDirUrl", "LabelNetDirUrlHint", "TextNetworkDirPin",
                                         "LabelNetDirStatus", "ButtonNetReplays", "ButtonNetConnDetails",
                                         "ButtonNetNatRelay", "LabelNetInternetReason"),
                        "misc-page": MISC_ROWS}[case]
            assert set(expected) <= rows.keys(), (case, sorted(rows))
            if case != "misc-page":
                # One value column across the four pages: every value control and every right-hand
                # control starts on it, and the pages' rows ride the Misc grid's 20px pitch.
                column = rows[f"CollectionBoxNetPage{sub_page.split(':')[1]}"]["rect"][0] + NETWORK_VALUE_COLUMN
                on_column = {
                    "net-chat": ("CheckboxNetworkChatSound", "ComboNetworkChatScope",
                                 "ComboNetworkChatTextSize", "TextNetworkChatKey", "LabelNetMutedReason"),
                    "net-recovery": ("CheckboxNetworkOfferRejoin", "LabelNetLastHost",
                                     "LabelNetRecoveryRecord", "LabelNetRecoveryStatus",
                                     "ButtonNetRejoin"),
                    "net-files": ("LabelNetAutosave", "LabelNetAutosaveInterval", "TextNetworkAutosavesKept",
                                  "ButtonNetOpenAutosaves", "TextNetworkDiagDir",
                                  "ButtonNetOpenDiagnostics", "ButtonNetSaveDiagnostics"),
                    "net-internet": ("TextNetworkDirUrl", "LabelNetDirUrlHint", "LabelNetDirStatus",
                                     "ButtonNetReplays")}[case]
                for name in on_column:
                    assert rows[name]["rect"][0] == column, (name, rows[name]["rect"], column)
                grid_rows = {
                    "net-chat": ("CheckboxNetworkChatVisible", "LabelNetworkChatScope",
                                 "CheckboxNetworkChatNotify", "LabelNetworkChatTextSize",
                                 "LabelNetworkChatKey", "ButtonNetMutedPlayers"),
                    "net-recovery": ("CheckboxNetworkAutoReconnect", "LabelNetLastHostTitle",
                                     "LabelNetRecoveryTitle", "LabelNetRecoveryStatusTitle",
                                     "LabelNetRecoveryError", "ButtonNetRejoin"),
                    "net-files": ("LabelNetAutosaveTitle", "LabelNetAutosaveIntTitle",
                                  "LabelNetAutosavesKeptTitle",
                                  "LabelNetAutosaveInfo", "ButtonNetOpenAutosaves",
                                  "LabelNetDiagDirTitle", "ButtonNetOpenDiagnostics",
                                  "CheckboxNetworkRecordReplays", "LabelNetFilesMessage"),
                    "net-internet": ("LabelNetDirUrl", "LabelNetDirUrlHint", "LabelNetDirPin",
                                     "LabelNetDirStatusTitle", "ButtonNetReplays",
                                     "LabelNetInternetReason", "LabelNetInternetError")}[case]
                deltas = [rows[b]["rect"][1] - rows[a]["rect"][1] for a, b in zip(grid_rows, grid_rows[1:])]
                # The internet pin box's own row sits between its label and the status row; the chat
                # page's muted stub waits one row under its rows.
                expected_pitch = [20, 20, 40, 20, 20, 20] if case == "net-internet" else \
                    [20] * 4 + [40] if case == "net-chat" else [20] * (len(grid_rows) - 1)
                assert deltas == expected_pitch, (case, deltas)
                if case == "net-files":
                    # The folders' action pairs stack in the same two columns, one row pair apart.
                    action_column = rows["CollectionBoxNetPageFiles"]["rect"][0] + NETWORK_ACTION_COLUMN
                    opens = [rows[name]["rect"] for name in ("ButtonNetOpenAutosaves", "ButtonNetOpenDiagnostics")]
                    copies = [rows[name]["rect"] for name in ("ButtonNetCopyAutosavesPath", "ButtonNetCopyDiagPath")]
                    assert all(rect[0] == column for rect in opens) and all(rect[0] == action_column for rect in copies), (opens, copies)
                    assert opens[0][2:] == opens[1][2:] and copies[0][2:] == copies[1][2:], (opens, copies)
                    assert opens[1][1] - opens[0][1] == copies[1][1] - copies[0][1] == 40, (opens, copies)
                    for name in ("LabelNetAutosaveHost", "LabelNetAutosaveIntervalHost"):
                        assert rows[name]["text"] == "Set by the host", rows[name]
                        assert rows[name]["enabled"] is False, rows[name]
                    widths = {rows[name]["rect"][2] for name in FILES_BUTTONS}
                    assert len(widths) == 1, {name: rows[name]["rect"][2] for name in FILES_BUTTONS}
            captioned = [control for control in images[0]["controls"] if control["text"]]
            assert captioned and all("text_fits" in control for control in captioned), "a caption carries no fit measurement"
            result["unfit"] = [control["name"] for control in captioned if control["text_fits"] is False]
            assert not result["unfit"], result["unfit"]
            disabled = {"net-chat": ("ButtonNetMutedPlayers",),
                        "net-recovery": ("ButtonNetRejoin", "ButtonNetCancelRecovery"),
                        "net-internet": ("ButtonNetReplays", "ButtonNetConnDetails", "ButtonNetNatRelay")}.get(case, ())
            for name in disabled:
                assert rows[name]["enabled"] is False, (name, rows[name])
            result["page_text"] = {name: rows[name]["text"] for name in expected if rows[name]["text"]}
            if case == "net-chat":
                assert rows["LabelNetMutedReason"]["text"] == "Muted players are managed in the match.", rows["LabelNetMutedReason"]
                assert rows["ComboNetworkChatScope"]["text"] == CHAT_SEED["NetworkChatDefaultScope"]
                assert rows["ComboNetworkChatTextSize"]["text"] == CHAT_SEED["NetworkChatTextSize"]
                result["saved"] = read_settings(runs["host"].cwd / "Userdata/Settings.ini", set(CHAT_SAVED))
                assert result["saved"] == CHAT_SAVED, result["saved"]
            if case == "net-recovery":
                assert rows["LabelNetRecoveryRecord"]["text"] == "No recovery record", rows["LabelNetRecoveryRecord"]
                result["saved"] = read_settings(runs["host"].cwd / "Userdata/Settings.ini", set(RECOVERY_SAVED))
                assert result["saved"] == RECOVERY_SAVED, result["saved"]
            if case == "net-files":
                assert rows["LabelNetAutosave"]["text"] == "Enabled", rows["LabelNetAutosave"]
                assert rows["LabelNetAutosaveInterval"]["text"] == "45 s", rows["LabelNetAutosaveInterval"]
                assert rows["TextNetworkDiagDir"]["text"] == FILES_SAVED["NetworkDiagnosticsDirectory"], rows["TextNetworkDiagDir"]
                result["saved"] = read_settings(runs["host"].cwd / "Userdata/Settings.ini", set(FILES_SAVED))
                assert result["saved"] == FILES_SAVED, result["saved"]
            if case == "net-internet":
                # The refused pin/URL kept the stored values and the last good commit cleared the reason.
                assert rows["TextNetworkDirUrl"]["text"] == INTERNET_SAVED["SessionDirectoryUrl"], rows["TextNetworkDirUrl"]
                assert rows["TextNetworkDirPin"]["text"] == INTERNET_SAVED["SessionDirectoryCertSha256"], rows["TextNetworkDirPin"]
                assert rows["LabelNetInternetError"]["text"] == "", rows["LabelNetInternetError"]
                result["saved"] = read_settings(runs["host"].cwd / "Userdata/Settings.ini", set(INTERNET_SAVED))
                assert result["saved"] == INTERNET_SAVED, result["saved"]
            if case == "misc-page":
                # The match-status rows are gone from the box itself, not just hidden: a dump names
                # every control the skin carries, and neither name may appear.
                assert not set(MISC_GONE) & rows.keys(), sorted(rows)
                grid = [rows["CheckboxSkipIntro"]["rect"][1], rows["LabelSceneBackgroundAutoScale"]["rect"][1]]
                assert grid[1] - grid[0] == 4 * 20, grid
        if case == "landing" and not failing:
            # A first visit owes the player no reconnect verdict: the record probe's negative stays silent.
            status = next(c for c in images[0]["controls"] if c["name"] == "LabelMultiplayerLandingStatus")
            assert status["text"] == "", status
            prompt = next(c for c in images[0]["controls"] if c["name"] == "LabelMultiplayerNamePrompt")
            assert prompt["text"] == "Multiplayer name:", prompt
            # The over-cap name was refused twice: once on the command line, once at the host's create.
            assert "-net-player-name over the 64-byte cap" in logs["host"], logs["host"][-2000:]
            refused = {c["name"]: c for c in images[-1]["controls"]}
            assert refused["LabelMultiplayerLandingStatus"]["text"] == "Display names are limited to 64 bytes.", \
                refused["LabelMultiplayerLandingStatus"]
        if case in ("lobby", "lobby-name"):
            # The Leave/Seats block is centred on the lobby panel the way Start Match is; doubled
            # centres avoid halves. The Players header starts on its rows' left edge and holds its line.
            drawn = {c["name"]: c for c in images[-1]["controls"]}
            if case == "lobby":
                result["host_options_geometry"] = host_options_geometry(images)
                result["timing_options_geometry"] = timing_options_geometry(images)
            leave, seats, last, panel = (drawn[name] for name in
                                         ("ButtonMultiplayerLeave", "ButtonMultiplayerModerate", "ButtonLobbyOptions",
                                          "MultiplayerLobbyPanel"))
            assert leave["rect"][0] + last["rect"][0] + last["rect"][2] == panel["rect"][0] * 2 + panel["rect"][2], \
                (leave["rect"], last["rect"], panel["rect"])
            header = drawn["LabelLobbyPlayersHeader"]
            seat_rows = [control for control in images[-1]["controls"]
                         if re.fullmatch(r"LabelLobbyPlayer\d", control["name"])]
            assert seat_rows and header["text_fits"] and all(
                row["rect"][0] == header["rect"][0] for row in seat_rows), (header, seat_rows)
            start = drawn["ButtonMultiplayerStart"]
            pair_span = last["rect"][0] + last["rect"][2] - leave["rect"][0]
            pair_gap = seats["rect"][0] - leave["rect"][0] - leave["rect"][2]
            back = next((c for c in drawn.values() if c["name"] == "ButtonBackToMain"), None)
            save = next((c for c in drawn.values() if c["name"] == "ButtonSaveDiagnostics"), None)
            assert pair_span == start["rect"][2], (pair_span, start["rect"], leave["rect"], last["rect"])
            if back and save:
                footer_gap = save["rect"][0] - back["rect"][0] - back["rect"][2]
                assert pair_gap == footer_gap, (pair_gap, footer_gap)
            else:
                assert pair_gap == 8, pair_gap
        if case == "lobby-name":
            assert next(c["text"] for c in images[0]["controls"] if c["name"] == "TextMultiplayerName") == NETWORK_SEED["NetworkDisplayName"]
            result["saved"] = read_settings(runs["host"].cwd / "Userdata/Settings.ini", {"NetworkDisplayName"})
            assert result["saved"] == {"NetworkDisplayName": "Recon7"}, result["saved"]
            # The seeded policy is automatic, so the lobby row must not call the delay fixed.
            result["lobby_row"] = next(c["text"] for c in images[-1]["controls"] if c["name"] == "LabelLobbyPlayer0")
            assert re.search(r'Ping \d+ ms - delay \d+ frames', result["lobby_row"]), result["lobby_row"]
            # The host screen under the seeded policy: the box says auto, and is not an editable count.
            host_setup = {c["name"]: c for c in images[-3]["controls"]}
            assert host_setup["TextHostInputDelay"]["text"] == "auto", host_setup["TextHostInputDelay"]
            assert host_setup["TextHostInputDelay"]["enabled"] is False, host_setup["TextHostInputDelay"]
            assert host_setup["LabelHostInputDelayPolicy"]["text"] == "(auto)", host_setup["LabelHostInputDelayPolicy"]
            assert host_setup["ComboHostActivity"]["rect"][0] == host_setup["TextHostPort"]["rect"][0], (
                host_setup["ComboHostActivity"]["rect"], host_setup["TextHostPort"]["rect"])
            # The disabled delay box's frame is DimRect at 55% of an enabled TextBox frame.
            delay_luma = frame_luma(images[-3]["png"], host_setup["TextHostInputDelay"]["rect"])
            port_luma = frame_luma(images[-3]["png"], host_setup["TextHostPort"]["rect"])
            result["delay_frame_luma"] = delay_luma
            result["port_frame_luma"] = port_luma
            assert port_luma > 0, (delay_luma, port_luma)
            ratio = delay_luma / port_luma
            result["delay_frame_luma_ratio"] = ratio
            assert 0.45 <= ratio <= 0.65, (delay_luma, port_luma, ratio)
            # The disabled Seats control sits in the lobby capture for the visual review.
            assert drawn["ButtonMultiplayerModerate"]["enabled"] is False, drawn["ButtonMultiplayerModerate"]
            ipv4_status = share_status_row(images[-2], options.port)
            ipv6_status = share_status_row(images[-1], options.port, WIDE_SHARE_HOST)
            assert ipv6_status["word_width"] > ipv6_status["row_width"], ipv6_status
            result["share_status"] = {"ipv4": ipv4_status, "ipv6": ipv6_status}
            # The multiplayer screen's panel centres vertically too; an odd height shifts one pixel,
            # and a panel taller than the viewport clamps to its top edge instead of centring.
            screen_rect = drawn["MultiplayerScreen"]["rect"]
            res_y = int(options.size.split("x")[1])
            top, bottom = screen_rect[1], res_y - screen_rect[1] - screen_rect[3]
            if screen_rect[3] <= res_y:
                assert 0 <= bottom - top <= 1, (screen_rect, res_y)
            else:
                assert top == 0, (screen_rect, res_y)
        if case == "host-defaults":
            result["timing_options_geometry"] = timing_options_geometry(images)
        if case == "net-options":
            # Both peers' rosters carry the host's saved options; the client's own copy differs and loses.
            result["match_rules"] = {}
            for who in ("host", "client"):
                report = json.loads((root / f"{who}-match.json").read_text(encoding="utf-8"))
                rules = report["service"]["runner"]["match_config"]["rules"]
                result["match_rules"][who] = {name: rules[name] for name in MATCH_RULES}
                assert result["match_rules"][who] == MATCH_RULES, (who, result["match_rules"][who])
            # The announced delay text is host-authored and synced: the host's post-leave menu loop is
            # the only pump a script gets before a match end quits e2e peers, so it is where the
            # (fixed) branch is read. Each report records the same leave from its own side.
            lobby = [line for line in logs["host"].splitlines() if "[menu-script] dump_lobby" in line]
            assert lobby and 'input_delay="Input delay: 3 (fixed)"' in lobby[-1], lobby
            reports = {who: json.loads((root / f"{who}-match.json").read_text(encoding="utf-8"))
                       for who in ("host", "client")}
            assert reports["host"]["service"]["status"] == "Match left", reports["host"]["service"]["status"]
            assert reports["client"]["service"]["status"] == "The other player left the match", reports["client"]["service"]["status"]
        if case == "net-host-left":
            status = [control["text"] for capture in images if capture["peer"] == "client"
                      for control in capture["controls"] if control["name"] == "LabelMultiplayerLandingStatus"]
            assert status and all("The host left the match" in value and "rematch roster:" not in value for value in status), status
            result["host_departure_status"] = status
        if case == "net-activity":
            # The combo's picked row is what the lobby carries, and both peers read the same
            # preset, module and scene off the wire - the client's label is the proof a bare name never was.
            dumped = {}
            for who, log in logs.items():
                rows = re.findall(
                    r'dump_lobby state=\S+ members=\d+ activity="([^"]*)" module="([^"]*)"'
                    r' scene="([^"]*)" scene_module="([^"]*)"',
                    log)
                assert rows, (who, log[-2000:])
                dumped[who] = rows[-1]
            assert dumped["host"] == dumped["client"], dumped
            preset, module, scene, scene_module = dumped["host"]
            assert module, dumped
            assert scene and scene != "Grasslands", dumped
            result["lobby_activity"] = {"preset": preset, "module": module, "scene": scene,
                                        "scene_module": scene_module, "dumps": dumped}
            host_setup = [image for image in images if image["peer"] == "host"
                          and any(c["name"] == "ComboHostActivity" for c in image["controls"])]
            assert host_setup, "ComboHostActivity missing from host dumps"
            picker = [next(c for c in image["controls"] if c["name"] == "ComboHostActivity") for image in host_setup]
            scenes = [next(c for c in image["controls"] if c["name"] == "ComboHostScene")
                      for image in host_setup if any(c["name"] == "ComboHostScene" for c in image["controls"])]
            assert scenes, "ComboHostScene missing from host dumps"
            assert picker[0]["text"] == "Skirmish Defense - Base.rte" and picker[0]["dropped"] is False, picker[0]
            assert picker[1]["text"] == "P4 Alpha Duel - Base.rte" and picker[1]["dropped"] is True, picker[1]
            assert any(row["text"] == "Brain vs Brain - Base.rte" and row["dropped"] is False for row in picker), picker
            assert any(row["text"] == f"{preset} - {module}" and not row["dropped"] for row in picker), picker
            assert picker[0]["item_count"] > 1, picker[0]
            assert picker[0]["item_count"] == len(picker[0].get("items", [])), picker[0]
            # Every drawn row of the dropped list fits its name room, and only an overlong name is ellipsized.
            for combo in (picker, scenes):
                dropped = next(row for row in combo if row["dropped"])
                items = dropped.get("items")
                assert isinstance(items, list) and items, dropped
                for item in items:
                    room = item["name_room"]
                    display = item["display"]
                    assert item["drawn_width"] <= room, item
                    if item.get("raw_width", 0) > room:
                        assert display.endswith("...") and item["text"].startswith(display[:-3]), item
                assert dropped["fit_width"] == min(dropped["fit_needed"], dropped["fit_clamp"]), dropped
                assert dropped["rect"][2] == dropped["fit_width"], dropped
            # SceneIsCompatible lives on the unfiltered census; the combo is scored against that filter.
            assert_combo_matches_loaded_activities(picker[0], host_setup[0])
            tutorial = next((row for row in (host_setup[0].get("game_activities") or [])
                             if row.get("preset") == "Tutorial Mission"), None)
            if tutorial:
                assert tutorial.get("activity_type") == "GATutorial", tutorial
            table = host_setup[0]["activity_table"]
            assert table and all(row["scenes"] for row in table), table
            brain = next(row for row in table if row["preset"] == "Brain vs Brain" and row["module"] == "Base.rte")
            assert all(entry["name"] != "Grasslands" for entry in brain["scenes"]), brain
            bvb_closed = []
            for image in host_setup:
                controls = {c["name"]: c for c in image["controls"]}
                activity = controls.get("ComboHostActivity") or {}
                scene_row = controls.get("ComboHostScene")
                if (activity.get("text") == "Brain vs Brain - Base.rte" and activity.get("dropped") is False
                        and scene_row and not scene_row["dropped"]):
                    bvb_closed.append(scene_row)
            assert bvb_closed, "no closed Brain vs Brain scene dump"
            brain_scene = bvb_closed[0]
            listed = [entry["name"] + (f" - {entry['module']}" if sum(1 for other in brain["scenes"] if other["name"] == entry["name"]) > 1
                                       else "") for entry in brain["scenes"]]
            assert combo_item_names(brain_scene) == listed, (combo_item_names(brain_scene), listed)
            assert "Grasslands" not in combo_item_names(brain_scene), brain_scene
            assert any(row["dropped"] is True for row in scenes), scenes
            assert any(row["dropped"] is True for row in picker), picker
            assert len(bvb_closed) >= 2, bvb_closed
            assert bvb_closed[-1]["text"] and bvb_closed[-1]["text"] != bvb_closed[0]["text"], bvb_closed
            assert "Grasslands" not in bvb_closed[-1]["text"], bvb_closed[-1]
            assert all(row["text"] == "Brain vs Brain - Base.rte" or row["dropped"] for row in picker[2:]), picker
            result["picker_cycle"] = picker
            result["scene_cycle"] = scenes
            picked_scene = bvb_closed[-1]["text"]
            result["picked_scene"] = picked_scene
            key_scene = combo_name(picked_scene)
            assert scene == key_scene, (scene, key_scene, dumped)
            modes = [next(c for c in image["controls"] if c["name"] == "ComboHostMode")
                     for image in images if image["peer"] == "host"
                     and any(c["name"] == "ComboHostMode" for c in image["controls"])]
            assert any(row["text"] == "Co-op PvE" for row in modes), modes
            result["mode_cycle"] = modes
            # The two header rows carry the friendly mode label, and both peers' panels are the
            # same rectangle for the same lobby state - no peer's own status text widens its panel.
            panels = {}
            for who in ("host", "client"):
                matches = [image for image in images if image["peer"] == who
                           and any(c["name"] == "LabelLobbyMatchMode" for c in image["controls"])]
                assert len(matches) == 1, (who, [image["json"] for image in matches])
                shot = matches[0]
                assert [shot["activity_preset"], shot["activity_module"]] == [preset, module], shot["json"]
                assert shot.get("scene_name") == key_scene, (who, shot.get("scene_name"), key_scene)
                controls = {c["name"]: c for c in shot["controls"]}
                assert controls["LabelLobbyMatch"]["text"] == f"{preset} - {module}", (who, controls["LabelLobbyMatch"])
                assert controls["LabelLobbyMatchMode"]["text"] == key_scene + " - Co-op PvE", (who, controls["LabelLobbyMatchMode"], key_scene)
                assert "Grasslands" not in controls["LabelLobbyMatchMode"]["text"], (who, controls["LabelLobbyMatchMode"])
                for name in ("LabelLobbyMatch", "LabelLobbyMatchMode", "LabelLobbyPlayersHeader"):
                    assert controls[name]["text_fits"] is True, (who, name, controls[name])
                panels[who] = controls["MultiplayerLobbyPanel"]["rect"]
            assert panels["host"] == panels["client"], panels
            result["lobby_panel_rects"] = panels
            result["key_committed"] = {"preset": preset, "scene": key_scene, "combo": picked_scene}
        if case == "input":
            assert next(c["text"] for c in images[0]["controls"] if c["name"] == "TextMultiplayerName") == "ab"
        if case == "input-parity":
            assert len(images) == 4, len(images)
            projected = [[{key: control[key] for key in ("name", "rect", "text", "enabled", "visible")}
                          for control in capture["controls"]] for capture in images]
            assert all(value == projected[0] for value in projected[1:]), "activation routes produce different controls"
        result["pass"] = True
    except Exception as error:
        result["error"] = str(error)
    finally:
        for run in runs.values():
            run.close()
        result["captures"] = images
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--case", choices=(*CASES, "all"), required=True)
    parser.add_argument("--size", choices=("640x360", "960x540", "1280x720", "1920x1080"), required=True)
    parser.add_argument("--all-sizes", action="store_true",
                        help="also run every SIZE_GATES row; net-chat and lobby-name always do this")
    parser.add_argument("--port", type=int, required=True)
    options = parser.parse_args()
    if Path("D:/mx/LEAD_FAMILY.lock").exists():
        parser.error("LEAD_FAMILY.lock exists; no engine launch")
    if not any(low <= options.port <= low + 9 for low in (48270, 48380, 48390, 48530, 48540, 48550, 48840, 48850, 49180, 49190)):
        parser.error("this detector owns ports 48270-48279, 48380-48389, 48390-48399, 48530-48539, 48540-48549, 48550-48559, 48840-48849, 48850-48859 and 49180-49199")
    options.repo = options.repo.resolve()
    options.out.mkdir(parents=True, exist_ok=False)
    options.revision = subprocess.check_output(["git", "-C", str(options.repo), "rev-parse", "HEAD"], text=True).strip()
    options.exe_sha = sha(options.repo / "Cortex Command.exe")
    requested = options.size

    def sizes_for(case):
        sizes = [requested]
        if options.all_sizes or case in ("net-chat", "lobby-name"):
            sizes.extend(size for name, size in SIZE_GATES if name == case and size not in sizes)
        return sizes

    rows = []
    for case in CASES if options.case == "all" else (options.case,):
        if case == "oracles":
            options.size = requested
            for name, command in {"visible": (LANDING, "", "assert_visible ButtonMultiplayerHostGame 0"),
                                  "focus": (LANDING, "", "assert_focus ButtonMultiplayerJoinGame"),
                                  "rect": (LANDING, "", "assert_rect_inside ButtonMultiplayerHostGame ButtonMultiplayerJoinGame"),
                                  "text": (LANDING, "settext TextMultiplayerName " + "W" * 200 + "\n", "assert_text_fits TextMultiplayerName"),
                                  "page": (OPTIONS, "select_settings_page Misc\nwait 3\n", "assert_settings_page Gameplay"),
                                  "page-name": (OPTIONS, "", "select_settings_page Nowhere")}.items():
                rows.append(run_case(options, "landing", options.out / f"oracle-{name}" / options.size, command))
            continue
        for size in sizes_for(case):
            options.size = size
            rows.append(run_case(options, case, options.out / case / size))
    result = {"pass": all(row["pass"] for row in rows), "driver_sha256": sha(__file__),
              "source_revision": options.revision, "exe_sha256": options.exe_sha, "port": options.port, "cases": rows}
    (options.out / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    (options.out / "captures.json").write_text(json.dumps([image for row in rows for image in row["captures"]], indent=2) + "\n", encoding="utf-8")
    print(f"[menu-readback] {'PASS' if result['pass'] else 'FAIL'} {options.out / 'result.json'}")
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
