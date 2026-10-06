"""Read real menu controls and drive scoped input through private engine runs."""

import argparse
import ctypes
import hashlib
import json
import os
import re
import subprocess
import sys
import threading
import time
import traceback
from pathlib import Path

from PIL import Image

from run_sim_test import make_run, engine_executable, file_sha256
from test_lobby_lifecycle import wait_for_log
from test_telemetry_bundle import set_visual_resolution

try:
    import spread_peers as spread
except ModuleNotFoundError as error:
    if error.name != "spread_peers":
        raise
    spread = None

managed_case = spread.managed_case if spread else lambda function: function


CASES = ("landing", "settings", "pages", "combo-fit", "lobby", "pause", "pause-save", "save-hotkey", "live", "input", "input-parity", "disabled",
         "scope-off", "network", "net-chat", "net-recovery", "net-files", "net-internet", "misc-page",
         "lobby-name", "net-options", "net-activity", "net-host-left", "net-host-left-early", "net-resume", "host-defaults", "host-stun", "host-stun-empty", "host-relay", "net-connection", "world-open-seat", "repair", "local-end-match", "prehost-visibility", "host-by-hand", "host-follows-activity", "lobby-ready-all", "lobby-countdown", "lobby-last-ready", "lobby-ready-back",
         "lobby-seat-missing", "lobby-escape", "host-one-draft", "host-hand-open",
         "sweep-landing", "sweep-host", "sweep-join", "sweep-lobby", "sweep-advanced-setup", "sweep-advanced-lobby", "sweep-advanced-client", "sweep-settings-network", "sweep-browsers",
         "host-draft-roundtrip", "host-draft-apply", "host-apply-all", "lobby-setup-unready", "lobby-setup-open", "lobby-long-names", "host-words",
         "host-words-nocrypto", "host-long-names", "port-by-hand", "old-skins-public", "old-skins-original", "sweep-fault", "oracles")
THIRD_PASS_CASES = ("host-draft-roundtrip", "host-draft-apply", "host-apply-all", "lobby-setup-unready", "lobby-setup-open", "lobby-long-names", "host-words",
                    "host-words-nocrypto", "host-long-names", "port-by-hand", "old-skins-public", "old-skins-original")
PAIRED_CASES = ("pause", "pause-save", "save-hotkey", "repair", "live", "net-options", "net-activity", "local-end-match", "net-host-left", "net-host-left-early",
                "lobby-ready-all", "lobby-countdown", "lobby-last-ready", "lobby-ready-back", "lobby-seat-missing", "sweep-advanced-client",
                "host-draft-roundtrip", "lobby-setup-unready", "lobby-setup-open", "lobby-long-names")
# These two UI cases keep their one original peer; the shared executor records its native box.
SPREAD_CASES = (*PAIRED_CASES, "net-chat", "lobby-name")
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
# The first view: the name, the Connection summary and its Change, the chat switches, the status display, then Advanced settings.
BASICS_ROWS = ("LabelNetworkDisplayName", "TextNetworkDisplayName", "LabelNetworkConnectionSummary", "LabelNetworkConnectionValue",
               "ButtonNetworkConnectionChange", "CheckboxNetworkChatVisible", "CheckboxNetworkChatNotify", "LabelMatchStatusWidget",
               "ComboMatchStatusWidget", "ButtonNetworkAdvanced", "LabelNetworkAdvancedHint")
NETWORK_ROWS = ("LabelNetworkDelayPolicy",
                "RadioNetworkDelayAuto", "RadioNetworkDelayFixed", "LabelNetworkIdleWait",
                "TextNetworkIdleWait", "LabelNetworkIdleWaitHint", "LabelNetworkPathHorizon",
                "TextNetworkPathHorizon", "LabelNetworkPathHorizonHint", "CheckboxNetworkAutoRepair",
                "CheckboxNetworkToasts", "CheckboxNetworkPrediction", "CheckboxNetworkDiagnostics")
NETWORK_FIXED_ROWS = ("LabelNetworkFixedDelay", "TextNetworkFixedDelay", "LabelNetworkFixedDelayHint")
MAX_INPUT_DELAY_FRAMES = 60  # NetMatchConfigUtil::c_MaxInputDelayFrames, which the hint states.
LOBBY_PORT_MAP_ROW = 14  # MainMenuGUI's port-map row: the host alone maps its router port, so only its lobby carries it.
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
NAT_LABEL = "Automatic direct connection"
NAT_STATES = ("On (default)", "Off")
# The row's first line for each drafted state; the lines under it say what this computer's settings mean for it.
NAT_HINTS = {"On (default)": "Tries a direct connection through each player's router first. Recommended.",
             "Off": "Players reach you only at your public address and port; many home networks cannot."}
# With no STUN server to ask, On reaches this network only, and its first line says so.
NAT_STUN_EMPTY = "The STUN server list is empty, so only players on your network connect directly (Settings - Network - Connection)."
RELAY_STATES = ("Off", "Game service (default)", "Custom relay")
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
PICTURE_WATCHES = "picture-layout layout always\npicture-overlap overlap always\n"
SIZE_GATES = (
    *((case, size) for case in ("lobby", "host-defaults", "host-stun", "host-stun-empty", "host-relay", "net-connection", "world-open-seat", "repair", "pause", "pause-save", "network", "net-host-left", "net-host-left-early")
      for size in ("640x360", "960x540", "1280x720")),
    ("net-chat", "960x540"),
    ("net-chat", "1280x720"),
    ("save-hotkey", "960x540"),
    ("lobby-name", "640x360"),
    ("lobby-name", "960x540"),
    ("lobby-name", "1280x720"),
    ("lobby-name", "1920x1080"),
    ("live", "1280x720"),
    # The start countdown pictured at the second size too.
    ("lobby-countdown", "1280x720"),
    # Every screen's controls by hand at the three sizes and in the 2560x1440 window a 960x540 screen is shown in.
    *((case, size) for case in CASES if case.startswith("sweep-") for size in ("640x360", "1280x720", "960x540@2.6667")),
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
INTERNET_REASON = "Connection sets your route. Host a Game > Advanced > Connection sets the match's relay."
# The wire's display-name cap; the landing name box and -net-player-name refuse past it.
DISPLAY_NAME_MAX_BYTES = 64
# FontSmall measured 125 glyphs of this alphabet at 502 px. Two hundred stay one token and are wider
# than the 640x360 roster column (ResX - 28) while the box is still capped by the screen.
WIDE_ROSTER_NAME = ("PeerExtremelyLongDisplayNameForWrapChecking" + "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789" * 5)[:200]
# 200 of those glyphs measured 807 px. 340 exceeds the 960 and 1280 status columns (ResX - 28).
WIDE_STATUS_TOKEN = (WIDE_ROSTER_NAME + "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789" * 5)[:340]
# The host's saved session options steer the match; the client's own copy differs and must not.
HOST_OPTIONS = {"NetworkSlowPlayerBoundTicks": "7", "NetworkSlowPlayerPolicy": "Pause", "NetworkHostDelayPolicy": "Fixed", "NetworkHostIdleWaitMinutes": "25", "NetworkHostAutoRepair": "0",
                "NetworkPathHorizonTicks": "45"}
CLIENT_OPTIONS = {"NetworkSlowPlayerBoundTicks": "3", "NetworkSlowPlayerPolicy": "Substitute", "NetworkHostDelayPolicy": "Auto", "NetworkHostIdleWaitMinutes": "5", "NetworkHostAutoRepair": "1",
                  "NetworkPathHorizonTicks": "15"}
# The host's saved Pause plays the default policy (1): V1 offers only it, whatever a setting saved.
MATCH_RULES = {"delay_policy": 2, "idle_wait_minutes": 25, "automatic_repair": False, "path_horizon_ticks": 45,
               "slow_player_bound_ticks": 7, "slow_player_policy": 1}
# A combo box draws its selected item left of the drop-down button, so its text budget is narrower than its rect.
COMBO_BUTTON = 17
FIT_LINE = re.compile(r"assert_text_fits (\w+).*?rect=\[(-?\d+),(-?\d+),(-?\d+),(-?\d+)\].*?available=\[(-?\d+),(-?\d+)\]")
WATCHED = ("ComboBrainlessHumansSpectate", "ComboMatchStatusWidget")
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
        return file_sha256(stream)


def version_line():
    """The line the main menu and the lobby show: the game's version, VERSION.txt's and the network protocol's, read from this tree."""
    tree = Path(__file__).resolve().parents[1]
    game = re.search(r'c_VersionString = "([^"]+)"', (tree / "Source/System/GameVersion.h").read_text(encoding="utf-8"))[1]
    protocol = re.search(r"c_Version = (\d+);", (tree / "Source/Network/NetProtocol.h").read_text(encoding="utf-8"))[1]
    return f"v{game}, multiplayer {(tree / 'VERSION.txt').read_text(encoding='utf-8').strip()} (protocol {protocol})"


def max_future_frame_skew():
    """NetLockstepCodec::c_MaxFutureFrameSkew from the header under test: how far a held seat's floor reaches back before its hold."""
    header = (Path(__file__).resolve().parents[1] / "Source/Network/NetLockstep.h").read_text(encoding="utf-8")
    factor = re.search(r"c_MaxFutureFrameSkew = (\d+)ULL \* c_MaxInputDelayFrames;", header)
    delay = re.search(r"c_MaxInputDelayFrames = (\d+);", header)
    if not factor or not delay:
        raise RuntimeError("NetLockstep.h no longer names c_MaxFutureFrameSkew as a multiple of c_MaxInputDelayFrames")
    return int(factor[1]) * int(delay[1])


def host_hint(name):
    """A consequence the host options page shows, read from its owner, Source/Menus/NetHostOptionsText.h."""
    header = (Path(__file__).resolve().parents[1] / "Source/Menus/NetHostOptionsText.h").read_text(encoding="utf-8")
    found = re.search(r"inline const char\* " + name + r"\(\) \{\s*return \"([^\"]*)\";", header)
    if not found:
        raise RuntimeError(f"NetHostOptionsText.h names no {name}()")
    return found[1]


# The policy the host runs once the Recovery rows pick a 10-minute return window, 10 minutes of history and a 1-minute limit: the window
# plus the frames its hold reaches back is longer than the history, so the window sets the retention.
JOIN_HISTORY_POLICY = re.compile(r"\[round-history\] host options world_history_s=600 catch_up_limit_s=60 return_window_min=10 "
                                 r"retain_frames=(\d+) retain_by=return_window lag_limit_frames=3600")


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
    if case in ("prehost-visibility", "host-by-hand"):
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
    if case == "net-host-left-early":
        # The input barrier below is measured from a fixed delay, so the negotiated floor is pinned.
        return {"host": {"NetworkInputDelayFrames": "1"}, "client": {"NetworkInputDelayFrames": "1"}}
    return {}


def host_lobby(port):
    return (LANDING + "activate ButtonMultiplayerHostGame\nwait 5\n"
            f"setup_host_port {port}\ncombo_select ComboHostPlayers 2\n"
            "activate ButtonMultiplayerCreate\nwait 15\nassert_substate Lobby\n")


def net_page(page):
    """The reach a script has into the network page's own selector: the tab name it would click. The pages past the first view
    show once Advanced settings is pressed, the way a player opens them."""
    text = "select_settings_page Network\nwait 3\nassert_settings_page Network\nselect_settings_page Network:Basics\nwait 3\n"
    if page != "Basics":
        text += "activate ButtonNetworkAdvanced\nwait 4\n"
    return (text + f"select_settings_page Network:{page}\nwait 3\nassert_settings_page Network:{page}\n"
            f"assert_visible CollectionBoxNetPage{page} 1\n")


def menu_step(command):
    return {"op": "menu", "command": command}


def hand_pick(combo, item):
    """A hand's pick from a drop-down, read back frames after the release."""
    return f"combo_select {combo} {item}\nwait 4\nassert_label {combo} {item}\n"


def hand_click(control):
    """A hand's click, frames before the next step."""
    return f"activate {control}\nwait 4\n"


def roster_fit_observations(observation):
    found = []
    for step in observation.get("steps", []):
        note = step.get("observed", {}).get("menu_observation", "")
        if isinstance(note, str) and '"surface":"roster_fits"' in note:
            found.append(json.loads(note))
    return found


def status_wrap_notes(observation):
    notes = []
    for step in observation.get("steps", []):
        note = step.get("observed", {}).get("menu_observation", "")
        if isinstance(note, str) and (note == "status: no wrap surface" or '"surface":"status"' in note):
            notes.append(note)
    return notes


def paired_round_ticks(case):
    """How many ticks a paired case's round runs: long enough for what its probes walk through before its end."""
    return 2400 if case == "repair" else 1200 if case in ("pause", "pause-save", "save-hotkey") else 400


def probe_root(root, who):
    """One directory per peer's probe: the engine writes its result beside the script it was handed."""
    return root / f"{who}_probe"


def ntdll():
    """ntdll's process suspension, loaded on first use: it is the one piece of this driver only Windows has."""
    return ctypes.WinDLL("ntdll")


def suspend_run(run):
    """Freeze every thread of a live run's process; a suspended peer is absent to the wire but its
    failure path carries no fixture error of its own the way an in-engine hold would."""
    if hasattr(run, "suspend"):
        return run.suspend()
    if run.process is None:
        raise RuntimeError("suspend asked for a process that has not started")
    if ntdll().NtSuspendProcess(ctypes.c_void_p(run.process)) != 0:
        raise RuntimeError("NtSuspendProcess failed")


def resume_run(run):
    if hasattr(run, "resume"):
        return run.resume()
    if ntdll().NtResumeProcess(ctypes.c_void_p(run.process)) != 0:
        raise RuntimeError("NtResumeProcess failed")


def unavailable_reason(case, platform=None):
    """Why this platform cannot drive a case, or None. The case is refused with that reason as its verdict."""
    if (platform or os.name) != "nt" and case == "net-host-left-early" and not (spread and spread.enabled()):
        return "suspends the client mid-launch through ntdll's NtSuspendProcess on the win32 runner's process handle (Windows only)"
    return None


def unavailable_row(case, size, reason):
    return {"pass": None, "verdict": "unavailable", "case": case, "size": size, "reason": reason, "captures": []}


def summarize(rows):
    """The driver's verdict: the rows that ran decide it; a refused row is named beside it, never counted as run."""
    refused = [f"{row['case']}/{row['size']}: {row['reason']}" for row in rows if row.get("verdict") == "unavailable"]
    ran = [row for row in rows if row.get("verdict") != "unavailable"]
    passed = bool(ran) and all(row["pass"] for row in ran)
    return passed, ("PASS" if passed else "FAIL" if ran else "UNAVAILABLE"), refused


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
        # beat the running-match assertions. Gate the leave on a later sim tick; the round runs 1200
        # ticks so the host's page tour never meets its end first. The menu pump that follows the leave still serves the
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
                  {"op": "wait", "service": "Completed", "scope": "menu"},
                  {"op": "signal", "name": "left", "scope": "menu"}]
    else:
        # ENGINE 200: End Match is the host's row only while the round runs. The enabled state is re-derived
        # from the live service on every pause-menu Update and the button is drawn by that same pass, so the
        # host reads it live here and the capture's recorded pause rows carry the state after completion.
        # The host keeps its probe running until the client has left: a host that stops probing plays at full rate while the
        # probing client cannot, and the round then holds that client as a slow player before it reaches Leave.
        steps += [menu_step("assert_enabled ButtonEndMatch 1"),
                  {"op": "assert", "equals": {"service": "Running"}},
                  {"op": "signal", "name": "done"},
                  {"op": "wait_file", "path": str(probe_root(root, "client") / "left.json")}]
    steps += [{"op": "finish"}]
    return {"schema": 1, "timeout_ms": 90000, "steps": steps}


def pause_save_probe(who, root):
    """The match pause menu's save row: the host saves the match from it; a client reads who saves and when it last did."""
    steps = [
        {"op": "wait", "sim_at_least": 150},
        {"op": "key_down", "key": "Escape"}, {"op": "key_up", "key": "Escape"},
        {"op": "wait", "screen": "Pause"}, menu_step("dump_host_options"),
        *row_checks("ButtonSaveMatch", "PauseScreen"), *row_checks("LabelSaveMatchHint", "PauseScreen"),
        menu_step(f"assert_enabled ButtonSaveMatch {1 if who == 'host' else 0}")]
    if who == "host":
        steps += [menu_step("assert_label LabelSaveMatchHint Not saved yet"),
                  menu_step("activate ButtonSaveMatch"),
                  {"op": "wait", "scope": "menu", "control": "LabelSaveMatchHint", "text_contains": "Last saved at"},
                  menu_step("dump_host_options"),
                  {"op": "signal", "name": "done"},
                  {"op": "wait_file", "path": str(probe_root(root, "client") / "done.json")}]
    else:
        # The client's own writer takes the same capture, so its row reads the save once that archive is written.
        steps += [menu_step("assert_label LabelSaveMatchHint The host saves the match"),
                  {"op": "wait_file", "path": str(probe_root(root, "host") / "done.json")},
                  {"op": "wait", "scope": "menu", "control": "LabelSaveMatchHint", "text_contains": "last saved at"},
                  menu_step("dump_host_options"),
                  {"op": "signal", "name": "done"}]
    steps += [{"op": "finish"}]
    return {"schema": 1, "timeout_ms": 90000, "steps": steps}


def save_hotkey_probe(who, root):
    """F5 in a running match: the host's press saves the match for every peer and says so on its toast line."""
    steps = [{"op": "wait", "sim_at_least": 150}]
    if who == "host":
        steps += [{"op": "key_down", "key": "F5"}, {"op": "key_up", "key": "F5"}, {"op": "wait", "renders": 120},
                  {"op": "signal", "name": "done"},
                  {"op": "wait_file", "path": str(probe_root(root, "client") / "done.json")}]
    else:
        steps += [{"op": "wait_file", "path": str(probe_root(root, "host") / "done.json")},
                  {"op": "signal", "name": "done"}]
    steps += [{"op": "finish"}]
    return {"schema": 1, "timeout_ms": 90000, "steps": steps}


def host_activity_label(row, rows=()):
    """The host screen names an activity's module only to tell apart two activities of one name."""
    module = row.get("module") or ""
    shared = sum(other["preset"] == row["preset"] for other in rows) > 1
    return row["preset"] + (f" - {module}" if module and shared else "")


def repair_probe(who, root, roomy=True):
    steps = [{"op": "wait", "service": "Running", "sim_at_least": 150},
             {"op": "key_down", "key": "F6"}, {"op": "key_up", "key": "F6"},
             {"op": "wait", "panel_open": True},
             {"op": "mouse_down", "control": "NetworkSeatsOptions"},
             {"op": "mouse_up", "control": "NetworkSeatsOptions"},
             {"op": "wait", "control": "NetworkSeatsOptionsText", "equals": {"visible": True}},
             {"op": "assert_control", "control": "NetworkSeatsOptionsText", **({"fits": True} if roomy else {}),
              "text_contains": "Repair match: Ready - pause menu > Match Options" if who == "host" else "Frame redundancy:"},
             *([] if roomy else [menu_step("assert_vertical_scroll NetworkSeatsOptionsText")]),
             {"op": "key_down", "key": "F6"}, {"op": "key_up", "key": "F6"},
             {"op": "wait", "panel_open": False},
             {"op": "key_down", "key": "Escape"}, {"op": "key_up", "key": "Escape"},
             {"op": "wait", "screen": "Pause"}, {"op": "wait", "renders": 2}, menu_step("activate ButtonMatchOptions"),
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
    rows = allowed_host_activities(dump)
    allowed = [host_activity_label(row, rows) for row in rows]
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
        row_checks += checks(control, "CollectionBoxHostPageConnection")
    # This computer's choice commits with Apply and stands when Advanced opens again.
    reopen = ("activate ButtonHostOptApply\nwait 3\nactivate ButtonHostOptBack\nwait 3\nactivate ButtonHostOptions\nwait 3\n"
              "activate TabHostPageConnection\nwait 3\n")
    text = (LANDING + "activate ButtonMultiplayerHostGame\nwait 5\n"
            f"setup_host_port {port}\nactivate ButtonHostOptions\nwait 3\n"
            "activate TabHostPageConnection\nwait 3\n" + row_checks +
            f"assert_label ComboHostNetIce {NAT_STATES[0]}\ndump_host_options\n")
    for state in (NAT_STATES[1], NAT_STATES[0]):
        text += (f"combo_select ComboHostNetIce {state}\nwait 3\n" + reopen + row_checks +
                 f"assert_label ComboHostNetIce {state}\ndump_host_options\n")
    # A live lobby keeps the routes it opened with: Apply refuses the change with the way to make it, and Cancel restores the page.
    text += (f"combo_select ComboHostNetIce {NAT_STATES[1]}\nwait 3\n" + reopen +
             "activate ButtonHostOptBack\nwait 3\nactivate ButtonMultiplayerCreate\nwait 15\n"
             "activate ButtonLobbyOptions\nwait 3\nactivate TabHostPageConnection\nwait 3\n"
             f"combo_select ComboHostNetIce {NAT_STATES[0]}\nwait 3\nactivate ButtonHostOptApply\nwait 3\n"
             "assert_label LabelHostOptStatus Close this lobby to change the direct connection.\n" + row_checks +
             "activate ButtonHostOptBack\nwait 3\nactivate ButtonLobbyOptions\nwait 3\nactivate TabHostPageConnection\nwait 3\n"
             f"assert_label ComboHostNetIce {NAT_STATES[1]}\ndump_host_options\nexit\n")
    return text


def host_relay_readback(port):
    text = (LANDING + "activate ButtonMultiplayerHostGame\nwait 5\n"
            f"setup_host_port {port}\nactivate ButtonHostOptions\nwait 3\n"
            "activate TabHostPageConnection\nwait 3\n"
            f"assert_label LabelHostNetRelay Relay fallback\nassert_label ComboHostNetRelay {RELAY_STATES[1]}\n"
            f"assert_label ComboHostNetIce {NAT_STATES[0]}\ndump_host_options\n")
    for state in (RELAY_STATES[0], RELAY_STATES[2], RELAY_STATES[1]):
        text += f"combo_select ComboHostNetRelay {state}\nwait 3\n"
        text += checks("LabelHostNetRelay", "CollectionBoxHostPageConnection")
        text += checks("ComboHostNetRelay", "CollectionBoxHostPageConnection")
        text += checks("LabelHostRelayHint", "CollectionBoxHostPageConnection")
        if state == RELAY_STATES[1]:
            text += "assert_label LabelHostRelayHint login renews itself while the session runs\n"
        if state == RELAY_STATES[2]:
            for suffix, value in (("Address", "relay.example:3478"), ("User", "fixed-user"), ("Pass", "fixed-password")):
                text += f"set_text TextHostRelay{suffix} {value}\nwait 3\n"
                text += checks("TextHostRelay" + suffix, "CollectionBoxHostPageConnection")
            text += "assert_label TextHostRelayPass **************\nassert_label LabelHostRelayHint never a signing secret\n"
        # This computer's choice commits with Apply and stands when Advanced opens again.
        text += ("activate ButtonHostOptApply\nwait 3\nactivate ButtonHostOptBack\nwait 3\nactivate ButtonHostOptions\nwait 3\n"
                 "activate TabHostPageConnection\nwait 3\n"
                 f"assert_label ComboHostNetRelay {state}\ndump_host_options\n")
    return text + "activate ButtonHostOptBack\nwait 3\nexit\n"


def connection_readback():
    text = OPTIONS + net_page("Connection")
    text += "assert_label LabelNetworkConnection Connection\nassert_label ComboNetworkConnection Automatic\n"
    text += "assert_label LabelNetworkStunServers STUN server list\n"
    text += f"assert_label TextNetworkStunServers {STUN_DEFAULT}\n"
    # The player's relay hint makes the host options hint's claim: UDP only, a self-renewing directory login.
    text += "assert_label LabelNetworkRelayHint UDP TURN only; no TCP/TLS relays. A host's directory login renews while the session runs.\n"
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


# The base screens' words for the same steps, so a case written for these screens runs on the base build for its RED.
BASE_WORDS = False
BASE_TRANSLATION = (("setup_host_port ", "settext TextHost" + "Port "), ("combo_select ComboHostPlayers ", "settext TextHostPlayers "),
                    ("activate ButtonJoinByAddress\nwait 4\n", ""), ("activate ButtonJoinAddressGo", "activate ButtonMultiplayer" + "Connect"),
                    ("Players versus AI", "Co-op PvE"), ("Players versus players", "PvP"), ("more players to join", "players to join"),
                    ("more player to join", "player to join"), ("wait_substate Lobby 30\n", ""),
                    ("activate ButtonHostOptions\n", "hand_press ButtonHostOptions\nwait 2\nhand_release ButtonHostOptions\n"),
                    ("activate TabHostPageTiming\n", "activate TabHostPageNetwork\nwait 3\nactivate TabHostNetTuning\n"))


def in_base_words(text):
    for new, old in BASE_TRANSLATION:
        text = text.replace(new, old)
    # The base hand has no marks: leaving one is dropped, and waiting for one is a fixed wait long enough for the other engine.
    text = re.sub(r"(?m)^touch_file \S+\n", "", text)
    return re.sub(r"(?m)^wait_file (\S+) \d+\n", lambda match: f"wait_ms {12000 if match[1].endswith('readied.mark') else 40000}\n", text)


def host_lobby(port, players=2, mode=None):
    """A host's way to an open lobby: Host a Game, the rows it changes, Create Lobby."""
    text = LANDING + "activate ButtonMultiplayerHostGame\nwait_ms 400\nassert_substate HostSetup\n" + f"setup_host_port {port}\n"
    if players != 2:
        text += f"combo_select ComboHostPlayers {players}\nwait 4\n"
    if mode:
        text += f"combo_select ComboHostMode {mode}\nwait 4\n"
    return text + "activate ButtonMultiplayerCreate\nwait 15\nassert_substate Lobby\n"


def join_by_address(port):
    """A joiner's way in by the host's address: Join a Game, Join by address, the address and port, Join."""
    return (LANDING + "activate ButtonMultiplayerJoinGame\nwait_ms 400\nassert_substate JoinSetup\n"
            "activate ButtonJoinByAddress\nwait 4\n" + f"settext TextJoinAddress 127.0.0.1\nsettext TextJoinPort {port}\n"
            "activate ButtonJoinAddressGo\nwait_connected 2 60\nwait_substate Lobby 30\nwait 5\n")


def lobby_case(case, port, root):
    """The lobby's Ready and Start as the user decided them, and its Escape, on real engines."""
    marks = {name: probe_root(root, "host") / f"{name}.mark" for name in ("saw", "cancelled", "cleared", "readied", "host_saw_ready", "unreadied", "done")}
    if case == "lobby-ready-all":
        # The host starts once the joined player has read that it is ready, so that line is seen before the round begins.
        host = (host_lobby(port) + f"wait_remote_ready 60\nwait_file {marks['readied']} 30\nwait 3\n"
                "wait_label LabelMultiplayerStatus Everyone is ready - press Start Match\nassert_label ButtonMultiplayerStart Start Match\n"
                "activate ButtonMultiplayerStart\nwait_state Running 20\nexit\n")
        client = (join_by_address(port) + "activate ButtonMultiplayerReady\nwait 4\nassert_label ButtonMultiplayerReady Cancel Ready\n"
                  f"wait_label LabelMultiplayerStatus You're ready\ntouch_file {marks['readied']}\nwait_state Running 20\nexit\n")
    elif case == "lobby-countdown":
        # The joined player counts once the lobby says Start Match would count down for that player's Ready; Start is live from then.
        host = (host_lobby(port) + "wait_connected 2 60\nwait_label LabelMultiplayerStatus Start Match starts in 30 s\n"
                "assert_enabled ButtonMultiplayerStart 1\n"
                "activate ButtonMultiplayerStart\nwait_label LabelMultiplayerStatus Starting in\nassert_label ButtonMultiplayerStart Cancel Start\n"
                # The count as the host sees it, Cancel Start in place of Start Match, pictured.
                f"dump_host_options\nwait_file {marks['saw']} 30\nactivate ButtonMultiplayerStart\nwait_label LabelMultiplayerStatus Start Match starts in 30 s\n"
                f"assert_label ButtonMultiplayerStart Start Match\ntouch_file {marks['cancelled']}\nwait_file {marks['cleared']} 30\n"
                "activate ButtonMultiplayerStart\nwait_label LabelMultiplayerStatus Starting in\nwait_state Running 50\nexit\n")
        client = (join_by_address(port) + "wait_label LabelMultiplayerStatus The host is starting the match in\n"
                  f"wait_label LabelMultiplayerStatus press Ready\ndump_host_options\ntouch_file {marks['saw']}\nwait_file {marks['cancelled']} 30\n"
                  f"wait_label LabelMultiplayerStatus Press Ready when you're ready to play\ntouch_file {marks['cleared']}\n"
                  "wait_label LabelMultiplayerStatus The host is starting the match in\nwait_state Running 50\nexit\n")
    elif case == "lobby-last-ready":
        # The last Ready ends the count: the round starts well before its thirty seconds.
        host = (host_lobby(port) + "wait_connected 2 60\nwait_label LabelMultiplayerStatus Start Match starts in 30 s\n"
                "activate ButtonMultiplayerStart\nwait_label LabelMultiplayerStatus Starting in\n"
                "wait_state Running 20\nexit\n")
        client = (join_by_address(port) + "wait_label LabelMultiplayerStatus The host is starting the match in\n"
                  "activate ButtonMultiplayerReady\nwait_state Running 20\nexit\n")
    elif case == "lobby-ready-back":
        host = (host_lobby(port) + "wait_remote_ready 60\nwait_label LabelMultiplayerStatus Everyone is ready\n"
                f"touch_file {marks['host_saw_ready']}\nwait_file {marks['unreadied']} 30\nwait_label LabelMultiplayerStatus to press Ready\n"
                "activate ButtonMultiplayerStart\nwait_label LabelMultiplayerStatus Starting in\nwait_state Running 50\nexit\n")
        client = (join_by_address(port) + "activate ButtonMultiplayerReady\nwait 4\nassert_label ButtonMultiplayerReady Cancel Ready\n"
                  f"wait_file {marks['host_saw_ready']} 30\nactivate ButtonMultiplayerReady\nwait 4\nassert_label ButtonMultiplayerReady Ready\n"
                  f"touch_file {marks['unreadied']}\nwait_label LabelMultiplayerStatus The host is starting the match in\nwait_state Running 50\nexit\n")
    else:  # lobby-seat-missing
        # A human seat nobody has joined keeps Start off, and the lobby says who is missing.
        # A lobby with an empty seat is never all ready: the host learns of the joined player's Ready by its mark.
        host = (host_lobby(port, players=3) + f"wait_connected 2 60\nwait_file {marks['readied']} 60\nwait 10\nassert_enabled ButtonMultiplayerStart 0\n"
                f"wait_label LabelMultiplayerStatus Waiting for 1 more player to join\ntouch_file {marks['done']}\nexit\n")
        client = join_by_address(port) + f"activate ButtonMultiplayerReady\nwait 4\ntouch_file {marks['readied']}\nwait_file {marks['done']} 60\nexit\n"
    scripts = {"host": host, "client": client}
    probes = {}
    if case != "lobby-seat-missing":
        # A menu script runs only while the menus do: once the round runs, the host ends it by hand (Escape, End Match) and both
        # scripts finish in the lobby they come back to.
        for who in scripts:
            steps = [{"op": "wait", "service": "Running"}, {"op": "wait", "elapsed_ms": 2000}]
            if who == "host":
                steps += [{"op": "key_down", "key": "Escape"}, {"op": "key_up", "key": "Escape"},
                          {"op": "wait", "screen": "Pause", "elapsed_ms": 400},
                          menu_step("assert_enabled ButtonEndMatch 1"), menu_step("activate ButtonEndMatch")]
            steps += [{"op": "wait", "service": "Starting", "scope": "menu"}, {"op": "signal", "name": "done", "scope": "menu"}, {"op": "finish"}]
            probes[who] = {"schema": 1, "timeout_ms": 150000, "steps": steps}
            assert scripts[who].endswith("exit\n"), scripts[who][-80:]
            # The host leaves last: its exit closes the lobby the joined player is back in.
            last = f"wait_file {probe_root(root, 'client') / 'done.json'} 60\n" if who == "host" else ""
            scripts[who] = scripts[who][:-len("exit\n")] + f"wait_file {probe_root(root, who) / 'done.json'} 60\n{last}assert_substate Lobby\nexit\n"
    return ({who: in_base_words(text) for who, text in scripts.items()} if BASE_WORDS else scripts), probes



def size_parts(size):
    """A size's screen width and height and the window multiplier it is shown at: "960x540@2.6667" is a 960x540 screen in a 2560x1440 window."""
    screen, _, multiplier = size.partition("@")
    width, height = (int(part) for part in screen.split("x"))
    return width, height, float(multiplier) if multiplier else 1.0


def set_window_multiplier(run, multiplier):
    """Shows the run's screen at the multiplier, the way a player's Settings.ini does."""
    if multiplier == 1.0:
        return
    path = run.cwd / "Userdata/Settings.ini"
    settings = path.read_text(encoding="utf-8")
    settings, count = re.subn(r"(?m)^(\s*ResolutionMultiplier\s*=\s*)[^\r\n]*", lambda match: match[1] + f"{multiplier:.6f}", settings)
    if count == 0:
        settings = settings.replace("\tResolutionY", f"\tResolutionMultiplier = {multiplier:.6f}\n\tResolutionY", 1)
    path.write_text(settings, encoding="utf-8")


ESCAPE = "key Escape down\nwait 2\nkey Escape up\nwait 8\n"
# Ways back for the buttons a sweep presses: one level back by Escape, to the screen the sweep stands on.
SWEEP_MACROS = ("define back_landing\n" + ESCAPE + "assert_substate Landing\nend\n"
                "define back_host\n" + ESCAPE + "assert_substate HostSetup\nend\n"
                "define back_lobby\n" + ESCAPE + "assert_substate Lobby\nend\n"
                "define back_join\n" + ESCAPE + "assert_substate JoinSetup\nend\n"
                "define back_options\n" + ESCAPE + "assert_substate HostOptions\nend\n"
                "define back_basics\nactivate TabNetPageBasics\nwait 4\nassert_settings_page Network:Basics\nend\n")
OPTION_PAGES = (("TabHostPageSeats", "CollectionBoxHostPageSeats"), ("TabHostPageRules", "CollectionBoxHostPageRules"),
                ("TabHostPageConnection", "CollectionBoxHostPageConnection"), ("TabHostPageTiming", "CollectionBoxHostPageTiming"),
                ("TabHostPageRecovery", "CollectionBoxHostPageRecovery"), ("TabHostPageFiles", "CollectionBoxHostPageFiles"),
                ("TabHostPageSession", "CollectionBoxHostPageSession"))
FOOTER = "ButtonHostOptBack,ButtonHostOptApply,ButtonHostOptDefaults,ButtonHostOptRestore"


def sweep_options(readonly=False, live=False):
    """Advanced, page by page: the footer and the selector, then every page's own controls; a dialog a page opens is closed by Escape."""
    own = FOOTER + (",ButtonHostSessEnd" if live and not readonly else "")
    text = f"sweep MultiplayerHostOptionsPanel depth=1 label=advanced-frame own={own}\n" if not readonly else "sweep MultiplayerHostOptionsPanel depth=1 label=advanced-frame readonly own=ButtonHostOptBack\n"
    # Each page and dialog is pictured as it opens, before the sweep changes anything on it.
    for tab, page in OPTION_PAGES:
        # The team picker chooses whose technology and AI skill the rows under it show; picking one changes no setting.
        quiet = " quiet=ComboHostRulesTeam" if page.endswith("Rules") and not readonly else ""
        # A page whose buttons act in place (Recalculate, Repair match now, Save diagnostics) is come back to by its own tab;
        # Escape is the way back from the dialogs the Seats and Session pages open, and would leave Advanced from the others.
        in_place = page.endswith(("Timing", "Recovery", "Files"))
        back = f"back_{tab}" if in_place else "back_options"
        if in_place:
            text += f"define {back}\nactivate {tab}\nwait 4\nassert_substate HostOptions\nassert_checked {tab} 1\nend\n"
        # A reader may open each seat's details: both are pressed below, the first for the seat dialog's sweep.
        seat_details = " own=ButtonHostSeatDetails0,ButtonHostSeatDetails1" if readonly and page.endswith("Seats") else ""
        # Saving diagnostics is this computer's own act, open to a reader too: pressed once below.
        seat_details += " own=ButtonHostFilesSaveDiag" if readonly and page.endswith("Files") else ""
        # So is this computer's own ban list on the Session page.
        seat_details += " own=ButtonHostSessBanned" if readonly and page.endswith("Session") else ""
        text += f"activate {tab}\nwait 4\ndump_host_options\nsweep {page} label={page} restore={back}" + (" readonly" if readonly else "") + (" own=ButtonHostSessEnd" if live and not readonly and page.endswith("Session") else "") + seat_details + quiet + "\n"
    if readonly:
        text += "activate TabHostPageFiles\nwait 4\nactivate ButtonHostFilesSaveDiag\nwait 6\nassert_substate HostOptions\n"
        text += "activate TabHostPageSession\nwait 4\nactivate ButtonHostSessBanned\nwait 6\nactivate ButtonHostBannedClose\nwait 4\nassert_substate HostOptions\n"
        text += "activate TabHostPageSeats\nwait 4\nactivate ButtonHostSeatDetails0\nwait 6\ndump_host_options\nsweep HostSeatDialog label=seat-dialog readonly own=ButtonHostSeatDlgClose\nactivate ButtonHostSeatDlgClose\nwait 4\n"
        text += "activate ButtonHostSeatDetails1\nwait 6\nassert_visible HostSeatDialog 1\nactivate ButtonHostSeatDlgClose\nwait 4\nassert_visible HostSeatDialog 0\n"
    if not readonly:
        # The seat dialog a Details button opens is a screen of its own.
        text += "activate TabHostPageSeats\nwait 4\nactivate ButtonHostSeatDetails0\nwait 6\ndump_host_options\nsweep HostSeatDialog label=seat-dialog own=ButtonHostSeatDlgClose\nactivate ButtonHostSeatDlgClose\nwait 4\n"
        text += "activate ButtonHostOptRestore\nwait 4\nactivate ButtonHostOptDefaults\nwait 4\nactivate ButtonHostOptApply\nwait 6\n"
    return text


def sweep_case(case, port, root):
    """The sweep of one screen; the screen's own control list says what is visited."""
    host_screen = LANDING + "activate ButtonMultiplayerHostGame\nwait_ms 400\nassert_substate HostSetup\n" + f"setup_host_port {port}\n"
    if case == "sweep-landing":
        text = LANDING + SWEEP_MACROS + "dump_host_options\nsweep MultiplayerLandingPanel label=landing restore=back_landing\nexit\n"
    elif case == "sweep-host":
        text = (host_screen + SWEEP_MACROS + "dump_host_options\nsweep MultiplayerHostPanel label=host restore=back_host own=ButtonHostBack,ButtonMultiplayerCreate\n"
                "activate ButtonMultiplayerCreate\nwait 15\nassert_substate Lobby\nactivate ButtonMultiplayerLeave\nwait 8\nassert_substate Landing\n"
                "activate ButtonMultiplayerHostGame\nwait 6\nactivate ButtonHostBack\nwait 6\nassert_substate Landing\nexit\n")
    elif case == "sweep-join":
        text = (LANDING + "activate ButtonMultiplayerJoinGame\nwait_ms 400\nassert_substate JoinSetup\n" + SWEEP_MACROS +
                "dump_host_options\nsweep MultiplayerJoinPanel label=join restore=back_join own=ButtonJoinBack\n"
                "activate ButtonJoinByAddress\nwait 4\ndump_host_options\nsweep JoinAddressDialog label=join-address own=ButtonJoinAddressCancel,ButtonJoinAddressGo\n"
                "activate ButtonJoinAddressCancel\nwait 4\nactivate ButtonJoinByAddress\nwait 4\nactivate ButtonJoinAddressGo\nwait 6\n"
                "wait_label LabelJoinSelected Enter the address the host gave you\nassert_substate JoinSetup\n"
                "activate ButtonJoinBack\nwait 6\nassert_substate Landing\nexit\n")
    elif case == "sweep-lobby":
        text = (host_screen + SWEEP_MACROS + "activate ButtonMultiplayerCreate\nwait 15\nassert_substate Lobby\n"
                "dump_host_options\nsweep MultiplayerLobbyPanel label=lobby restore=back_lobby quiet=TextLobbyChat own=ButtonMultiplayerLeave\n"
                "activate ButtonMultiplayerLeave\nwait 8\nassert_substate Landing\nexit\n")
    elif case == "sweep-advanced-setup":
        text = (host_screen + SWEEP_MACROS + "activate ButtonHostOptions\nwait_ms 400\nassert_substate HostOptions\n" + sweep_options() +
                "activate ButtonHostOptBack\nwait 6\nassert_substate HostSetup\nexit\n")
    elif case == "sweep-advanced-lobby":
        text = (host_screen + SWEEP_MACROS + "activate ButtonMultiplayerCreate\nwait 15\nassert_substate Lobby\n"
                "activate ButtonLobbyOptions\nwait_ms 400\nassert_substate HostOptions\n" + sweep_options(live=True) +
                # Back returns to the lobby; Advanced again, and the session's own end button.
                "activate ButtonHostOptBack\nwait 6\nassert_substate Lobby\nactivate ButtonLobbyOptions\nwait_ms 400\nassert_substate HostOptions\n"
                "activate TabHostPageSession\nwait 4\nactivate ButtonHostSessEnd\nwait 8\nassert_substate Landing\nexit\n")
    elif case == "sweep-advanced-client":
        done = probe_root(root, "host") / "client-swept.mark"
        host = (host_screen + "activate ButtonMultiplayerCreate\nwait 15\nassert_substate Lobby\nwait_connected 2 60\n" + f"wait_file {done} 300\nexit\n")
        client = (LANDING + "activate ButtonMultiplayerJoinGame\nwait_ms 400\nactivate ButtonJoinByAddress\nwait 4\n"
                  f"settext TextJoinAddress 127.0.0.1\nsettext TextJoinPort {port}\nactivate ButtonJoinAddressGo\nwait_connected 2 60\nwait_substate Lobby 30\nwait 5\n" + SWEEP_MACROS +
                  "dump_host_options\nactivate ButtonLobbyEditSetup\nwait_ms 400\nassert_substate HostOptions\n" + sweep_options(readonly=True) +
                  f"activate ButtonHostOptBack\nwait 6\nassert_substate Lobby\ntouch_file {done}\nexit\n")
        return {"host": host, "client": client}, {}
    elif case == "sweep-settings-network":
        pages = ("Player", "Chat", "Recovery", "Files", "Internet", "Connection")
        # A button's way back is to the page its sweep stands on, so the page's next button is still under the hand.
        backs = "".join(f"define back_net_{page.lower()}\nactivate TabNetPage{page}\nwait 4\nassert_settings_page Network:{page}\nend\n" for page in pages)
        text = (OPTIONS + SWEEP_MACROS + backs + "select_settings_page Network\nwait 4\nassert_settings_page Network:Basics\n"
                "dump_player_options\nsweep CollectionBoxNetPageBasics label=network-basics restore=back_basics\n")
        # The boxes that take a key name or a digest are given one they take.
        takes = {"Chat": " with=TextNetworkChatKey:Y", "Internet": " with=TextNetworkDirPin:" + "0123456789abcdef" * 4}
        for page in pages:
            text += (f"activate TabNetPage{page}\nwait 4\ndump_player_options\nsweep CollectionBoxNetPage{page} label=network-{page.lower()} restore=back_net_{page.lower()}"
                     f"{takes.get(page, '')}\nactivate TabNetPage{page}\nwait 4\n")
        text += "activate TabNetPageBasics\nwait 4\nexit\n"
    else:  # sweep-browsers
        text = (LANDING + SWEEP_MACROS + "activate ButtonMultiplayerReplays\nwait 6\ndump_host_options\nsweep ReplayBrowserPanel label=replays own=ButtonReplayBack\n"
                "activate ButtonReplayBack\nwait 6\nassert_substate Landing\nactivate ButtonMultiplayerResumeGame\nwait 6\n"
                "dump_host_options\nsweep MultiplayerResumePanel label=saved-matches own=ButtonResumeBack\nactivate ButtonResumeBack\nwait 6\nassert_substate Landing\nexit\n")
    return {"host": text}, {}


# Every field of the seven host pages, each set by hand to something other than its default, and the words that read it back.
# A match field rides the lobby's config to every peer; a computer field is this machine's own. Order matters only where one
# field opens another: the mode and the activity before the seats and the rules they seed, the count before the seat it
# opens, Fixed before a seat's delay,
# autosave on before its interval.
def draft_fields(port):
    match = (
        ("mode", "Rules", hand_pick("ComboHostRulesMode", "Players and AI opponents"), "assert_label ComboHostRulesMode Players and AI opponents\n"),
        ("activity", "Rules", "combo_select ComboHostRulesActivity Wave Defense - Base.rte\nwait 4\n", "assert_label ComboHostRulesActivity Wave Defense\n"),
        ("scene", "Rules", hand_pick("ComboHostRulesScene", "Croco Cave"), "assert_label ComboHostRulesScene Croco Cave\n"),
        ("difficulty", "Rules", "slider_set SliderHostRulesDifficulty 80\nwait 4\n", "assert_label LabelHostRulesDifficultyValue 80\n"),
        ("starting_gold", "Rules", "slider_set SliderHostRulesGold 5000\nwait 4\n", "assert_label LabelHostRulesGoldValue 5000 oz\n"),
        ("fog_of_war", "Rules", "setcheck CheckHostRulesFog 0\nwait 4\n", "assert_checked CheckHostRulesFog 0\n"),
        ("clear_path", "Rules", "setcheck CheckHostRulesClearPath 0\nwait 4\n", "assert_checked CheckHostRulesClearPath 0\n"),
        ("deploy_units", "Rules", "setcheck CheckHostRulesDeploy 0\nwait 4\n", "assert_checked CheckHostRulesDeploy 0\n"),
        ("brain_loss", "Rules", hand_pick("ComboHostRulesBrainless", "End the match"), "assert_label ComboHostRulesBrainless End the match\n"),
        ("team_technology", "Rules", hand_pick("ComboHostRulesTeam", "Team 2") + hand_pick("ComboHostRulesTech", "Coalition.rte"),
         hand_pick("ComboHostRulesTeam", "Team 2") + "assert_label ComboHostRulesTech Coalition.rte\n"),
        ("team_ai_skill", "Rules", hand_pick("ComboHostRulesTeam", "Team 1") + "slider_set SliderHostRulesSkill 30\nwait 4\n",
         hand_pick("ComboHostRulesTeam", "Team 1") + "assert_label LabelHostRulesSkillValue 30\n"),
        ("players", "Seats", hand_pick("ComboHostSeatPlayers", "3"), "assert_label ComboHostSeatPlayers 3\n"),
        ("seat_teams", "Seats", hand_pick("ComboHostSeatTeam0", "Team 2") + hand_pick("ComboHostSeatTeam1", "Team 1"),
         "assert_label ComboHostSeatTeam0 Team 2\nassert_label ComboHostSeatTeam1 Team 1\n"),
        ("open_seat", "Seats", hand_pick("ComboHostSeatType2", "Open"), "assert_label ComboHostSeatType2 Open\n"),
        ("delay_policy", "Timing", hand_pick("ComboHostNetPolicy", "Fixed"), "assert_label ComboHostNetPolicy Fixed\n"),
        ("slow_bound", "Timing", "settext TextHostNetSlowBound 7\nwait 4\n", "assert_label TextHostNetSlowBound 7\n"),
        ("redundancy", "Timing", hand_pick("ComboHostNetRedundancy", "7 ticks"), "assert_label ComboHostNetRedundancy 7 ticks\n"),
        ("input_delay", "Timing", "settext TextHostNetMinDelay 5\nwait 4\n", "assert_label TextHostNetMinDelay 5\n"),
        ("seat_delay", "Timing", "settext TextHostNetPeerDelay2 6\nwait 4\n", "assert_label TextHostNetPeerDelay2 6\n"),
        ("repair", "Recovery", "setcheck CheckHostRecRepair 0\nwait 4\n", "assert_checked CheckHostRecRepair 0\n"),
        ("autosave", "Recovery", "setcheck CheckHostRecAutosave 1\nwait 4\n", "assert_checked CheckHostRecAutosave 1\n"),
        ("autosave_interval", "Recovery", "settext TextHostRecAutosaveInterval 120\nwait 4\n", "assert_label TextHostRecAutosaveInterval 120\n"),
        ("return_window", "Recovery", hand_pick("ComboHostRecReturnWindow", "10 minutes"), "assert_label ComboHostRecReturnWindow 10 minutes\n"),
        ("idle_wait", "Session", hand_pick("ComboHostSessIdle", "20 minutes"), "assert_label ComboHostSessIdle 20 minutes\n"),
    )
    computer = (
        ("listing", "Connection", hand_pick("ComboHostNetVisibility", "Unlisted"), "assert_label ComboHostNetVisibility Unlisted\n"),
        ("nat_traversal", "Connection", hand_pick("ComboHostNetIce", "Off"), "assert_label ComboHostNetIce Off\n"),
        ("relay", "Connection", hand_pick("ComboHostNetRelay", "Off"), "assert_label ComboHostNetRelay Off\n"),
        ("port", "Connection", f"settext TextHostNetPort {port}\nwait 4\n", f"assert_label TextHostNetPort {port}\n"),
        ("status_widget", "Files", hand_pick("ComboHostFilesWidget", "Always"), "assert_label ComboHostFilesWidget Always\n"),
    )
    return match, computer


def page(name):
    return f"activate TabHostPage{name}\nwait 4\n"


def read_fields(fields):
    """Every field's value read back, page by page, as the controls show it."""
    return "".join(page(tab) + check for _, tab, _, check in fields)


def third_pass_case(case, port, root, size):
    marks = {name: probe_root(root, "host") / f"{name}.mark" for name in
             ("readied", "applied", "counting", "opened", "read", "back", "done", "client_done", "clientb_done", "hosted")}
    advanced = "activate ButtonHostOptions\nwait 10\nassert_substate HostOptions\n"
    setup = LANDING + "activate ButtonMultiplayerHostGame\nwait_ms 400\nassert_substate HostSetup\n" + f"setup_host_port {port}\n"
    match, computer = draft_fields(port)
    if case == "host-draft-roundtrip":
        # Set by hand, Apply, Back, reopened: the controls show every value; Create Lobby carries every match value into the lobby
        # (the host's own live view reads the adopted config) and the joined player's read-only view shows each one.
        host = setup + advanced + "".join(page(tab) + change for _, tab, change, _ in match + computer)
        # What Apply's own button reads is host-draft-apply's to check; here the values themselves are followed.
        host += "activate ButtonHostOptApply\nwait 6\n"
        host += "activate ButtonHostOptBack\nwait 6\nassert_substate HostSetup\n" + advanced + read_fields(match + computer)
        host += "activate ButtonHostOptBack\nwait 6\nassert_substate HostSetup\nactivate ButtonMultiplayerCreate\nwait 15\nassert_substate Lobby\n"
        host += "activate ButtonLobbyEditSetup\nwait 10\nassert_substate HostOptions\n" + read_fields(match)
        host += f"activate ButtonHostOptBack\nwait 6\nassert_substate Lobby\nwait_connected 2 60\nwait_file {marks['client_done']} 90\nexit\n"
        client = join_by_address(port) + "assert_label ButtonLobbyEditSetup Match details\nactivate ButtonLobbyEditSetup\nwait 10\nassert_substate HostOptions\n"
        # The joined player's details mirror the lobby's config from the moment it arrives.
        client += page("Rules") + "wait_label ComboHostRulesMode Players and AI opponents\n" + read_fields(match) + f"activate ButtonHostOptBack\nwait 6\nassert_substate Lobby\ntouch_file {marks['client_done']}\nexit\n"
        return {"host": host, "client": client}, {}
    if case == "host-draft-apply":
        # After an earlier Apply, each field changed alone enables Apply and is applied.
        host = setup + advanced + page("Timing") + hand_pick("ComboHostNetRedundancy", "6 ticks") + "activate ButtonHostOptApply\nwait 6\n"
        # The setup already listens on the run's port: another one is the change.
        for _, tab, change, check in match + draft_fields(port + 1)[1]:
            host += (page(tab) + change + "assert_enabled ButtonHostOptApply 1\nactivate ButtonHostOptApply\nwait 6\n"
                     "assert_enabled ButtonHostOptApply 0\n" + check)
        return {"host": host + "exit\n"}, {}
    if case == "host-apply-all":
        # Apply takes all of a draft or none of it. A refused port keeps the rule changed beside it unapplied; a team's random
        # technology and a listing change apply together (a match half the base refused after it had taken the listing).
        host = (setup + advanced + page("Rules") + "setcheck CheckHostRulesFog 0\nwait 4\n"
                + page("Connection") + "settext TextHostNetPort 80\nwait 4\n"
                + "activate ButtonHostOptApply\nwait 6\ndump_host_options\nassert_label LabelHostOptStatus The game port must be from 1024 to 65535\n"
                "assert_enabled ButtonHostOptApply 1\nactivate ButtonHostOptBack\nwait 6\nassert_substate HostSetup\n" + advanced
                + page("Rules") + "assert_checked CheckHostRulesFog 1\n" + page("Connection") + f"assert_label TextHostNetPort {port}\n"
                + page("Rules") + hand_pick("ComboHostRulesTech", "-Random-") + page("Connection") + hand_pick("ComboHostNetVisibility", "Unlisted")
                + "activate ButtonHostOptApply\nwait 6\ndump_host_options\nactivate ButtonHostOptBack\nwait 6\nassert_substate HostSetup\n" + advanced
                + page("Connection") + "assert_label ComboHostNetVisibility Unlisted\n" + page("Rules") + "assert_label ComboHostRulesTech -Random-\nexit\n")
        return {"host": host}, {}
    if case == "lobby-setup-unready":
        # The normal lobby (its hand-over order on): a setup change takes back the joined player's Ready on both screens,
        # the player reads why, and Start then counts down instead of starting.
        host = (host_lobby(port) + "wait_remote_ready 60\nwait_label LabelMultiplayerStatus Everyone is ready\n"
                "activate ButtonLobbyEditSetup\nwait 10\nassert_substate HostOptions\n" + page("Rules")
                + "setcheck CheckHostRulesFog 0\nwait 4\nactivate ButtonHostOptApply\nwait 6\nactivate ButtonHostOptBack\nwait 6\nassert_substate Lobby\n"
                f"touch_file {marks['applied']}\nwait_label LabelMultiplayerStatus to press Ready\nwait_file {marks['read']} 60\nwait 30\n"
                "wait_label LabelMultiplayerStatus to press Ready\nactivate ButtonMultiplayerStart\nwait_label LabelMultiplayerStatus Starting in\n"
                f"activate ButtonMultiplayerStart\nwait 6\ntouch_file {marks['done']}\nexit\n")
        client = (join_by_address(port) + "activate ButtonMultiplayerReady\nwait 4\nassert_label ButtonMultiplayerReady Cancel Ready\n"
                  f"wait_file {marks['applied']} 60\nwait_label LabelMultiplayerStatus check it and press Ready again\n"
                  f"assert_label ButtonMultiplayerReady Ready\ntouch_file {marks['read']}\nwait_file {marks['done']} 60\nexit\n")
        return {"host": host, "client": client}, {}
    if case == "lobby-setup-open":
        # Opening the setup during the count stops it: the host's panel says so, the joined player reads why, nothing starts.
        host = (host_lobby(port) + "wait_connected 2 60\nwait_label LabelMultiplayerStatus Start Match starts in 30 s\n"
                f"activate ButtonMultiplayerStart\nwait_label LabelMultiplayerStatus Starting in\nwait_file {marks['counting']} 60\n"
                "activate ButtonLobbyEditSetup\nwait 10\nassert_substate HostOptions\n"
                f"wait_label LabelHostOptStatus The start countdown stopped while you change the setup\ntouch_file {marks['opened']}\n"
                f"wait_file {marks['read']} 60\nactivate ButtonHostOptBack\nwait 6\nassert_substate Lobby\n"
                "wait_label LabelMultiplayerStatus Start Match starts in 30 s\nassert_label ButtonMultiplayerStart Start Match\n"
                f"touch_file {marks['back']}\nwait_file {marks['client_done']} 60\nexit\n")
        client = (join_by_address(port) + f"wait_label LabelMultiplayerStatus The host is starting the match in\ntouch_file {marks['counting']}\n"
                  f"wait_file {marks['opened']} 60\nwait_label LabelMultiplayerStatus The host is changing the setup\ntouch_file {marks['read']}\n"
                  f"wait_file {marks['back']} 60\nwait_label LabelMultiplayerStatus Press Ready when you're ready to play\n"
                  f"touch_file {marks['client_done']}\nexit\n")
        return {"host": host, "client": client}, {}
    if case == "lobby-long-names":
        # Two joiners with 24 wide characters each: the names shorten, still told apart, and the sentence keeps its end.
        names = {"client": "W" * 23 + "A", "clientb": "W" * 23 + "B"}
        # Both sentences that name the joiners: the one before the count (the longer) and the one during it.
        host = (host_lobby(port, players=3) + "wait_connected 3 90\nwait_label LabelMultiplayerStatus Start Match starts in 30 s\n"
                "assert_text_fits LabelMultiplayerStatus\nassert_label LabelMultiplayerStatus WWA\nassert_label LabelMultiplayerStatus WWB\nassert_label LabelMultiplayerStatus to press Ready\n"
                "assert_enabled ButtonMultiplayerStart 1\nactivate ButtonMultiplayerStart\nwait_label LabelMultiplayerStatus Starting in\n"
                "wait 4\nassert_label LabelMultiplayerStatus to press Ready\nassert_text_fits LabelMultiplayerStatus\n"
                "assert_label LabelMultiplayerStatus WWA\nassert_label LabelMultiplayerStatus WWB\nassert_label LabelMultiplayerStatus to press Ready\ndump_host_options\n"
                f"activate ButtonMultiplayerStart\nwait 6\ntouch_file {marks['done']}\nexit\n")
        scripts_ = {"host": host}
        for who, name in names.items():
            scripts_[who] = (LANDING + f"settext TextMultiplayerName {name}\nwait 3\n" + join_by_address(port)[len(LANDING):]
                             + f"wait_file {marks['done']} 120\nexit\n")
        return scripts_, {}
    if case == "host-words":
        # Words a player reads: Recalculate offers nothing before a lobby exists; the Internet page points at a page that
        # exists; the Timing page names seats, not peers; the authentication line says what this build does.
        host = (setup + advanced + page("Timing") + "assert_enabled ButtonHostNetRecalc 0\n" + hand_pick("ComboHostNetPolicy", "Fixed")
                + "assert_label_absent LabelHostNetPeer1 Peer\nassert_label LabelHostNetPeer1 Seat 1\n"
                + page("Recovery") + "assert_label LabelHostRecRejoin Authenticated rejoin: On\n"
                + "activate ButtonHostOptBack\nwait 6\nactivate ButtonHostBack\nwait 6\nassert_substate Landing\n"
                "goto_main\nwait 6\nactivate ButtonMainToOptions\nwait 8\nassert_screen SettingsScreen\n" + net_page("Internet")
                + "assert_label_absent LabelNetInternetReason Host Options > Network\n"
                "assert_label LabelNetInternetReason Host a Game > Advanced > Connection\nexit\n")
        return {"host": host}, {}
    if case == "host-words-nocrypto":
        # A build without its cryptography says so where the host reads the rejoin line.
        host = setup + advanced + page("Recovery") + "assert_label LabelHostRecRejoin Authenticated rejoin: Off\nexit\n"
        return {"host": host}, {}
    if case == "host-long-names":
        # A mod's 60-character activity and scene names fit the Advanced pages at every size.
        host = (setup + advanced + page("Rules") + f"combo_select ComboHostRulesActivity {LONG_ACTIVITY} - {LONG_MODULE}\nwait 6\n"
                f"combo_select ComboHostRulesScene {LONG_SCENE}\nwait 6\nassert_text_fits ComboHostRulesActivity\nassert_text_fits ComboHostRulesScene\n"
                f"assert_label ComboHostRulesActivity The Extraordinarily\nassert_label ComboHostRulesActivity - {LONG_MODULE}\nassert_label ComboHostRulesScene Northern Mining\n"
                "dump_host_options\n" + page("Seats") + "dump_host_options\nactivate ButtonHostOptBack\nwait 6\nassert_substate HostSetup\n"
                "assert_text_fits LabelHostInfo\ndump_host_options\nexit\n")
        return {"host": host}, {}
    if case == "port-by-hand":
        # Run through the failing form in main: the word must fail at the port check.
        return {"host": setup + "exit\n"}, {}
    if case in ("old-skins-public", "old-skins-original"):
        # A replacement of Base's menu files from an older version: no crash; the main menu, every Settings page a player
        # uses and a single-player start work; the multiplayer part says in one sentence why it is off.
        probe = probe_root(root, "host")
        text = ("wait 40\nassert_screen MainScreen\nactivate ButtonMainToOptions\nwait 8\nassert_screen SettingsScreen\n"
                "assert_label LabelSettingsMultiplayerOff Multiplayer settings are off\nassert_text_fits LabelSettingsMultiplayerOff\n"
                + "".join(f"select_settings_page {name}\nwait 4\nassert_settings_page {name}\n" for name in ("Video", "Audio", "Input", "Gameplay", "Misc"))
                + "activate ButtonBackToMainMenu\nwait 8\nassert_screen MainScreen\nactivate ButtonMainToMultiplayer\nwait 8\nassert_screen MainScreen\n"
                "assert_label LabelMultiplayerOff Multiplayer is off\nassert_text_fits LabelMultiplayerOff\n"
                "activate ButtonMainToSkirmish\nwait_ms 1600\nassert_screen ScenarioPicker\ncombo_select ComboBoxActivitySelect P4 Alpha Duel\n"
                "wait_ms 700\nselect_scene Grasslands\nwait_ms 900\nactivate ButtonStartActivityConfig\nwait_ms 700\nassert_screen ScenarioConfig\n"
                "activate P1T1Box\nactivate P2T2Box\nwait_ms 1000\nactivate ButtonStartGame\n"
                f"wait_file {probe / 'done.json'} 120\nwait 10\nassert_screen ScenarioPicker\n"
                "activate BackToMainButton\nwait_ms 700\nassert_screen MainScreen\nexit\n")
        # The pause menu returns to the scenario picker, whose Back button returns to the main menu.
        return {"host": text}, {"host": {"schema": 1, "timeout_ms": 150000, "steps": [
            {"op": "wait_file", "path": str(probe / "started.mark")}, {"op": "wait", "renders": 180},
            {"op": "wait", "screen": "Gameplay", "sim_at_least": 120},
            {"op": "key_down", "key": "Escape"}, {"op": "key_up", "key": "Escape"}, {"op": "wait", "screen": "Pause"},
            menu_step("activate ButtonBackToMain"), {"op": "wait", "renders": 30}, {"op": "signal", "name": "done"}, {"op": "finish"}]}}
    raise ValueError(case)


# A mod's long names, at the length mods give them.
LONG_MODULE = "LongNames.rte"
LONG_ACTIVITY = "The Extraordinarily Long Siege of the Northern Mining Colony"
LONG_SCENE = "Northern Mining Colony Outskirts Beneath the Glacier Ridges"
OLD_SKINS = {"old-skins-public": "d95f5dee05", "old-skins-original": "upstream/development"}
OLD_SKIN_FILES = ("SettingsGUI.ini", "MainMenuSubMenuGUI.ini")


def stage_old_skin_runtime(repo, run_root, revision):
    """A runtime whose Base.rte/GUIs holds an older version's two menu files, every other file the tree's own: the tree's
    Data is reached through junctions and never written."""
    import _winapi
    data = Path(repo) / "Data"
    runtime = Path(run_root) / "runtime"
    for name in ("Data", "Mods", "ScreenShots", "Userdata", "Temp"):
        (runtime / name).mkdir(parents=True)
    from run_sim_test import runtime_settings_text
    source = Path(repo) / "Userdata/Settings.ini"
    (runtime / "Userdata/Settings.ini").write_text(runtime_settings_text(source.read_text(encoding="utf-8-sig") if source.is_file() else "SettingsMan\n"), encoding="utf-8")
    for module in data.iterdir():
        if module.name != "Base.rte":
            _winapi.CreateJunction(str(module), str(runtime / "Data" / module.name)) if module.is_dir() else None
    for entry in (data / "Base.rte").iterdir():
        target = runtime / "Data/Base.rte" / entry.name
        target.parent.mkdir(parents=True, exist_ok=True)
        if entry.name == "GUIs":
            target.mkdir()
            for item in entry.iterdir():
                if item.is_dir():
                    _winapi.CreateJunction(str(item), str(target / item.name))
                elif item.name in OLD_SKIN_FILES:
                    old = subprocess.check_output(["git", "-C", str(repo), "show", f"{revision}:Data/Base.rte/GUIs/{item.name}"])
                    (target / item.name).write_bytes(old)
                else:
                    (target / item.name).write_bytes(item.read_bytes())
        elif entry.is_dir():
            _winapi.CreateJunction(str(entry), str(target))
        else:
            target.write_bytes(entry.read_bytes())
    return runtime


def scripts(case, port, root, size="960x540"):
    if case in THIRD_PASS_CASES:
        return third_pass_case(case, port, root, size)
    if case == "sweep-fault":
        return {"host": LANDING + "exit\n"}, {}
    if case.startswith("sweep-"):
        return sweep_case(case, port, root)
    if case in ("lobby-ready-all", "lobby-countdown", "lobby-last-ready", "lobby-ready-back", "lobby-seat-missing"):
        return lobby_case(case, port, root)
    if case == "lobby-escape":
        # Escape in an open lobby asks before it closes it; Stay keeps the lobby and its players.
        text = (host_lobby(port) + "key Escape down\nwait 2\nkey Escape up\nwait 6\nassert_substate Lobby\nwait_state Starting 5\n"
                "assert_visible LobbyLeaveDialog 1\nactivate ButtonLobbyLeaveStay\nwait 4\nassert_substate Lobby\nwait_state Starting 5\nexit\n")
        return {"host": in_base_words(text) if BASE_WORDS else text}, {}
    if case == "host-hand-open":
        # Opened by a hand - its press, then its release frames later - Advanced shows the activity's own rules and the draft's
        # own timing (the default redundancy), never what its controls held before the panel showed the draft.
        text = (LANDING + "activate ButtonMultiplayerHostGame\nwait_ms 400\nassert_substate HostSetup\n"
                "activate ButtonHostOptions\nwait 10\nassert_substate HostOptions\n"
                "activate TabHostPageRules\nwait 3\nassert_checked CheckHostRulesFog 1\nassert_label LabelHostRulesGoldValue 2000 oz\n"
                "activate TabHostPageTiming\nwait 3\nassert_label ComboHostNetRedundancy 4 ticks\nexit\n")
        return {"host": in_base_words(text) if BASE_WORDS else text}, {}
    if case == "host-one-draft":
        # The rows, Advanced and the summary are one draft: rows changed after a visit to Advanced reach the lobby.
        text = (LANDING + "activate ButtonMultiplayerHostGame\nwait_ms 400\n" + f"setup_host_port {port}\n"
                "activate ButtonHostOptions\nwait_ms 400\nassert_substate HostOptions\nactivate TabHostPageTiming\nwait 3\n"
                "combo_select ComboHostNetRedundancy 7 ticks\nwait 4\nactivate ButtonHostOptApply\nwait 4\n"
                "activate ButtonHostOptBack\nwait_ms 400\nassert_substate HostSetup\n"
                "combo_select ComboHostPlayers 3\nwait 4\ncombo_select ComboHostMode Players versus AI\nwait 4\n"
                "activate ButtonMultiplayerCreate\nwait 15\nassert_substate Lobby\n"
                "wait_label LabelLobbyMatchMode Players versus AI\nwait_label LabelMultiplayerStatus Waiting for 2 more players to join\nexit\n")
        return {"host": in_base_words(text) if BASE_WORDS else text}, {}
    if case == "local-end-match":
        host = (LANDING + "activate ButtonMultiplayerHostGame\nwait_ms 400\n"
                "combo_select ComboHostActivity P4 Alpha Duel\nwait_ms 400\n"
                f"setup_host_port {port}\ncombo_select ComboHostPlayers 2\n"
                "activate ButtonMultiplayerCreate\nwait_connected 2 60\nwait_remote_ready 60\n"
                # The Start button takes the remote ready on the menu's next update.
                "wait 3\nactivate ButtonMultiplayerStart\n")
        client = (LANDING + "activate ButtonMultiplayerJoinGame\nwait_ms 400\n"
                  f"activate ButtonJoinByAddress\nwait 4\nsettext TextJoinAddress 127.0.0.1\nsettext TextJoinPort {port}\n"
                  "activate ButtonJoinAddressGo\nwait_connected 2 60\nwait_substate Lobby 30\nactivate ButtonMultiplayerReady\n")
        probes = {}
        for who in ("host", "client"):
            steps = [{"op": "wait", "service": "Running"}, {"op": "wait", "elapsed_ms": 2000}]
            if who == "host":
                steps += [{"op": "key_down", "key": "Escape"}, {"op": "key_up", "key": "Escape"},
                          {"op": "wait", "screen": "Pause", "elapsed_ms": 400},
                          menu_step("assert_enabled ButtonEndMatch 1"), menu_step("activate ButtonEndMatch")]
            steps += [{"op": "wait", "service": "Starting", "scope": "menu"},
                      {"op": "assert", "equals": {"service": "Starting"}, "scope": "menu"},
                      # The lobby box's rect lands next to the net_ui rects in the same observation,
                      # so the overlay's placement against the lobby's own controls is on record.
                      {"op": "assert_control", "scope": "menu", "control": "MultiplayerLobbyPanel", "equals": {}},
                      {"op": "assert_control", "scope": "menu", "control": "MultiplayerScreen", "equals": {}},
                      {"op": "signal", "name": "done", "scope": "menu"}, {"op": "finish"}]
            probes[who] = {"schema": 1, "timeout_ms": 45000, "steps": steps}
        return ({who: text + f"wait_file {probe_root(root, who) / 'done.json'} 60\nassert_substate Lobby\nexit\n"
                 for who, text in (("host", host), ("client", client))}, probes)
    if case == "host-by-hand":
        # Every kind of control on the host options, operated the way a hand does: a press and its release on different
        # frames with the panel's own per-frame refresh in between, and the value read back frames after.
        text = (LANDING + "activate ButtonMultiplayerHostGame\nwait_ms 400\n"
                # Under the activity, the first words of what it is: a one-line description.
                "assert_label LabelHostActivityAbout Survive waves of AI-controlled enemies\n"
                "activate ButtonHostOptions\nwait_ms 400\nassert_substate HostOptions\n"
                # The draft starts from the activity's own rules: Skirmish Defense names no gold (2,000) and fog of war on.
                "activate TabHostPageRules\nwait 3\nassert_label LabelHostRulesGoldValue 2000 oz\nassert_checked CheckHostRulesFog 1\n"
                "activate TabHostPageSeats\nwait 3\n"
                + hand_pick("ComboHostSeatPlayers", "3") + hand_pick("ComboHostSeatPlayers", "4")
                # A new seat count keeps the rules.
                + "activate TabHostPageRules\nwait 3\n"
                + "assert_label LabelHostRulesGoldValue 2000 oz\nassert_checked CheckHostRulesFog 1\n"
                + hand_pick("ComboHostRulesMode", "Players versus AI")
                + hand_click("CheckHostRulesFog") + "assert_checked CheckHostRulesFog 0\n"
                + hand_click("CheckHostRulesFog") + "assert_checked CheckHostRulesFog 1\n"
                + hand_click("CheckHostRulesDeploy") + "assert_checked CheckHostRulesDeploy 1\n"
                + "slider_set SliderHostRulesDifficulty 80\nwait 4\nassert_label LabelHostRulesDifficultyValue 80\n"
                + "slider_set SliderHostRulesGold 5000\nwait 4\nassert_label LabelHostRulesGoldValue 5000 oz\n"
                # A team's rules stay its own: picking another team shows that team's and writes nothing onto it.
                + "slider_set SliderHostRulesSkill 30\nwait 4\nassert_label LabelHostRulesSkillValue 30\n"
                + hand_pick("ComboHostRulesTeam", "Team 2") + "assert_label LabelHostRulesSkillValue 50\n"
                + hand_pick("ComboHostRulesTeam", "Team 1") + "assert_label LabelHostRulesSkillValue 30\n"
                + "activate TabHostPageConnection\nwait 3\nscreenshot hand_net_connection\nwait 2\n"
                + "activate TabHostPageTiming\nwait 3\nscreenshot hand_net_delay\nwait 2\n"
                + hand_pick("ComboHostNetRedundancy", "7 ticks")
                + "activate TabHostPageRecovery\nwait 3\n"
                + hand_click("CheckHostRecRepair") + "assert_checked CheckHostRecRepair 0\n"
                + hand_pick("ComboHostRecReturnWindow", "10 minutes")
                + "activate TabHostPageSession\nwait 3\n"
                + hand_pick("ComboHostSessIdle", "20 minutes"))
        return {"host": text + "exit\n"}, {}
    if case == "host-follows-activity":
        # The host's rules start from the activity and follow it, as the Scenario screen's do: a new activity brings its
        # own gold, fog of war, clear path and deployment, a new difficulty its gold band, and a rule the host set himself stays.
        text = (LANDING + "activate ButtonMultiplayerHostGame\nwait_ms 400\nactivate ButtonHostOptions\nwait_ms 400\n"
                "assert_substate HostOptions\nactivate TabHostPageRules\nwait 3\n"
                # Wave Defense: 4,000 in its medium band, fog of war, clear path and deployment on.
                + hand_pick("ComboHostRulesActivity", "Wave Defense - Base.rte") + "wait 3\n"
                "assert_label LabelHostRulesGoldValue 4000 oz\nassert_checked CheckHostRulesFog 1\n"
                "assert_checked CheckHostRulesClearPath 1\nassert_checked CheckHostRulesDeploy 1\n"
                + "slider_set SliderHostRulesDifficulty 80\nwait 4\nassert_label LabelHostRulesGoldValue 3000 oz\n"
                + "slider_set SliderHostRulesGold 5000\nwait 4\nassert_label LabelHostRulesGoldValue 5000 oz\n"
                + hand_click("CheckHostRulesFog") + "assert_checked CheckHostRulesFog 0\n"
                # Skirmish Defense names no gold and no deployment; the gold and the fog of war are the host's now.
                + hand_pick("ComboHostRulesActivity", "Skirmish Defense - Base.rte") + "wait 3\n"
                "assert_label LabelHostRulesGoldValue 5000 oz\nassert_checked CheckHostRulesFog 0\n"
                "assert_checked CheckHostRulesClearPath 1\nassert_checked CheckHostRulesDeploy 0\n"
                + "slider_set SliderHostRulesDifficulty 20\nwait 4\nassert_label LabelHostRulesGoldValue 5000 oz\n")
        return {"host": text + "exit\n"}, {}
    if case == "prehost-visibility":
        text = (LANDING + "activate ButtonMultiplayerHostGame\nwait_ms 400\n"
                "activate ButtonHostOptions\nwait_ms 400\nactivate TabHostPageConnection\nwait_ms 400\n")
        for label in ("Unlisted", "Public (default)", "Local discovery"):
            text += f"combo_select ComboHostNetVisibility {label}\nwait_ms 500\nassert_label ComboHostNetVisibility {label}\n"
        return {"host": text + "exit\n"}, {}
    if case == "repair":
        return ({who: f"wait_file {probe_root(root, who) / 'done.json'} 90\nexit\n" for who in ("host", "client")},
                {who: repair_probe(who, root, size != "640x360") for who in ("host", "client")})
    if case == "save-hotkey":
        return ({who: f"wait_file {probe_root(root, 'client') / 'done.json'} 90\nexit\n" for who in ("host", "client")},
                {who: save_hotkey_probe(who, root) for who in ("host", "client")})
    if case == "pause-save":
        # Both peers stay until the client has read the save the host made.
        return ({who: f"wait_file {probe_root(root, 'client') / 'done.json'} 90\nexit\n" for who in ("host", "client")},
                {who: pause_save_probe(who, root) for who in ("host", "client")})
    if case == "pause":
        # Keep both peers until the leave has completed.
        return ({who: f"wait_file {probe_root(root, 'client') / 'left.json'} 90\nexit\n"
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
        text += (f"setup_host_port {port}\nactivate ButtonHostOptions\nwait 5\n"
                 "activate TabHostPageConnection\nwait 3\nactivate TabHostPageTiming\nwait 3\n"
                 "assert_label ComboHostNetRedundancy 6 ticks\n"
                 "assert_label TextHostNetSlowBound 3\n"
                 "assert_label ComboHostNetSlowPolicy Give the seat to the AI (host too) until they catch up\n"
                 # V1 offers the default policy only; the hint says the others come back.
                 "assert_combo_items ComboHostNetSlowPolicy Give the seat to the AI (host too) until they catch up\n"
                 "assert_label LabelHostNetSlowPolicyHint A player late past 3 ticks (50 ms), host too, is held to the AI while others play on. Other policies return in a later version.\n"
                 "assert_text_fits LabelHostNetSlowPolicyHint\n"
                 # Nothing drafted yet: Apply has nothing to commit.
                 "assert_enabled ButtonHostOptApply 0\n"
                 "combo_select ComboHostNetRedundancy 7 ticks\nwait 3\n"
                 "set_text TextHostNetSlowBound 7\nwait 3\n"
                 "assert_enabled ButtonHostOptApply 1\nactivate ButtonHostOptApply\nwait 3\n"
                 "assert_enabled ButtonHostOptApply 0\ndump_host_options\n"
                 "activate ButtonHostOptBack\nwait 3\nactivate ButtonMultiplayerCreate\nwait 15\n"
                 "activate ButtonLobbyOptions\nwait 3\nactivate TabHostPageConnection\nwait 3\nactivate TabHostPageTiming\nwait 3\n"
                 "assert_label ComboHostNetRedundancy 7 ticks\nassert_label TextHostNetSlowBound 7\n"
                 "assert_label LabelHostNetSlowPolicyHint A player late past 7 ticks (117 ms), host too, is held to the AI while others play on. Other policies return in a later version.\n"
                 "assert_combo_items ComboHostNetSlowPolicy Give the seat to the AI (host too) until they catch up\n"
                 "assert_enabled TextHostNetSlowBound 1\nassert_text_fits LabelHostNetSlowPolicyHint\n"
                 "dump_host_options\nexit\n")
    elif case == "landing":
        text = LANDING + "assert_label LabelMultiplayerNamePrompt Your name\n"
        text += checks("ButtonMultiplayerHostGame", "MultiplayerLandingPanel")
        text += "assert_visible ButtonMultiplayerCreate 0\n"
        text += "dump_host_options\n"
        # A name past the wire's 64-byte cap would die in the hello encode. A hand never gets one into the
        # box: it takes 24 printable characters, and a saved name over 64 bytes is refused when the
        # settings load. The command line's over-cap name is refused on the console (checked below).
        text += ("settext TextMultiplayerName " + "N" * (DISPLAY_NAME_MAX_BYTES + 1) + "\nwait 3\n"
                 "assert_box_text TextMultiplayerName " + "N" * 24 + "\n"
                 "dump_host_options\nexit\n")
    elif case == "net-resume":
        # The resume entry sits on the landing panel and opens a screen of its own. With no resumable
        # match on this private runtime the list is empty, the status says so in its own words and
        # Resume cannot be pressed - the state a player meets before any match has been checkpointed.
        text = LANDING + checks("ButtonMultiplayerResumeGame", "MultiplayerLandingPanel")
        text += "assert_label ButtonMultiplayerResumeGame Host Saved Match\n"
        text += "activate ButtonMultiplayerResumeGame\nwait 5\nassert_substate ResumeSetup\n"
        for name in RESUME_ROWS:
            text += checks(name, "MultiplayerResumePanel") if name != "ListResumeMatches" else \
                (f"assert_visible {name} 1\nassert_rect_inside {name} MultiplayerResumePanel\n"
                 f"assert_rect_inside {name} viewport\n")
        text += "assert_label LabelResumeTitle H O S T   S A V E D   M A T C H\n"
        text += "assert_label LabelResumeSelected Pick a saved match to host it again.\n"
        text += "assert_label LabelResumeStatus No saved matches found.\n"
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
                # A click opens the list, Down moves, Enter commits: the keys a player uses in an open list.
                text += ("combo_drop ComboBrainlessHumansSpectate\nwait 4\n"
                         "key_down Down\nwait 2\nkey_up Down\nwait 3\n"
                         "key_down Return\nwait 2\nkey_up Return\nwait 3\n"
                         "assert_label ComboBrainlessHumansSpectate End the match\n")
            text += "dump_player_options\n"
        text += "post_command ButtonBackToMainMenu\nwait 5\nassert_screen MainScreen\nexit\n"
    elif case == "network":
        # The saved preferences reach the player page, an edit on the page reaches the settings, and the
        # lobby's own name box shows what the page saved. A page switch is a page leave: the typed
        # name commits when the selector moves to chat and back.
        text = OPTIONS + "select_settings_page Network\nwait 3\nassert_settings_page Network\nassert_settings_page Network:Basics\n"
        text += "assert_visible CollectionBoxNetworkSettings 1\n"
        text += checks("TabNetworkSettings", "CollectionBoxSettingsBase")
        for control in BASICS_ROWS:
            text += checks(control, "CollectionBoxNetPageBasics")
        # The first view's rows keep apart whatever the Player page's delay policy hides.
        text += "assert_no_overlap_within CollectionBoxNetPageBasics\n"
        # The pages behind Advanced settings stay off the first view.
        for tab in NETWORK_TABS:
            text += f"assert_visible {tab} 0\n"
        text += ("assert_label TextNetworkDisplayName " + NETWORK_SEED["NetworkDisplayName"] + "\n"
                 # The saved Auto, in the player's words.
                 "assert_label ComboMatchStatusWidget When needed\n")
        text += "dump_player_options\n"
        text += "set_text TextNetworkDisplayName " + NETWORK_SAVED["NetworkDisplayName"] + "\n"
        text += "activate ButtonNetworkAdvanced\nwait 4\nassert_settings_page Network:Player\n"
        for tab in NETWORK_TABS:
            text += checks(tab, "CollectionBoxNetworkSettings")
        for index, box in enumerate(NETWORK_BOXES):
            text += f"assert_visible {box} {1 if index == 0 else 0}\n"
        for control in NETWORK_ROWS:
            text += checks(control, NETWORK_PAGE_BOX)
        text += ("assert_label TextNetworkIdleWait " + NETWORK_SEED["NetworkHostIdleWaitMinutes"] + "\n"
                 "assert_label TextNetworkPathHorizon " + NETWORK_SEED["NetworkPathHorizonTicks"] + "\n")
        # The saved policy is automatic here, so the fixed-delay row is not on the page at all.
        for control in NETWORK_FIXED_ROWS:
            text += f"assert_visible {control} 0\n"
        text += "dump_player_options\n"
        # A page switch is a page leave: the typed name committed when the selector moved, and the first view shows it.
        text += "select_settings_page Network:Chat\nwait 3\nassert_settings_page Network:Chat\nassert_visible CollectionBoxNetPagePlayer 0\n"
        text += "select_settings_page Network:Basics\nwait 3\nassert_label TextNetworkDisplayName " + NETWORK_SAVED["NetworkDisplayName"] + "\n"
        text += "activate ButtonNetworkAdvanced\nwait 4\nselect_settings_page Network:Player\nwait 3\n"
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
        # The host screen keeps the delay behind Advanced; the page flipped the policy to Fixed with 7 frames, so Advanced's
        # Timing page proposes that policy and floor for the next lobby.
        text += ("activate ButtonMultiplayerHostGame\nwait 5\nassert_substate HostSetup\n"
                 "assert_visible LabelHostInputDelayPolicy 0\nassert_visible TextHostInputDelay 0\n"
                 "activate ButtonHostOptions\nwait 5\nactivate TabHostPageTiming\nwait 3\n"
                 "assert_label ComboHostNetPolicy Fixed\nassert_label TextHostNetMinDelay 7\nassert_enabled TextHostNetMinDelay 1\ndump_host_options\n"
                 "activate ButtonHostOptBack\nwait 4\npost_command ButtonHostBack\nwait 4\nassert_substate Landing\ndump_host_options\nexit\n")
    elif case == "net-chat":
        # Every chat row is read where it is drawn; the muted-players button stays disabled with its
        # reason until a muted-players store exists.
        text = OPTIONS + net_page("Basics")
        for control in ("CheckboxNetworkChatVisible", "CheckboxNetworkChatNotify"):
            text += checks(control, "CollectionBoxNetPageBasics")
        text += net_page("Chat")
        for control in ("CheckboxNetworkChatSound", "LabelNetworkChatScope", "ComboNetworkChatScope", "LabelNetworkChatTextSize",
                        "ComboNetworkChatTextSize", "LabelNetworkChatKey", "TextNetworkChatKey",
                        "ButtonNetMutedPlayers", "LabelNetMutedReason"):
            text += checks(control, "CollectionBoxNetPageChat")
        text += ("assert_enabled ButtonNetMutedPlayers 0\n"
                 "assert_label LabelNetMutedReason managed in the match\n"
                 "assert_label ComboNetworkChatScope " + CHAT_SEED["NetworkChatDefaultScope"] + "\n"
                 "assert_label ComboNetworkChatTextSize " + CHAT_SEED["NetworkChatTextSize"] + "\n"
                 "dump_player_options\n"
                 + net_page("Basics") +
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
                        "LabelNetAutosaveIntTitle", "LabelNetAutosaveInterval",
                        "LabelNetAutosavesKeptTitle", "TextNetworkAutosavesKept", "LabelNetAutosavesKeptHint",
                        "LabelNetAutosaveInfo", "ButtonNetOpenAutosaves", "ButtonNetCopyAutosavesPath",
                        "LabelNetDiagDirTitle", "TextNetworkDiagDir", "ButtonNetOpenDiagnostics",
                        "ButtonNetCopyDiagPath", "ButtonNetSaveDiagnostics", "CheckboxNetworkRecordReplays"):
            text += checks(control, "CollectionBoxNetPageFiles")
        # The seeded 45 is under the minute a hosted match clamps to; the page shows that 60 s.
        text += ("assert_label LabelNetAutosave Enabled\n"
                 "assert_label LabelNetAutosaveInterval 60 s\n"
                 "assert_label LabelNetAutosaveHost Both set by the host\n"
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
                        "LabelNetInternetReason"):
            text += checks(control, "CollectionBoxNetPageInternet")
        # Replays, connection details and the relay live on the multiplayer screens: the page hides their buttons.
        text += ("assert_visible ButtonNetReplays 0\nassert_visible ButtonNetConnDetails 0\n"
                 "assert_visible ButtonNetNatRelay 0\nassert_label LabelNetDirStatus Configured\n"
                 "assert_label LabelNetDirUrlHint " + INTERNET_HINT + "\n"
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
                 "assert_visible LabelHostInputDelayPolicy 0\nassert_visible TextHostInputDelay 0\nassert_visible TextHostPort 0\n"
                 "dump_host_options\n"
                 # The seeded policy is automatic and Advanced's Timing page says so; autosave is off, so its interval box is off.
                 "activate ButtonHostOptions\nwait 5\nactivate TabHostPageTiming\nwait 3\nassert_label ComboHostNetPolicy Automatic\ndump_host_options\n"
                 "activate TabHostPageRecovery\nwait 3\nassert_enabled TextHostRecAutosaveInterval 0\ndump_host_options\n"
                 "activate ButtonHostOptBack\nwait 4\nassert_substate HostSetup\n"
                 f"setup_host_port {port}\ncombo_select ComboHostPlayers 2\n"
                 "activate ButtonMultiplayerCreate\nwait 15\nassert_substate Lobby\n"
                 # The listing keeps its public default, but a run's settings name no directory: this host registers nowhere.
                 "assert_label LabelLobbyPortMap Shown to players on your network only\n"
                 "assert_text_fits LabelLobbyPortMap\n"
                 "dump_host_options\nexit\n")
    elif case in ("net-host-left", "net-host-left-early"):
        done = probe_root(root, "host") / "done.json"
        client_frame = probe_root(root, "client") / "client_frame.json"
        host = (LANDING + "activate ButtonMultiplayerHostGame\nwait 5\n"
                "combo_select ComboHostActivity P4 Alpha Duel\nwait 5\n"
                f"setup_host_port {port}\ncombo_select ComboHostPlayers 2\n"
                "activate ButtonMultiplayerCreate\nwait_connected 2 60\nwait_remote_ready 60\n"
                # The Start button takes the remote ready on the menu's next update.
                "wait 3\nactivate ButtonMultiplayerStart\n")
        client = (LANDING + "activate ButtonMultiplayerJoinGame\nwait 5\n"
                  f"activate ButtonJoinByAddress\nwait 4\nsettext TextJoinAddress 127.0.0.1\nsettext TextJoinPort {port}\n"
                  "activate ButtonJoinAddressGo\nwait_connected 2 60\nwait_substate Lobby 30\nwait 5\n"
                  "activate ButtonMultiplayerReady\n")
        if case == "net-host-left":
            host += f"wait_file {done} 90\nwait_ms 500\ndump_lobby\ndump_host_options\nexit\n"
            client += f"wait_file {done} 90\n"
            client += (# The failure's error text lands before the menu reconciles the subscreen to
                       # Landing; assert_status reads the lobby label while it is still there.
                       "wait_error The host left the match\n"
                       "wait_label LabelMultiplayerLandingStatus The host left the match\n"
                       "assert_status The host left the match\n"
                       "assert_substate Landing\nassert_visible LabelMultiplayerLandingStatus 1\n"
                       "assert_label LabelMultiplayerLandingStatus The host left the match\n"
                       "assert_text_fits LabelMultiplayerLandingStatus\ndump_lobby\ndump_host_options\nexit\n")
        else:
            # The driver suspends the client mid-launch and terminates the host while its seat is
            # held; the parked host never reaches a menu step past the start.
            host += "wait 99999\n"
            client += (# A launch that dies with its host has no sealed seat to reclaim, so no
                       # reconnect or rematch lobby is offered and the player lands back on the
                       # main screen: the departure verdict is the service's own error, state and
                       # roster, which dump_lobby prints.
                       "wait_error The host left the match\nwait_state Failed 30\n"
                       "dump_lobby\ndump_reconnect\ndump_host_options\nexit\n")
        leave = [{"op": "key_down", "key": "Escape"}, {"op": "key_up", "key": "Escape"},
                 {"op": "wait", "screen": "Pause"}, {"op": "wait", "renders": 2}, menu_step("activate ButtonLeaveMatch"),
                 {"op": "wait", "screen": "PauseLeaveConfirm"}, {"op": "wait", "renders": 2}, menu_step("activate ButtonLeaveConfirm"),
                 {"op": "wait", "scope": "menu", "elapsed_ms": 500},
                 {"op": "signal", "name": "done", "scope": "menu"}, {"op": "finish"}]
        if case == "net-host-left":
            # The host leaves only after the client's own round has committed frames, so the case
            # measures a mid-match departure on any machine load instead of racing the client's start.
            # The round's own committed frame: the launch counter also counts the lobby's ticks.
            host_steps = [{"op": "wait", "service": "Running", "screen": "Gameplay", "lockstep_frame_at_least": 150},
                          {"op": "wait_file", "path": str(client_frame)},
                          {"op": "wait", "elapsed_ms": 500},
                          {"op": "assert", "equals": {"paused": False, "service": "Running"}}] + leave
            # The survivor lands on the multiplayer landing once the dead rematch lobby reports Failed;
            # its panel rect sits beside the net_ui rects in that observation.
            client_probe = {"schema": 1, "timeout_ms": 90000, "steps": [
                {"op": "wait", "service": "Running", "screen": "Gameplay", "lockstep_frame_at_least": 30},
                {"op": "signal", "name": "client_frame"},
                # A menu-scope wait also runs on game frames: the landing is read once the menu itself is back.
                {"op": "wait", "scope": "menu", "service": "Failed", "screen": "MultiplayerScreen"},
                {"op": "assert_control", "scope": "menu", "control": "MultiplayerLandingPanel", "equals": {}},
                {"op": "assert_control", "scope": "menu", "control": "MultiplayerScreen", "equals": {}},
                {"op": "finish"}]}
            return {"host": host, "client": client}, {
                "host": {"schema": 1, "timeout_ms": 90000, "steps": host_steps},
                "client": client_probe}
        # The client signals when its service commits to the launch; the run itself owns the
        # host's departure (see the hold/terminate pair in run_case), so no host probe rides along.
        return {"host": host, "client": client}, {
            "client": {"schema": 1, "timeout_ms": 90000, "steps": [
                {"op": "wait", "service": "Running"},
                {"op": "signal", "name": "client_rtl"},
                {"op": "wait", "service": "Failed"},
                {"op": "signal", "name": "client_done"}, {"op": "finish"}]}}
    elif case == "net-activity":
        # Fresh host setup uses Skirmish Defense; the keyboard anchor is the row above the picked one.
        # A vanished pick needs a module unload the menu harness cannot drive; the native
        # host_request_fallback row covers the empty-list Base.rte request fields instead.
        # The keyboard commits the activity and scene combos and the mouse the mode combo; Create
        # uses those picks, not a later mouse Select.
        host = (LANDING + "activate ButtonMultiplayerHostGame\nwait 5\n"
                "assert_visible LabelHostActivity 1\nassert_label LabelHostActivity Activity\n"
                "assert_label ComboHostActivity Skirmish Defense\n"
                "assert_text_fits ComboHostActivity\n"
                "dump_host_options\n"
                "assert_visible LabelHostScene 1\nassert_label LabelHostScene Scene\n"
                "assert_visible ComboHostScene 1\n"
                "assert_text_fits ComboHostScene\n"
                "assert_visible ComboHostMode 1\nassert_label ComboHostMode Players versus players\n"
                "assert_label LabelHostInfo Grasslands - 2 players on separate teams\n"
                "combo_select ComboHostActivity Persistent World\nwait 3\n"
                # A hand opens the list; the arrow keys and Return pick in it.
                "combo_drop ComboHostActivity\nwait 3\ndump_host_options\n"
                "key_down Down\nwait 2\nkey_up Down\nwait 3\n"
                "key_down Return\nwait 2\nkey_up Return\nwait 3\n"
                "assert_label ComboHostActivity Brain vs Brain\ndump_host_options\n"
                "combo_drop ComboHostScene\nwait 3\ndump_host_options\n"
                "key_down Down\nwait 2\nkey_up Down\nwait 3\n"
                "key_down Return\nwait 2\nkey_up Return\nwait 3\n"
                "assert_label ComboHostScene Fredeleig Bunkers\ndump_host_options\n"
                # combo_drop is the same panel-level click a user makes; the dumped capture shows the
                # list open. combo_select picks the row by its text the way a click on it would.
                "combo_drop ComboHostMode\nwait 3\ndump_host_options\n"
                "combo_select ComboHostMode Players versus AI\nwait 3\n"
                "assert_label ComboHostMode Players versus AI\n"
                "assert_label LabelHostInfo Fredeleig Bunkers - 2 players together against an AI team\ndump_host_options\n"
                f"setup_host_port {port}\ncombo_select ComboHostPlayers 2\n"
                "activate ButtonMultiplayerCreate\nwait 15\nassert_substate Lobby\n"
                "wait_label LabelLobbyPlayer1 Joiner\n"
                f"wait_file {(root / 'client/runtime/ScreenShots/dump_host_options_0.json').as_posix()} 60\n"
                "assert_label LabelLobbyMatch Brain vs Brain\nassert_label_absent LabelLobbyMatch Base.rte\n"
                "assert_label LabelLobbyMatchMode Fredeleig Bunkers - Players versus AI\n"
                "assert_text_fits LabelLobbyMatch\nassert_text_fits LabelLobbyMatchMode\n"
                "assert_text_fits LabelLobbyPlayersHeader\n"
                "assert_text_fits LabelLobbyPlayer0\nassert_text_fits LabelLobbyPlayer1\n"
                # The host's options panel edits the adopted config; its Rules page carries the
                # picked activity/mode and the L33 row the ledger names.
                "activate ButtonLobbyOptions\nwait 5\nassert_substate HostOptions\n"
                "assert_label LabelHostOptionsTitle A D V A N C E D\nactivate TabHostPageSeats\nwait 3\n"
                # H09/H10: the host's own seat is never kickable, whoever else is in the lobby.
                "activate ButtonHostSeatDetails0\nwait 3\nassert_visible HostSeatDialog 1\n"
                "assert_enabled ButtonHostSeatDlgKick 0\nassert_enabled ButtonHostSeatDlgBan 0\n"
                "assert_label LabelHostSeatDlgActionHint The host's own seat is never kicked or banned.\n"
                "activate ButtonHostSeatDlgClose\nwait 3\nassert_visible HostSeatDialog 0\n"
                # On the two-peer fixture the adopted config names both seated humans, the lobby is kept to
                # this network, and Apply refuses a port edit mid-session.
                "activate TabHostPageTiming\nwait 3\nassert_visible CollectionBoxHostPageTiming 1\n"
                "assert_label LabelHostNetMode Host mode: Playing - capacity 2 - humans seated 2\n"
                "activate TabHostPageConnection\nwait 3\nassert_label ComboHostNetVisibility Local discovery\n"
                "set_text TextHostNetPort 40000\nwait 3\nactivate ButtonHostOptApply\nwait 3\n"
                "assert_label LabelHostOptStatus Close this lobby to change the game port\n"
                f"set_text TextHostNetPort {port}\nwait 3\nassert_label TextHostNetPort {port}\n"
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
                "assert_label ComboHostRulesMode Players versus AI\n"
                "dump_host_options\n"
                # The live republish: the host's Apply moves every peer's adopted config to the
                # next revision. The status line reads the acknowledge, then the client's Details
                # below reads the new value off its own mirror.
                "combo_select ComboHostRulesBrainless End the match\nwait 3\n"
                "assert_label ComboHostRulesBrainless End the match\n"
                "activate ButtonHostOptApply\nwait 5\ndump_host_options\n"
                "activate ButtonHostOptBack\nwait 5\nassert_substate Lobby\n"
                "dump_lobby\ndump_host_options\n"
                # The client finishes its adopted-config read before the host removes it.
                f"wait_file {(root / 'client/runtime/ScreenShots/dump_host_options_2.json').as_posix()} 60\n"
                "activate ButtonLobbyOptions\nwait 5\nassert_substate HostOptions\n"
                "activate TabHostPageSeats\nwait 3\nassert_visible CollectionBoxHostPageSeats 1\n"
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
                # The kicked seat is open again: it reads open, never CPU nor the removed player, and the
                # kicked client rejoins it below - only the ban list keeps an identity out.
                "dump_lobby\nassert_label LabelLobbyPlayer1 Open seat - Team 1\n"
                "assert_label_absent LabelLobbyPlayer0 Joiner\nassert_label_absent LabelLobbyPlayer1 Joiner\n"
                # The computer's seat, in the lobby's words.
                "assert_label LabelLobbyPlayer2 AI - Team\n"
                # The client's rejoin waits on this dump, taken on the options panel so the lobby keeps one capture.
                "activate ButtonLobbyOptions\nwait 5\nassert_substate HostOptions\ndump_host_options\n"
                "activate ButtonHostOptBack\nwait 5\nassert_substate Lobby\n"
                # The kicked client's own rejoin is admitted and seats it under its name again.
                "wait_label LabelLobbyPlayer1 Joiner\n"
                "dump_lobby\n"
                # The ban list is the only refusal: the host bans the re-seated joiner, the session
                # ban list names it, and its next join draws the plain refusal on the error line.
                "activate ButtonLobbyOptions\nwait 5\nassert_substate HostOptions\n"
                "activate TabHostPageSeats\nwait 3\nassert_visible CollectionBoxHostPageSeats 1\n"
                "activate ButtonHostSeatDetails1\nwait 3\nassert_visible HostSeatDialog 1\n"
                "assert_label LabelHostSeatDlgName Joiner\n"
                "assert_enabled ButtonHostSeatDlgBan 1\n"
                "activate ButtonHostSeatDlgBan\nwait 10\n"
                "assert_visible HostSeatDialog 0\n"
                "assert_label LabelHostOptStatus Ban: Ok\n"
                "activate TabHostPageSession\nwait 3\n"
                "activate ButtonHostSessBanned\nwait 5\n"
                "assert_visible HostBannedDialog 1\nassert_label LabelHostBannedList Joiner\n"
                "activate ButtonHostBannedClose\nwait 3\n"
                "activate ButtonHostOptBack\nwait 5\nassert_substate Lobby\n"
                "wait_label LabelMultiplayerError Joiner was refused: banned from this session\n"
                # The last dump stays the post-Apply lobby readback: a second Lobby capture here
                # would break the paired check that counts one lobby-mode capture per peer.
                "dump_lobby\nwait 600\nexit\n")
        client = (LANDING + "settext TextMultiplayerName Joiner\n"
                  "activate ButtonMultiplayerJoinGame\nwait 10\n"
                  "activate ButtonJoinByAddress\nwait 4\nsettext TextJoinAddress 127.0.0.1\n"
                  f"settext TextJoinPort {port}\n"
                  f"wait_file {(probe_root(root, 'host') / 'hosting.json').as_posix()} 60\n"
                  "activate ButtonJoinAddressGo\n"
                  "wait_label LabelLobbyPlayer1 Joiner\nwait_activity Brain vs Brain\nwait 12\n"
                  "assert_substate Lobby\n"
                  "assert_label LabelLobbyMatch Brain vs Brain\nassert_label_absent LabelLobbyMatch Base.rte\n"
                  "assert_label LabelLobbyMatchMode Fredeleig Bunkers - Players versus AI\n"
                  "assert_text_fits LabelLobbyMatch\nassert_text_fits LabelLobbyMatchMode\n"
                  "assert_text_fits LabelLobbyPlayersHeader\n"
                  "assert_text_fits LabelLobbyPlayer0\nassert_text_fits LabelLobbyPlayer1\n"
                  # The client's Match details button opens the same adopted config as a read-only
                  # details view: every edit control is disabled, the L33 row reads identically,
                  # and the title names what it is.
                  "activate ButtonLobbyEditSetup\nwait 5\nassert_substate HostOptions\n"
                  "assert_label LabelHostOptionsTitle M A T C H   D E T A I L S\n"
                  "activate TabHostPageRules\nwait 3\nassert_visible CollectionBoxHostPageRules 1\n"
                  "assert_label LabelHostRulesBrainless When every human brain is lost\n"
                  "assert_label ComboHostRulesBrainless Keep playing, humans spectate\n"
                  "assert_enabled ComboHostRulesBrainless 0\n"
                  "assert_enabled ComboHostRulesActivity 0\n"
                  "assert_label ComboHostRulesActivity Brain vs Brain - Base.rte\n"
                  "assert_label ComboHostRulesMode Players versus AI\n"
                  "assert_enabled ButtonHostOptApply 0\n"
                  "dump_host_options\n"
                  "activate ButtonHostOptBack\nwait 5\nassert_substate Lobby\n"
                  # The host's dump follows Apply; the client reads the resulting revision.
                  f"wait_file {(root / 'host/runtime/ScreenShots/dump_host_options_8.json').as_posix()} 60\n"
                  "activate ButtonLobbyEditSetup\nwait 5\nassert_substate HostOptions\n"
                  "assert_label LabelHostOptionsTitle M A T C H   D E T A I L S\n"
                  "activate TabHostPageRules\nwait 3\nassert_visible CollectionBoxHostPageRules 1\n"
                  "wait_label ComboHostRulesBrainless End the match\n"
                  "assert_label ComboHostRulesBrainless End the match\n"
                  "dump_host_options\n"
                  # A client's Details dialog is read-only: the host's own seat and every other
                  # seat keep Kick and Ban off - moderation is never the client's call.
                  "activate TabHostPageSeats\nwait 3\nassert_visible CollectionBoxHostPageSeats 1\n"
                  "activate ButtonHostSeatDetails1\nwait 3\nassert_visible HostSeatDialog 1\n"
                  "assert_enabled ButtonHostSeatDlgKick 0\nassert_enabled ButtonHostSeatDlgBan 0\n"
                  "activate ButtonHostSeatDlgClose\nwait 3\nassert_visible HostSeatDialog 0\n"
                  "activate ButtonHostOptBack\nwait 5\nassert_substate Lobby\n"
                  "dump_lobby\ndump_host_options\n"
                  # The host kicks this peer once its own pass is done: the lobby that vanishes has
                  # to name the removal instead of reading as a network fault.
                  "wait_state Failed 60\n"
                  "assert_status The host removed you from this session\n"
                  "assert_substate Landing\n"
                  "dump_lobby\n"
                  # The host's post-kick asserts read the open seat before this identity knocks again.
                  f"wait_file {(root / 'host/runtime/ScreenShots/dump_host_options_11.json').as_posix()} 60\n"
                  # A kick is not a ban: the same identity joins again and lands back in the lobby.
                  "activate ButtonMultiplayerJoinGame\nwait 5\n"
                  "activate ButtonJoinByAddress\nwait 4\nsettext TextJoinAddress 127.0.0.1\n"
                  f"settext TextJoinPort {port}\n"
                  "activate ButtonJoinAddressGo\n"
                  "wait_connected 3 60\n"
                  "assert_substate Lobby\n"
                  "assert_label LabelLobbyPlayer1 Joiner\n"
                  "dump_lobby\n"
                  # The host bans the re-seated peer next: the removal and the refused rejoin after
                  # it both read the ban sentence, and the lobby never comes back.
                  "wait_state Failed 60\n"
                  "assert_status The host banned you from this session\n"
                  "assert_substate Landing\n"
                  "activate ButtonMultiplayerJoinGame\nwait 5\n"
                  "activate ButtonJoinByAddress\nwait 4\nsettext TextJoinAddress 127.0.0.1\n"
                  f"settext TextJoinPort {port}\n"
                  "activate ButtonJoinAddressGo\n"
                  "wait_state Failed 60\n"
                  "assert_status The host banned you from this session\n"
                  # No trailing dump: a refused peer's lobby view is its own local default config,
                  # and the paired check reads each side's last dump_lobby for the same match.
                  # A join refused with nothing to answer stays on the Join screen with its reason.
                  "assert_substate JoinSetup\nexit\n")
        return {"host": host, "client": client}, {
            "host": {"schema": 1, "timeout_ms": 90000, "steps": [
                {"op": "wait", "service": "Starting", "scope": "menu"},
                {"op": "signal", "name": "hosting", "scope": "menu"},
                {"op": "wait_file", "path": str(root / "host/runtime/ScreenShots/dump_host_options_10.json"), "scope": "menu"},
                {"op": "finish"}]}}
    elif case == "net-options":
        # Two real peers: the host's saved session options ride the lobby config onto both rosters. A
        # match end quits e2e peers outright, so the host's own pause-menu leave is what pauses the
        # activity and lets its menu loop run the dump while the roster is still up.
        return ({"host": f"dump_lobby\nwait_file {probe_root(root, 'host') / 'left.json'} 90\nexit\n",
                 "client": f"wait_file {probe_root(root, 'host') / 'left.json'} 90\nexit\n"},
                {"host": {"schema": 1, "timeout_ms": 90000, "steps": [
                    {"op": "wait", "sim_at_least": 150},
                    {"op": "assert", "equals": {"service": "Running", "paused": False}, "sim_at_least": 150},
                    {"op": "key_down", "key": "Escape"}, {"op": "key_up", "key": "Escape"},
                    {"op": "wait", "screen": "Pause"}, menu_step("dump_host_options"),
                    {"op": "signal", "name": "done"},
                    menu_step("activate ButtonLeaveMatch"), {"op": "wait", "screen": "PauseLeaveConfirm"},
                    menu_step("activate ButtonLeaveConfirm"),
                    {"op": "wait", "service": "Completed", "scope": "menu"},
                    {"op": "signal", "name": "left", "scope": "menu"}, {"op": "finish"}]}})
    elif case == "combo-fit":
        # Reached by the tab control so the measurement runs on a build that has no page op yet. Which
        # resolution box the page shows depends on the display the engine measured, so the dump decides it.
        text = (OPTIONS + "activate TabVideoSettings\nwait 3\ndump_player_options\nexit\n")
    elif case == "lobby":
        # The host starts the match, so its lobby hides the ready button the joining peers get.
        text = host_lobby(port) + "assert_visible ButtonMultiplayerReady 0\n"
        text += checks("ButtonMultiplayerLeave", "MultiplayerLobbyPanel")
        # Players (the moderation panel) is a match feature: the lobby before a match does not show it. The dump lands
        # before the header and readiness asserts so the capture exists either way.
        text += "assert_visible ButtonMultiplayerModerate 0\n"
        text += "dump_host_options\n"
        text += checks("LabelLobbyPlayersHeader", "MultiplayerLobbyPanel")
        # The lobby shows the main menu's version line inside its panel, under the chat entry.
        text += checks("LabelLobbyVersion", "MultiplayerLobbyPanel")
        text += f"assert_label LabelLobbyVersion {version_line()}\nassert_no_overlap LabelLobbyVersion TextLobbyChat\n"
        text += "assert_enabled ButtonMultiplayerStart 0\n"
        # H01-H35: the six host-options pages behind the lobby's Options button. Each tab shows its
        # own collection box, every visited control answers the fit checks, and the dump rows land
        # on each page so the capture names the page's whole surface at this size.
        text += "activate ButtonLobbyOptions\nwait 5\nassert_substate HostOptions\nassert_visible CollectionBoxHostPageConnection 1\n"
        text += "activate TabHostPageSeats\nwait 3\nassert_visible MultiplayerHostOptionsPanel 1\n"
        text += "assert_label LabelHostOptionsTitle A D V A N C E D\n"
        text += checks("TabHostPageSeats", "MultiplayerHostOptionsPanel")
        text += checks("TabHostPageRules", "MultiplayerHostOptionsPanel")
        text += checks("TabHostPageConnection", "MultiplayerHostOptionsPanel")
        text += checks("TabHostPageTiming", "MultiplayerHostOptionsPanel")
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
        # A seated human's kind never moves, and an open lobby's human seat stays its peer's: the host's
        # own row and the joined player's row offer no other kind, so a hand cannot open them.
        text += "assert_enabled ComboHostSeatType0 0\n"
        text += "wait_members 2\nwait 3\nassert_enabled ComboHostSeatType1 0\n"
        # A two-peer lobby has no free peer id, so the closed tail turns a hand's human seat down
        # with the reason in the status line, then accepts the peerless CPU seat the same row offers.
        text += ("combo_refused ComboHostSeatType2 Open\nwait 3\n"
                 "assert_label LabelHostOptStatus No free seat\n")
        text += ("combo_select ComboHostSeatType2 CPU\nwait 3\n"
                 "assert_label LabelHostOptStatus Unsaved changes\n"
                 "assert_label LabelHostSeatState2 CPU / Skill\n")
        text += "assert_no_overlap_within CollectionBoxHostPageSeats\n"
        text += "dump_host_options\n"
        # H04-H11: the seat's Details dialog - the seat's identity, the reclaim clock's line, the
        # applicant row and the moderation actions, Kick and Ban among them.
        text += ("activate ButtonHostSeatDetails0\nwait 3\nassert_visible HostSeatDialog 1\n")
        for control in ("LabelHostSeatDlgName", "LabelHostSeatDlgSeat", "LabelHostSeatDlgTeam",
                        "LabelHostSeatDlgState", "LabelHostSeatDlgReclaim", "LabelHostSeatDlgApplicants",
                        "ButtonHostSeatDlgWait", "ButtonHostSeatDlgApprove",
                        "ButtonHostSeatDlgCancel", "ButtonHostSeatDlgKick", "ButtonHostSeatDlgBan",
                        "LabelHostSeatDlgActionHint", "LabelHostSeatDlgStatus", "ButtonHostSeatDlgClose"):
            text += checks(control, "HostSeatDialog")
        # Nobody has applied: the line above the list says so, and the empty list has no row to measure.
        text += ("assert_label LabelHostSeatDlgApplicants Nobody is asking for this seat\n"
                 "assert_visible ListHostSeatDlgApplicants 1\nassert_rect_inside ListHostSeatDlgApplicants HostSeatDialog\n"
                 "assert_rect_inside ListHostSeatDlgApplicants viewport\nassert_list_rows ListHostSeatDlgApplicants 0\n")
        # H09/H10: the host's own seat is never kickable - the row stays pressable-looking but off.
        text += ("assert_enabled ButtonHostSeatDlgKick 0\nassert_enabled ButtonHostSeatDlgBan 0\n"
                 "dump_host_options\nactivate ButtonHostSeatDlgClose\nwait 3\n"
                 "assert_visible HostSeatDialog 0\n")
        # H07-H20 Rules: the L33 row keeps the ledger's exact label and pair of answers.
        text += "activate TabHostPageRules\nwait 3\nassert_visible CollectionBoxHostPageRules 1\n"
        text += "assert_label LabelHostOptionsTitle A D V A N C E D\n"
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
        text += "assert_no_overlap_within CollectionBoxHostPageRules\n"
        text += "dump_host_options\n"
        # Timing: the delay policy and its numbers, the redundancy, the slow-player bound and policy.
        text += "activate TabHostPageTiming\nwait 3\nassert_visible CollectionBoxHostPageTiming 1\n"
        text += "assert_label LabelHostOptionsTitle A D V A N C E D\n"
        text += checks("ComboHostNetPolicy", "CollectionBoxHostPageTiming")
        text += checks("LabelHostNetRedundancy", "CollectionBoxHostPageTiming")
        text += checks("ComboHostNetRedundancy", "CollectionBoxHostPageTiming")
        # The saved host defaults' 6 ticks, not the 4-tick default.
        text += "assert_label ComboHostNetRedundancy 6 ticks\n"
        text += checks("TextHostNetMinDelay", "CollectionBoxHostPageTiming")
        for control in ("LabelHostNetSlowBound", "TextHostNetSlowBound", "LabelHostNetSlowBoundHint", "LabelHostNetSlowPolicy", "ComboHostNetSlowPolicy",
                        "LabelHostNetSlowPolicyHint"):
            text += checks(control, "CollectionBoxHostPageTiming")
        text += "assert_label TextHostNetSlowBound 3\n"
        text += "assert_label ComboHostNetSlowPolicy Give the seat to the AI (host too) until they catch up\n"
        text += "assert_label LabelHostNetSlowPolicyHint A player late past 3 ticks (50 ms), host too, is held to the AI while others play on. Other policies return in a later version.\n"
        text += "assert_label LabelHostNetEffective Effective delay: ping plus a 3-tick margin, raised live if inputs arrive late, at least\n"
        text += checks("LabelHostNetEffective", "CollectionBoxHostPageTiming")
        text += checks("ButtonHostNetRecalc", "CollectionBoxHostPageTiming")
        # The host row names mode/capacity/seated humans off the adopted config.
        text += checks("LabelHostNetMode", "CollectionBoxHostPageTiming")
        text += "assert_label LabelHostNetMode Host mode: Playing - capacity 2 - humans seated 1\n"
        text += "assert_no_overlap_within CollectionBoxHostPageTiming\n"
        text += "dump_host_options\n"
        # Connection: the listing in its honest names and the port, this computer's choices that commit with Apply.
        text += "activate TabHostPageConnection\nwait 3\nassert_visible CollectionBoxHostPageConnection 1\n"
        text += checks("ComboHostNetVisibility", "CollectionBoxHostPageConnection")
        text += "assert_label ComboHostNetVisibility Local discovery\n"
        text += checks("TextHostNetPort", "CollectionBoxHostPageConnection")
        text += f"assert_label TextHostNetPort {port}\n"
        # An online listing needs the game list service this fixture lacks: Apply refuses it with the reason and the page keeps
        # the pick to change or cancel.
        text += ("combo_select ComboHostNetVisibility Public (default)\nwait 3\nactivate ButtonHostOptApply\nwait 3\n"
                 "assert_label LabelHostOptStatus An online listing needs the online game list service\n"
                 "assert_label ComboHostNetVisibility Public (default)\n"
                 "combo_select ComboHostNetVisibility Unlisted\nwait 3\nactivate ButtonHostOptApply\nwait 3\n"
                 "assert_label LabelHostOptStatus An online listing needs the online game list service\n"
                 "combo_select ComboHostNetVisibility Local discovery\nwait 3\nassert_label ComboHostNetVisibility Local discovery\n")
        # A hosted lobby owns its bound port: Apply refuses the edit and names the way to change it.
        text += ("set_text TextHostNetPort 40000\nwait 3\nactivate ButtonHostOptApply\nwait 3\n"
                 "assert_label LabelHostOptStatus Close this lobby to change the game port\n"
                 f"set_text TextHostNetPort {port}\nwait 3\nassert_label TextHostNetPort {port}\n")
        text += "assert_no_overlap_within CollectionBoxHostPageConnection\n"
        text += "dump_host_options\n"
        # H25-H28 Recovery.
        text += "activate TabHostPageRecovery\nwait 3\nassert_visible CollectionBoxHostPageRecovery 1\n"
        text += "assert_label LabelHostOptionsTitle A D V A N C E D\n"
        text += checks("CheckHostRecRepair", "CollectionBoxHostPageRecovery")
        text += checks("CheckHostRecAutosave", "CollectionBoxHostPageRecovery")
        text += checks("LabelHostRecAutosaveHint", "CollectionBoxHostPageRecovery")
        text += checks("LabelHostRecAutosaveNote", "CollectionBoxHostPageRecovery")
        text += "assert_label LabelHostRecAutosaveNote Every player takes each checkpoint at the same tick; a player who rejoins starts from one.\n"
        text += checks("LabelHostRecAutosaveCost", "CollectionBoxHostPageRecovery")
        text += "assert_label LabelHostRecAutosaveCost Saving may cause a brief pause for other players on slower hosts\n"
        # The return window: five minutes by default, its consequence in the page's hint area when the row is pointed at, a pick drafted at once.
        for control in ("LabelHostRecReturnWindow", "ComboHostRecReturnWindow", "LabelHostRecOptionHint"):
            text += checks(control, "CollectionBoxHostPageRecovery")
        text += ("assert_label LabelHostRecReturnWindow Return window\n"
                 "assert_label ComboHostRecReturnWindow 5 minutes\n"
                 f"focus ComboHostRecReturnWindow\nwait_ms 300\nassert_label LabelHostRecOptionHint {host_hint('NetReturnWindowHint')}\n"
                 "combo_select ComboHostRecReturnWindow 10 minutes\nwait_ms 500\n"
                 "assert_label ComboHostRecReturnWindow 10 minutes\n")
        # This host's own world history and catch-up limit: the saved settings by default, each consequence in the hint area while its
        # row is pointed at or focused (the last one stays), a pick taken by the host at once (the host's log names the policy it now runs).
        for control in ("LabelHostRecJoinHistory", "ComboHostRecJoinHistory", "LabelHostRecJoinLag", "ComboHostRecJoinLag"):
            text += checks(control, "CollectionBoxHostPageRecovery")
        text += ("assert_label LabelHostRecJoinHistory World history\n"
                 "assert_label ComboHostRecJoinHistory 6 minutes\n"
                 "assert_label LabelHostRecJoinLag Catch-up limit\n"
                 "assert_label ComboHostRecJoinLag 2 minutes\n"
                 f"focus ComboHostRecJoinHistory\nwait_ms 300\nassert_label LabelHostRecOptionHint {host_hint('NetJoinHistoryHint')}\n"
                 "assert_text_fits LabelHostRecOptionHint\n"
                 f"focus ComboHostRecJoinLag\nwait_ms 300\nassert_label LabelHostRecOptionHint {host_hint('NetJoinLagHint')}\n"
                 "assert_text_fits LabelHostRecOptionHint\n"
                 f"focus ComboHostRecReturnWindow\nwait_ms 300\nassert_label LabelHostRecOptionHint {host_hint('NetReturnWindowHint')}\n"
                 "assert_text_fits LabelHostRecOptionHint\n"
                 "combo_select ComboHostRecJoinHistory 10 minutes\nwait_ms 500\n"
                 "assert_label ComboHostRecJoinHistory 10 minutes\n"
                 "combo_select ComboHostRecJoinLag 1 minute\nwait_ms 500\n"
                 "assert_label ComboHostRecJoinLag 1 minute\n")
        text += ("setcheck CheckHostRecAutosave 1\nwait_ms 500\nassert_checked CheckHostRecAutosave 1\n"
                 "assert_enabled TextHostRecAutosaveInterval 1\n"
                 # Switching autosave on from off starts at the shortest cadence, not the off zero.
                 "assert_label TextHostRecAutosaveInterval 60\n"
                 "assert_label LabelHostRecLastSave Checkpoint every 60 sim seconds - none saved yet\n"
                 # ENGINE 166: the caption follows the typed interval on the Changed notification,
                 # before any Apply or focus loss commits it. The product bounds the interval to
                 # every minute through every hour: 5 commits as 60, 3600 keeps, 0 stays off.
                 "set_text TextHostRecAutosaveInterval 5\nwait_ms 500\n"
                 "assert_label TextHostRecAutosaveInterval 60\n"
                 "assert_label LabelHostRecLastSave Checkpoint every 60 sim seconds - none saved yet\n"
                 "set_text TextHostRecAutosaveInterval 3600\nwait_ms 500\n"
                 "assert_label TextHostRecAutosaveInterval 3600\n"
                 "assert_label LabelHostRecLastSave Checkpoint every 3600 sim seconds - none saved yet\n"
                 "set_text TextHostRecAutosaveInterval 0\nwait_ms 500\n"
                 "assert_label LabelHostRecLastSave No autosaves while this is off\n"
                 "assert_label LabelHostRecAutosaveHint Every 60 s to 60 min, or off (default)\n"
                 # A typed 0 is off at Apply too: the service accepts off instead of refusing an enabled zero.
                 # Apply publishes an open-seat lobby too; read the committed revision's
                 # confirmation rather than accepting a pending draft or stale edit hint.
                 "activate ButtonHostOptApply\nwait_ms 500\n"
                 "assert_label_absent LabelHostOptStatus requires a nonzero interval\n"
                 "assert_label LabelHostOptStatus Applied: this lobby was republished.\n"
                 "assert_checked CheckHostRecAutosave 0\n"
                 "assert_label LabelHostRecLastSave No autosaves while this is off\n"
                 # The applied draft keeps the picked window.
                 "assert_label ComboHostRecReturnWindow 10 minutes\n"
                 # The host's own picks stand through the Apply that republishes the lobby.
                 "assert_label ComboHostRecJoinHistory 10 minutes\n"
                 "assert_label ComboHostRecJoinLag 1 minute\n"
                 "setcheck CheckHostRecAutosave 0\nwait_ms 500\nassert_checked CheckHostRecAutosave 0\n"
                 "assert_enabled TextHostRecAutosaveInterval 0\n")
        text += checks("TextHostRecAutosaveInterval", "CollectionBoxHostPageRecovery")
        # H25: a lobby is not a live match, so the button is off and the hint says why.
        text += checks("ButtonHostRecRepairNow", "CollectionBoxHostPageRecovery")
        text += checks("LabelHostRecRepairHint", "CollectionBoxHostPageRecovery")
        text += ("assert_enabled ButtonHostRecRepairNow 0\n"
                 "assert_label LabelHostRecRepairHint Repair needs a live match session\n")
        text += "assert_no_overlap_within CollectionBoxHostPageRecovery\n"
        text += "dump_host_options\n"
        # H29-H31 Files and status.
        text += "activate TabHostPageFiles\nwait 3\nassert_visible CollectionBoxHostPageFiles 1\n"
        text += "assert_label LabelHostOptionsTitle A D V A N C E D\n"
        text += checks("ButtonHostFilesSaveDiag", "CollectionBoxHostPageFiles")
        text += checks("ComboHostFilesWidget", "CollectionBoxHostPageFiles")
        text += "assert_no_overlap_within CollectionBoxHostPageFiles\n"
        text += "dump_host_options\n"
        # H32-H35 Session.
        text += "activate TabHostPageSession\nwait 3\nassert_visible CollectionBoxHostPageSession 1\n"
        text += "assert_label LabelHostOptionsTitle A D V A N C E D\n"
        text += checks("LabelHostSessHosting", "CollectionBoxHostPageSession")
        text += checks("ComboHostSessIdle", "CollectionBoxHostPageSession")
        text += ("combo_select ComboHostSessIdle 5 minutes\nwait_ms 500\n"
                 "assert_label ComboHostSessIdle 5 minutes\n"
                 "combo_select ComboHostSessIdle 10 minutes (default)\nwait_ms 500\n"
                 "assert_label ComboHostSessIdle 10 minutes\n")
        text += checks("LabelHostSessIdleState", "CollectionBoxHostPageSession")
        text += checks("LabelHostSessBanned", "CollectionBoxHostPageSession")
        text += checks("ButtonHostSessBanned", "CollectionBoxHostPageSession")
        text += checks("ButtonHostSessEnd", "CollectionBoxHostPageSession")
        # H10's count is the ban store's own rows: none yet, so the session row reads zero.
        text += "assert_label LabelHostSessBanned 0 banned this session\n"
        text += "assert_no_overlap_within CollectionBoxHostPageSession\n"
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
                "assert_visible TextMultiplayerName 1\nfocus TextMultiplayerName\nassert_focus TextMultiplayerName\n"
                "settext TextMultiplayerName abc\nkey End down\nkey End up\nkey Backspace down\n"
                "key Backspace up\nassert_label TextMultiplayerName ab\ndump_player_options\n"
                "focus ButtonMultiplayerHostGame\nkey KP1 down\nkey KP1 up\nwait 3\n"
                "assert_substate HostSetup\nactivate ButtonHostBack\nwait 4\n"
                "focus ButtonMultiplayerHostGame\n"
                "pad south down\npad south up\nwait 3\nassert_substate HostSetup\n"
                "dump_host_options\nexit\n")
    elif case in ("live", "disabled", "scope-off", "input-parity"):
        text = ("wait 12\nassert_screen Pause\nassert_visible ButtonSettings 1\n" if case == "live"
                else RESET_INPUT + host_lobby(port) if case == "disabled"
                else RESET_INPUT + LANDING if case == "input-parity" else LANDING)
        text += "assert_visible root 1\n"
        if case in ("scope-off", "input-parity"):
            text += "focus TextMultiplayerName\nassert_focus TextMultiplayerName\n"
        text += f"wait_file {probe_root(root, 'host') / 'done.json'} 90\nexit\n"
        steps = (([{"op": "wait", "sim_at_least": 150},
                  # The host's own sim count says nothing about the client's start: the first checks
                  # wait until the client's round has committed frames of its own.
                  {"op": "wait_file", "path": str(probe_root(root, "client") / "client_frame.json")},
                  # ENGINE 195: a band pushed while the panel is closed paints at the bottom of the
                  # game screen; opening the panel moves it, and `single` fails if the old band's
                  # pixels stay behind on the GUI layer. The watch arms before the move so a ghost
                  # that only lives for the frames between the move and the next wipe still counts.
                  menu_step("push_toast info band-before-panel"), {"op": "wait", "renders": 3},
                  menu_step("assert_toast_band"),
                  menu_step("ghost_watch start"),
                  {"op": "key_down", "key": "F6"}, {"op": "key_up", "key": "F6"},
                  {"op": "wait", "panel_open": True}, {"op": "wait", "renders": 5},
                  menu_step("ghost_watch assert"),
                  menu_step("assert_toast_band single"),
                  # ENGINE 210: the corner roster box wraps at word boundaries only, and its width
                  # rule grows the panel to the longest word instead of letting it hang over. The
                  # status box only wraps in its tall layout; the strip path is one FitLine'd line.
                  menu_step("assert_word_wrap probe Seats [F6] Input delay: 15 (auto, re-sized live) PeerExtremelyLongDisplayNameForWrapChecking0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 sits row"),
                  menu_step("assert_roster_fits " + WIDE_ROSTER_NAME),
                  menu_step("status_line " + WIDE_STATUS_TOKEN), {"op": "wait", "renders": 3},
                  menu_step("assert_word_wrap status"),
                  menu_step("status_line"), {"op": "wait", "renders": 3}] +
                  [{"op": "key_down", "key": "F6"}, {"op": "key_up", "key": "F6"},
                  {"op": "wait", "panel_open": False},
                  {"op": "key_down", "key": "F6"}, {"op": "key_up", "key": "F6"},
                  {"op": "wait", "panel_open": True},
                  menu_step("push_toast info toast-one"), menu_step("push_toast info toast-two"),
                  menu_step("push_toast info toast-three"),
                  menu_step("assert_inside_screen NetworkSeats"),
                  menu_step("dump_refresh_count"), {"op": "wait", "renders": 60},
                  menu_step("dump_refresh_count"),
                  {"op": "key_down", "key": "F6"}, {"op": "key_up", "key": "F6"},
                  {"op": "key_down", "key": "Escape"},
                  {"op": "key_up", "key": "Escape"}, {"op": "wait", "screen": "Pause"}]) if case == "live"
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
            steps += [menu_step("key Return down"), menu_step("dump_enter_state"), menu_step("key Return up"),
                      menu_step("key KPEnter down"), menu_step("dump_enter_state"), menu_step("key KPEnter up")]
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
                steps += [menu_step("assert_visible ComboHostPlayers 1"), menu_step("dump_host_options"),
                          menu_step("post_command ButtonHostBack"), menu_step("assert_visible ButtonMultiplayerHostGame 1")]
                steps += [menu_step("focus TextMultiplayerName"), menu_step("assert_focus TextMultiplayerName")]
            steps += [menu_step("post_command ButtonMultiplayerHostGame"), menu_step("assert_visible ComboHostActivity 1"),
                      menu_step("focus ComboHostActivity"),
                      menu_step("key Return down"), menu_step("dump_enter_state"), menu_step("key Return up"),
                      menu_step("key KPEnter down"), menu_step("dump_enter_state"), menu_step("key KPEnter up"),
                      menu_step("post_command ButtonHostBack")]
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
        probe = {"schema": 1, "timeout_ms": 150000, "steps": steps}
    else:
        raise ValueError(case)
    texts = {"host": text}
    if case == "live":
        texts["client"] = f"wait_file {probe_root(root, 'host') / 'done.json'} 90\nexit\n"
        probes = {"host": probe,
                  # The client signals once its own round is running and committing frames, so the
                  # host's first checks measure a live match on any machine load.
                  "client": {"schema": 1, "timeout_ms": 150000, "steps": [
                      {"op": "wait", "service": "Running", "screen": "Gameplay", "sim_at_least": 30},
                      {"op": "signal", "name": "client_frame"}, {"op": "finish"}]}}
        return texts, probes
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
            # The mask colour is never drawn: where it shows, a panel draws without its skin.
            masked = next((count for count, colour in source.convert("RGB").getcolors(1 << 24) if colour == (255, 0, 255)), 0)
            assert not masked, f"{masked} mask-colour pixels on screen: {png}"
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
               ("Seats", "Rules", "Connection", "Timing", "Recovery", "Files", "Session")), measured
    return measured


def timing_options_geometry(images):
    measured = []
    # Rows sit where the Timing page draws them.
    expected = {'LabelHostNetSlowBound': (8, 44, 104, 18), 'TextHostNetSlowBound': (120, 44, 44, 18),
                'LabelHostNetSlowBoundHint': (172, 44, 100, 18), 'LabelHostNetSlowPolicy': (8, 64, 172, 18),
                'ComboHostNetSlowPolicy': (184, 64, 300, 18)}
    for capture in images:
        rows = {row['name']: row for row in capture['controls']}
        if 'CollectionBoxHostPageTiming' not in rows or not rows['CollectionBoxHostPageTiming'].get('visible', True):
            continue
        page = rows['CollectionBoxHostPageTiming']['rect']
        for name, rectangle in expected.items():
            row = rows[name]
            actual = (row['rect'][0] - page[0], row['rect'][1] - page[1], *row['rect'][2:])
            assert actual == rectangle and row['text_fits'], (name, actual, rectangle, row)
        combo = rows['ComboHostNetSlowPolicy']['rect']
        assert combo[1] + combo[3] + 40 <= page[1] + page[3], combo
        measured.append({'page': page, 'bound': rows['TextHostNetSlowBound']['rect'], 'policy': combo})
    assert measured, 'timing option rows were never read back'
    return measured


def spread_menu_scripts(case, texts, port, root):
    """Keep each page oracle and add a real remote lobby participant."""
    if case not in ("net-chat", "lobby-name"):
        return texts
    hosting = probe_root(root, "host") / "hosting.json"
    client_done = root / "client-done.mark"
    host = texts["host"]
    if not host.endswith("exit\n"):
        raise ValueError("paired page script must retain its final exit")
    host = host[:-len("exit\n")]
    if case == "net-chat":
        host += (LANDING + "settext TextMultiplayerName MenuHost\n"
                 "activate ButtonMultiplayerHostGame\nwait 5\n"
                 f"setup_host_port {port}\ncombo_select ComboHostPlayers 2\n"
                 "activate ButtonMultiplayerCreate\n")
    else:
        host = host.replace("activate ButtonMultiplayerCreate\n", "activate ButtonMultiplayerCreate\nwait_connected 2 60\n", 1)
    name = "MenuHost" if case == "net-chat" else "Recon7"
    if case == "net-chat":
        host += "wait_connected 2 60\n"
    host += "wait_label LabelLobbyPlayer1 Joiner\n"
    client = (LANDING + "settext TextMultiplayerName Joiner\n"
              f"wait_file {hosting} 60\nactivate ButtonMultiplayerJoinGame\nwait 5\n"
              "activate ButtonJoinByAddress\nwait 4\n"
              f"settext TextJoinAddress 127.0.0.1\nsettext TextJoinPort {port}\n"
              "activate ButtonJoinAddressGo\nwait_connected 2 60\n"
              f"wait_label LabelLobbyPlayer0 {name}\n")
    if case == "net-chat":
        host += "chat all hello-from-host\nwait_label LabelLobbyChatAny hello-from-client\nchat all host-received-client\n"
        client += "wait_label LabelLobbyChatAny hello-from-host\nchat all hello-from-client\nwait_label LabelLobbyChatAny host-received-client\n"
    else:
        client += f"wait_file {probe_root(root, 'host') / 'host-read.json'} 60\n"
    host += f"wait_file {client_done} 60\nexit\n"
    client += "exit\n"
    probes = {"host": {"schema": 1, "timeout_ms": 90000, "steps": [
        {"op": "wait", "screen": "MultiplayerScreen", "control": "LabelLobbyPlayer0", "text_contains": name,
         "equals": {"visible": True}, "scope": "menu"},
        {"op": "signal", "name": "hosting", "scope": "menu"},
        # The publishing probe shares the input scope with the menu script. Its cleanup
        # belongs after the client's exchange, so that script can still press Enter.
        {"op": "wait_file", "path": str(client_done), "scope": "menu"}, {"op": "finish"}]}}
    if case == "lobby-name":
        probes["host"]["steps"][-2:-2] = [
            {"op": "wait_file", "path": str(root / "host/runtime/ScreenShots/dump_host_options_4.json"), "scope": "menu"},
            {"op": "signal", "name": "host-read", "scope": "menu"}]
    return {"host": host, "client": client}, probes


@managed_case
def run_case(options, case, root, failing=None):
    root.mkdir(parents=True, exist_ok=False)
    texts, probes = scripts(case, options.port, root, options.size)
    smoke = getattr(options, "single_box_smoke", False)
    network_page_pair = case in ("net-chat", "lobby-name") and (smoke or (spread and spread.enabled(options))) and not failing
    if network_page_pair:
        texts, probes = spread_menu_scripts(case, texts, options.port, root)
    if failing:
        prelude, setup, assertion = failing
        texts, probes = {"host": prelude + setup + assertion + "\nexit\n"}, {}
    inputs = root / "input.txt"
    inputs.write_text(INPUT_SCRIPT, encoding="utf-8")
    paired = case in PAIRED_CASES or network_page_pair
    # A menu-driven pair joins through the real UI, so it carries no service-e2e flags.
    menu_driven = network_page_pair or case in ("net-activity", "local-end-match", "net-host-left", "net-host-left-early", "lobby-ready-all", "lobby-countdown",
                           "lobby-last-ready", "lobby-ready-back", "lobby-seat-missing", "sweep-advanced-client", *THIRD_PASS_CASES)
    seeded = {} if failing else seeds(case)
    runs, records, argv, images = {}, {}, {}, []
    executor = None
    result = {"pass": False, "case": case, "scripts": {}, "records": records, "probes": {}, "seeds": seeded}
    spread_requested = case in SPREAD_CASES and not smoke and spread and spread.enabled(options)
    result.update(topology="spread" if spread_requested else "single-box" if paired else "single-peer", proof=False)
    if smoke:
        result.update(execution_scope="LAN smoke", peer_boxes={who: os.environ.get("CC_RUNNER_BOX_NAME", os.environ.get("COMPUTERNAME", "local")) for who in texts})
    picture_watches = not BASE_WORDS and case not in OLD_SKINS
    result["picture_watch_scope"] = "all current controls" if picture_watches else "compatibility notices" if case in OLD_SKINS else "base controls"
    try:
        factory = make_run
        if case in SPREAD_CASES and not smoke:
            if not spread or not spread.enabled(options):
                raise RuntimeError(("paired" if paired else "selected") + " menu readback requires the shared spread executor")
            width, height, multiplier = size_parts(options.size)
            reviewed = "client" if case in ("host-draft-roundtrip", "sweep-advanced-client") else "host"
            peers = [spread.Peer(who, share_ok=False, os="windows" if who == reviewed or case == "net-host-left-early" else "any",
                                 size=(int(width * multiplier), int(height * multiplier)), reviewed=who == reviewed,
                                 held=(case == "net-host-left-early" and who == "client")) for who in texts]
            parameters = {"lane": "menus"}
            if case == "host-draft-roundtrip" or network_page_pair:
                # This case deliberately reads back Unlisted with traversal and relay Off.
                parameters["network"] = "direct"
            executor = spread.prepare_case(options.repo, root, peers, spread.Match(options.port, parameters=parameters))
            factory = executor.make_run
        # A pair runs host and client; a case may seat a second joiner beside them.
        for who in (tuple(texts) if paired else ("host",)):
            script = root / f"{who}-menu.txt"
            script.write_text(texts[who], encoding="utf-8")
            result["scripts"][str(script)] = sha(script)
            args = ["-menu-script", str(script)]
            if case in ("net-host-left", "net-host-left-early"):
                args += ["-input-script", str(inputs)]
            if case in ("local-end-match", "net-host-left"):
                # The post-match lobby's status surfaces report into this run's events.jsonl.
                video = root / f"{who}-video"
                video.mkdir()
                args += ["-record-video", str(video)]
            if case == "landing":
                # The flag takes the same over-cap name the box gets below: one console refusal.
                args += ["-net-player-name", "F" * (DISPLAY_NAME_MAX_BYTES + 1)]
            if paired and not menu_driven:
                # A repair round is long enough for a seat held at its start to finish its rejoin before the repair runs.
                round_ticks = str(paired_round_ticks(case))
                args += ["-net-match-service-e2e", "-net-port", str(options.port), "-net-match-peers", "2",
                         "-net-match-ticks", round_ticks, "-net-match-input-delay", "3", "-net-autosave-seconds", "0",
                         "-input-script", str(inputs), "-net-match-report", str(root / f"{who}-match.json")]
                args += ["-net-host"] if who == "host" else ["-net-join", "127.0.0.1"]
                if case == "repair":
                    args += ["-net-match-e2e-resync"]
            env = {"CCCP_HEADLESS": "1"}
            if picture_watches:
                # The picture rules every screen is held to, on every frame drawn, at every size: a caption fits its control and
                # its panel and the screen, and no caption draws over or under another control of its panel.
                watches = root / "picture-watches.txt"
                watches.write_text(PICTURE_WATCHES, encoding="utf-8")
                env["CCCP_TEST_SCREEN_WATCHES"] = str(watches)
            if who in probes:
                directory = probe_root(root, who)
                directory.mkdir()
                path = directory / "probe.json"
                path.write_text(json.dumps(probes[who], indent=2) + "\n", encoding="utf-8")
                result["scripts"][str(path)] = sha(path)
                env["CC_TEST_NET_UI_SCRIPT"] = str(path)
            if case == "host-words-nocrypto":
                env["CCCP_TEST_FAIL_CLOSED_CRYPTO"] = "1"
            if case == "sweep-fault":
                env["CCCP_TEST_MENU_FAULT"] = "hide:ButtonMultiplayerJoinGame,disable:ButtonMultiplayerReplays"
            if case.startswith("sweep-") and getattr(options, "harvest_declared", False):
                # A draft run: the sweep writes what each screen shows, for a person to declare from; it proves nothing.
                harvest = root / f"{who}-declared"
                harvest.mkdir()
                env["CCCP_TEST_SWEEP_HARVEST"] = str(harvest)
            elif case.startswith("sweep-"):
                # Each screen's controls are declared per state under tools/menu_declared/<case>; the fault case breaks the landing's.
                env["CCCP_TEST_SWEEP_DECLARED"] = str(options.repo / "tools" / "menu_declared" / ("sweep-landing" if case == "sweep-fault" else case))
            argv[who] = args
            if case in OLD_SKINS:
                runs[who] = factory(options.repo, args, root / who, 180, env=env, runtime=stage_old_skin_runtime(options.repo, root / f"{who}-staged", OLD_SKINS[case]))
            else:
                runs[who] = factory(options.repo, args, root / who, 180, env=env)
            if case == "host-long-names":
                fixture = Path(options.repo) / "tools/fixtures" / LONG_MODULE
                for item in fixture.rglob("*"):
                    if item.is_file():
                        target = runs[who].cwd / "Mods" / LONG_MODULE / item.relative_to(fixture)
                        target.parent.mkdir(parents=True, exist_ok=True)
                        target.write_bytes(item.read_bytes())
            if case in OLD_SKINS:
                # A staged runtime's record starts with no settings overrides; the size is the first.
                record_path = runs[who].out / "runtime.json"
                record = json.loads(record_path.read_text(encoding="utf-8"))
                record.setdefault("settings_overrides", {})
                record_path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
            set_visual_resolution(runs[who], *size_parts(options.size)[:2])
            set_window_multiplier(runs[who], size_parts(options.size)[2])
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
                run = runs[who].start()
                if case in OLD_SKINS:
                    # A transition has no menu controls too; Escape waits for the round's first tick.
                    wait_for_log(run, "[e2e] rules tick=1", 120)
                    (probe_root(root, who) / "started.mark").write_text("first gameplay tick\n", encoding="utf-8")
                records[who] = run.finish()
                if network_page_pair and who == "client":
                    spread.atomic_bytes(root / "client-done.mark", b"complete\n")
            except Exception as error:
                records[who] = {"error": repr(error)}

        threads = [threading.Thread(target=drive, args=(who,)) for who in runs]
        for index, thread in enumerate(threads):
            thread.start()
            if menu_driven and index == 0:
                # The joining peer's menu must find the host's lobby already listening.
                threading.Event().wait(2.0)
        if case == "net-host-left-early":
            # The client signals Running at its launch transition; suspending it there freezes the
            # start mid-flight so the departure can be staged while nothing has committed. The host
            # rules the absent seat held ("AI in control") before it dies, so the resumed client
            # wakes into a held rejoin against a dead session - the resync path, not a migration.
            while runs["client"].process is None or runs["host"].process is None:
                time.sleep(0.05)
            rtl = probe_root(root, "client") / "client_rtl.json"
            deadline = time.monotonic() + 90
            while not rtl.exists():
                if time.monotonic() >= deadline:
                    raise RuntimeError("the client never committed to its launch")
                if runs["client"].poll() is not None:
                    raise RuntimeError("the client ended before reaching its launch window")
                time.sleep(0.05)
            suspend_run(runs["client"])
            try:
                wait_for_log(runs["host"], "AI in control", 45)
                runs["host"].terminate()
            finally:
                resume_run(runs["client"])
            result["host_departure"] = "terminated while the held client was suspended mid-launch"
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
                provenance = {"source_revision": options.revision,
                    "executable": str(engine_executable(options.repo)), "exe_sha256": options.exe_sha,
                    "os": os.name, "configuration": "Final", "argv": record.get("argv", argv[who]),
                    "peer": who, "case": case, "logical_size": options.size}
                if executor:
                    box, claim = executor.members[who][:2]
                    provenance.update(box=box["name"], os=box["os"], executable=claim["exe"],
                                      exe_sha256=claim["exe_sha256"], source_revision=claim["head"])
                images += captures(run.cwd, provenance)
                if case == "net-activity" and who == "host":
                    host_setup = [image for image in images if image["peer"] == "host"
                                  and any(c["name"] == "ComboHostActivity" for c in image["controls"])]
                    if host_setup:
                        picker = next(c for c in host_setup[0]["controls"] if c["name"] == "ComboHostActivity")
                        assert_combo_matches_loaded_activities(picker, host_setup[0])
                # The early variant's host is the departure itself: terminated mid-launch, 137.
                expected_exit = 137 if (case, who) == ("net-host-left-early", "host") else 0
                assert record.get("exit_code") == expected_exit, record
                assert "[menu-script] FAILED:" not in logs[who], logs[who][-3000:]
                # A script that was meant to run to its end did: a run that never read it is no pass. A service-e2e pair
                # starts its round without the menus (its probes are its checks), and a script whose last wait is for its
                # round to run leaves the menus there.
                if expected_exit == 0 and not (paired and not menu_driven):
                    _, waits_for_round, tail = texts[who].rpartition("wait_state Running")
                    rest = [line.strip() for line in tail.splitlines()[1:] if line.strip()]
                    in_round = bool(waits_for_round) and rest in ([], ["exit"]) and "state:Running -> OK" in logs[who]
                    assert "[menu-script] complete" in logs[who] or in_round, logs[who][-3000:]
                if picture_watches:
                    armed = re.findall(r"\[text-watch\] armed .*\"(picture-[\w-]+)\"", logs[who])
                    violations = [line for line in logs[who].splitlines() if "[text-watch] violation picture-" in line]
                    result.setdefault("pictures", {})[who] = {"armed": armed, "violations": violations[:20]}
                    assert sorted(set(armed)) == ["picture-layout", "picture-overlap"], (who, armed)
                    assert not violations, (who, violations[:20])
        if not failing:
            if case == "lobby":
                # The picks drive this host's history policy, in frames of the round's tick.
                policy = JOIN_HISTORY_POLICY.search(logs["host"])
                # Ten minutes of a 60 Hz round plus the frames a hold reaches back and one, a frame more when the tick's length rounds up.
                low = 36000 + max_future_frame_skew() + 1
                assert policy and low <= int(policy[1]) <= low + 1, [line for line in logs["host"].splitlines() if "host options" in line]
                result["join_history_policy"] = policy[0]
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
            refresh = [json.loads(line.split("[refresh-count] ", 1)[1]) for line in logs["host"].splitlines()
                       if "[refresh-count] " in line]
            assert len(refresh) >= 2, refresh
            delta = refresh[-1]["refresh_count"] - refresh[0]["refresh_count"]
            changes = refresh[-1]["refresh_changes"] - refresh[0]["refresh_changes"]
            result["refresh"] = {"before": refresh[0], "after": refresh[-1], "delta": delta, "changes": changes,
                                 "frames": 60}
            assert delta < 60, result["refresh"]
        for who in probes:
            observation = json.loads((probe_root(root, who) / "net-ui-result.json").read_text(encoding="utf-8"))
            result["probes"][who] = observation
            assert observation["pass"] and observation["complete"], (who, observation)
            if case == "live" and who == "host":
                # The roster-fit and status-wrap surfaces are read off the host's probe; the
                # client's probe is only the committed-frame rendezvous the checks wait on.
                fits = roster_fit_observations(observation)
                assert len(fits) == 1, fits
                row = fits[0]
                assert row["widest_token"] <= row["column"] and row["widest_line"] <= row["column"], row
                assert row["inside"] is True and len(row["rect"]) == 4, row
                width, height = size_parts(options.size)[:2]
                assert row["rect"][0] + row["rect"][2] <= width and row["rect"][1] + row["rect"][3] <= height, row
                if options.size == "640x360":
                    assert row["ellipsis"] is True and WIDE_ROSTER_NAME not in row["drawn"], row
                notes = status_wrap_notes(observation)
                if options.size == "640x360":
                    assert notes == ["status: no wrap surface"], notes
                else:
                    assert len(notes) == 1, notes
                    status = json.loads(notes[0])
                    assert status["widest_line"] <= status["text_width"] and status["split_token"] == "", status
                    drawn = "\n".join(status["rows"])
                    if WIDE_STATUS_TOKEN not in drawn:
                        assert "..." in drawn, status
                    if options.size == "960x540":
                        assert WIDE_STATUS_TOKEN not in drawn and "..." in drawn, status
        if case in ("host-stun", "host-stun-empty"):
            pages = [{c["name"]: c for c in capture["controls"]} for capture in images
                     if any(c["name"] == "ComboHostNetIce" for c in capture["controls"])]
            assert [page["ComboHostNetIce"]["text"] for page in pages] == [*NAT_STATES, *NAT_STATES], pages
            for page in pages:
                label, combo, hint = (page[name] for name in ("LabelHostNetIce", "ComboHostNetIce", "LabelHostNetIceHint"))
                first = NAT_STUN_EMPTY if (case, combo["text"]) == ("host-stun-empty", "On (default)") else NAT_HINTS[combo["text"]]
                assert label["text"] == NAT_LABEL and hint["text"].startswith(first + "\n"), (label, hint)
                assert combo_item_names(combo) == list(NAT_STATES), combo
                assert all(row["text_fits"] for row in (label, combo, hint)), (label, combo, hint)
                assert label["rect"][1] == combo["rect"][1], (label, combo)
                assert label["rect"][0] + label["rect"][2] + 8 <= combo["rect"][0], (label, combo)
                page_rect = page["CollectionBoxHostPageConnection"]["rect"]
                assert page_rect[1] + page_rect[3] + 4 <= page["ButtonHostOptBack"]["rect"][1], page
            for page in (pages[0], pages[2]):
                # An empty STUN list leaves direct connections to this network, and the hint says so; the shipped list says nothing of it.
                empty = "The STUN server list is empty" in page["LabelHostNetIceHint"]["text"]
                assert empty == (case == "host-stun-empty"), page
                assert "online game list service" in page["LabelHostNetIceHint"]["text"], page
            expected_saved = dict.fromkeys(NAT_KEYS, "")
            expected_saved.update({"NetworkIceEnable": "0", "NetworkStunServers": "" if case == "host-stun-empty" else STUN_DEFAULT})
            result["saved"] = read_settings(runs["host"].cwd / "Userdata/Settings.ini", set(NAT_KEYS))
            assert result["saved"] == expected_saved, result["saved"]
            result["nat_rows"] = [{name: page[name]["rect"] for name in ("LabelHostNetIce", "ComboHostNetIce", "LabelHostNetIceHint")}
                                  for page in pages]
        if case == "host-relay":
            pages = [{row["name"]: row for row in capture["controls"]} for capture in images]
            assert [page["ComboHostNetRelay"]["text"] for page in pages] == [RELAY_STATES[1], RELAY_STATES[0], RELAY_STATES[2], RELAY_STATES[1]], pages
            assert all(combo_item_names(page["ComboHostNetRelay"]) == list(RELAY_STATES) for page in pages)
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
        if case == "save-hotkey":
            # F5 is the host's save for every peer: the press names one save, both writers take it, the toast says so.
            report = json.loads((root / "host-match.json").read_text(encoding="utf-8"))
            texts = [toast["text"] for toast in report["ui"]["toasts"] if toast["kind"] == "match_save"]
            assert texts[:1] == ["Saving..."], texts
            named = re.findall(r"^\[autosave\] manual save named tick=(\d+)", logs["host"], re.MULTILINE)
            assert len(named) == 1, named
            for who in ("host", "client"):
                taken = re.findall(r"^\[autosave\] manual save taken tick=(\d+)", logs[who], re.MULTILINE)
                assert taken == named, (who, taken, named)
            result["save_hotkey"] = {"tick": int(named[0]), "toasts": texts}
        if case == "pause-save":
            # The row is the host's to press and a client's to read; both peers write the one capture it names, marked as
            # the host's save, and the host hears back from both writers.
            for who in ("host", "client"):
                pauses = [capture for capture in images if capture["peer"] == who and capture["screen"] == "Pause"]
                assert len(pauses) == 2, (who, [capture["screen"] for capture in images if capture["peer"] == who])
                row = next(control for control in pauses[0]["controls"] if control["name"] == "ButtonSaveMatch")
                assert row["text"].lower() == "save match" and row["enabled"] is (who == "host"), (who, row)
                hint = next(control for control in pauses[-1]["controls"] if control["name"] == "LabelSaveMatchHint")
                assert ("Last saved at" if who == "host" else "The host saves the match - last saved at") in hint["text"], (who, hint)
            named = re.findall(r"^\[autosave\] manual save named tick=(\d+)", logs["host"], re.MULTILINE)
            assert len(named) == 1, named
            for who in ("host", "client"):
                taken = re.findall(r"^\[autosave\] manual save taken tick=(\d+)", logs[who], re.MULTILINE)
                assert taken == named, (who, taken, named)
                manifests = sorted((runs[who].cwd / "Autosaves").glob(f"*-{named[0]}.ccmanifest"))
                assert len(manifests) == 1 and "SavedBy = host" in manifests[0].read_text(encoding="utf-8").splitlines(), (who, manifests)
            report = json.loads((root / "host-match.json").read_text(encoding="utf-8"))
            texts = [toast["text"] for toast in report["ui"]["toasts"] if toast["kind"] == "match_save"]
            assert texts[:1] == ["Saving..."] and "Match saved (2 peers)" in texts, texts
            result["pause_save"] = {"tick": int(named[0]), "toasts": texts}
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
            # An acknowledged leave hands the seat to the AI while the host keeps playing.
            reports = {who: json.loads((root / f"{who}-match.json").read_text(encoding="utf-8"))
                       for who in ("host", "client")}
            assert reports["client"]["service"]["status"] == "Match left", reports["client"]["service"]["status"]
            leave = reports["client"]["service"]["reconnect"]
            assert leave["client_leave_acks"] == 1 and leave["client_unacknowledged_leaves"] == 0, leave
            # A player who leaves keeps the seat and its ticket, so Rejoin Match brings the player back while the match runs.
            assert leave["client_state"] == "Left" and leave["ticket_stored"] is True, leave
            assert "[net-reconnect] leave: Left (ticket kept)" in logs["client"], logs["client"][-2000:]
            announcements = re.findall(r"\[net-lockstep\] a leave becomes a hold for peer 2 at frame (\d+): Match left", logs["host"])
            assert len(announcements) == 1, announcements
            leave_frame = int(announcements[0])
            assert f"[net-match] hold peer=2 frame={leave_frame} AI in control" in logs["host"], leave_frame
            host = reports["host"]
            lockstep = host["service"]["runner"]["lockstep"]
            ticks = paired_round_ticks(case)
            assert lockstep["peer_leave_frames"] == {"2": leave_frame} and 0 < leave_frame < ticks, lockstep
            assert host["service"]["is_host"] is True and lockstep["host_peer_id"] == 1, host["service"]["is_host"]
            assert host["frames_planned"] == ticks and host["running_ticks"] >= ticks, host["running_ticks"]
            assert lockstep["completed_simulation_tick"] >= ticks, lockstep["completed_simulation_tick"]
            assert host["service"]["status"] == "e2e complete", host["service"]["status"]
            result["announced_leave"] = {"peer_id": 2, "frame": leave_frame, "leave_acks": leave["client_leave_acks"],
                "ticket_cleared": not leave["ticket_stored"], "completed_tick": lockstep["completed_simulation_tick"]}
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
            assert len(images) == 1, len(images)
            page = images[0]
            display = page["display"]
            rows = {control["name"]: control for control in page["controls"]}
            combo = rows.get("ComboPresetResolution")
            assert combo, sorted(rows)
            # The page shows the preset box on a display whose aspect the window matches, and the custom box
            # otherwise; a fullscreen window always takes the preset box.
            matched = abs(display["res_x"] / display["res_y"] - display["max_res_x"] / display["max_res_y"]) < 1e-9
            expected = bool(display["fullscreen"] or matched)
            result["display"] = {**display, "aspect_matches_display": matched, "preset_box_expected": expected}
            assert combo["visible"] == expected, (display, combo["visible"])
            shown = "CollectionPresetResolution" if expected else "CollectionCustomResolution"
            assert rows.get(shown, {}).get("visible"), (shown, sorted(name for name, row in rows.items() if row["visible"]))
            # The measurement this case exists for: the combo's own line fits its box minus the drop-down button.
            assert combo.get("text"), combo
            available = re.search(r"available=\[(\d+),\s*(\d+)\]", combo["text_measure"])
            assert available, combo["text_measure"]
            available = [int(available.group(1)), int(available.group(2))]
            result["combo_fit"] = [["ComboPresetResolution", combo["rect"], available]]
            assert available[0] <= combo["rect"][2] - COMBO_BUTTON, (combo["rect"], available)
            assert combo["text_fits"], combo
        if case == "network":
            # The page shows the saved name, the page's own edits are saved, and the lobby box starts from them.
            page = [capture for capture in images if capture["settings_page"] == "Network:Player"]
            assert len(page) == 2, [capture["settings_page"] for capture in images]
            rows = {control["name"]: control for control in page[0]["controls"]}
            assert set(NETWORK_ROWS) <= rows.keys(), sorted(rows)
            # The settings dialog's base box centres on the viewport at every size, not only at 640.
            base = rows["CollectionBoxSettingsBase"]["rect"]
            res_y = size_parts(options.size)[1]
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
            # The name is the first view's: the seeded one there, and the page's edit saved.
            basics = [capture for capture in images if capture["settings_page"] == "Network:Basics"]
            assert basics and next(c["text"] for c in basics[0]["controls"] if c["name"] == "TextNetworkDisplayName") == NETWORK_SEED["NetworkDisplayName"], basics[:1]
            result["saved"] = read_settings(runs["host"].cwd / "Userdata/Settings.ini", set(NETWORK_SAVED))
            assert result["saved"] == NETWORK_SAVED, result["saved"]
            # The page sits on the Misc page's grid: its first row at the top row, a 20px row pitch, and under the
            # automatic policy the hidden fixed row leaves no gap behind it.
            pitch = ("LabelNetworkDelayPolicy", "LabelNetworkFixedDelay", "LabelNetworkIdleWait", "LabelNetworkPathHorizon",
                     "CheckboxNetworkAutoRepair", "CheckboxNetworkToasts", "CheckboxNetworkDiagnostics")
            assert after[pitch[0]]["rect"][1] - after["CollectionBoxNetPagePlayer"]["rect"][1] == 12, (after[pitch[0]], after["CollectionBoxNetPagePlayer"])
            deltas = [after[b]["rect"][1] - after[a]["rect"][1] for a, b in zip(pitch, pitch[1:])]
            assert deltas == [20] * 6, deltas
            without_fixed = pitch[:1] + pitch[2:]
            closed = [rows[b]["rect"][1] - rows[a]["rect"][1] for a, b in zip(without_fixed, without_fixed[1:])]
            assert closed == [20] * 5, closed
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
            assert host_rows["ComboHostNetPolicy"]["text"].startswith("Fixed"), host_rows["ComboHostNetPolicy"]
            assert host_rows["TextHostNetMinDelay"]["text"] == NETWORK_SAVED["NetworkInputDelayFrames"], host_rows["TextHostNetMinDelay"]
            assert host_rows["TextHostNetMinDelay"]["enabled"] is True, host_rows["TextHostNetMinDelay"]
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
            expected = {"net-chat": ("CheckboxNetworkChatSound", "ComboNetworkChatScope", "ComboNetworkChatTextSize",
                                     "LabelNetworkChatKey", "TextNetworkChatKey",
                                     "ButtonNetMutedPlayers", "LabelNetMutedReason"),
                        "net-recovery": ("CheckboxNetworkAutoReconnect", "CheckboxNetworkOfferRejoin",
                                         "LabelNetLastHost", "LabelNetRecoveryRecord",
                                         "LabelNetRecoveryStatusTitle", "LabelNetRecoveryStatus",
                                         "ButtonNetRejoin", "ButtonNetCancelRecovery"),
                        "net-files": ("LabelNetAutosave", "LabelNetAutosaveInterval", "LabelNetAutosaveHost",
                                      "LabelNetAutosavesKeptTitle",
                                      "TextNetworkAutosavesKept", "LabelNetAutosavesKeptHint", "LabelNetAutosaveInfo",
                                      "ButtonNetOpenAutosaves", "ButtonNetCopyAutosavesPath",
                                      "TextNetworkDiagDir", "ButtonNetOpenDiagnostics",
                                      "ButtonNetCopyDiagPath", "ButtonNetSaveDiagnostics",
                                      "CheckboxNetworkRecordReplays"),
                        "net-internet": ("TextNetworkDirUrl", "LabelNetDirUrlHint", "TextNetworkDirPin",
                                         "LabelNetDirStatus", "LabelNetInternetReason"),
                        "misc-page": MISC_ROWS}[case]
            assert set(expected) <= rows.keys(), (case, sorted(rows))
            if case != "misc-page":
                # One value column across the four pages: every value control and every right-hand
                # control starts on it, and the pages' rows ride the Misc grid's 20px pitch.
                column = rows[f"CollectionBoxNetPage{sub_page.split(':')[1]}"]["rect"][0] + NETWORK_VALUE_COLUMN
                on_column = {
                    "net-chat": ("ComboNetworkChatScope", "ComboNetworkChatTextSize", "TextNetworkChatKey", "LabelNetMutedReason"),
                    "net-recovery": ("CheckboxNetworkOfferRejoin", "LabelNetLastHost",
                                     "LabelNetRecoveryRecord", "LabelNetRecoveryStatus",
                                     "ButtonNetRejoin"),
                    "net-files": ("LabelNetAutosave", "LabelNetAutosaveInterval", "TextNetworkAutosavesKept",
                                  "ButtonNetOpenAutosaves", "TextNetworkDiagDir",
                                  "ButtonNetOpenDiagnostics", "ButtonNetSaveDiagnostics"),
                    "net-internet": ("TextNetworkDirUrl", "LabelNetDirUrlHint", "LabelNetDirStatus")}[case]
                for name in on_column:
                    assert rows[name]["rect"][0] == column, (name, rows[name]["rect"], column)
                grid_rows = {
                    "net-chat": ("LabelNetworkChatScope", "CheckboxNetworkChatSound", "LabelNetworkChatTextSize",
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
                                     "LabelNetDirStatusTitle",
                                     "LabelNetInternetReason", "LabelNetInternetError")}[case]
                deltas = [rows[b]["rect"][1] - rows[a]["rect"][1] for a, b in zip(grid_rows, grid_rows[1:])]
                # The internet pin box's own row sits between its label and the status row; the chat
                # page's muted stub waits one row under its rows.
                expected_pitch = [20, 20, 40, 40, 20] if case == "net-internet" else \
                    [20] * 3 + [40] if case == "net-chat" else [20] * (len(grid_rows) - 1)
                assert deltas == expected_pitch, (case, deltas)
                if case == "net-files":
                    # The folders' action pairs stack in the same two columns, one row pair apart.
                    action_column = rows["CollectionBoxNetPageFiles"]["rect"][0] + NETWORK_ACTION_COLUMN
                    opens = [rows[name]["rect"] for name in ("ButtonNetOpenAutosaves", "ButtonNetOpenDiagnostics")]
                    copies = [rows[name]["rect"] for name in ("ButtonNetCopyAutosavesPath", "ButtonNetCopyDiagPath")]
                    assert all(rect[0] == column for rect in opens) and all(rect[0] == action_column for rect in copies), (opens, copies)
                    assert opens[0][2:] == opens[1][2:] and copies[0][2:] == copies[1][2:], (opens, copies)
                    assert opens[1][1] - opens[0][1] == copies[1][1] - copies[0][1] == 40, (opens, copies)
                    # One note covers both host-set rows: it spans them, so the page never says it twice.
                    note, first, second = (rows[name] for name in ("LabelNetAutosaveHost", "LabelNetAutosave", "LabelNetAutosaveInterval"))
                    assert note["text"] == "Both set by the host" and note["enabled"] is False, note
                    assert note["rect"][1] == first["rect"][1] and note["rect"][1] + note["rect"][3] == second["rect"][1] + second["rect"][3], (note, first, second)
                    assert "LabelNetAutosaveIntervalHost" not in rows, rows.get("LabelNetAutosaveIntervalHost")
                    widths = {rows[name]["rect"][2] for name in FILES_BUTTONS}
                    assert len(widths) == 1, {name: rows[name]["rect"][2] for name in FILES_BUTTONS}
            captioned = [control for control in images[0]["controls"] if control["text"]]
            assert captioned and all("text_fits" in control for control in captioned), "a caption carries no fit measurement"
            result["unfit"] = [control["name"] for control in captioned if control["text_fits"] is False]
            assert not result["unfit"], result["unfit"]
            disabled = {"net-chat": ("ButtonNetMutedPlayers",),
                        "net-recovery": ("ButtonNetRejoin", "ButtonNetCancelRecovery")}.get(case, ())
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
                assert rows["LabelNetAutosaveInterval"]["text"] == "60 s", rows["LabelNetAutosaveInterval"]
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
            assert prompt["text"] == "Your name", prompt
            # The over-cap name was refused on the command line, and the box kept a hand to its 24 characters.
            assert "-net-player-name over the 64-byte cap" in logs["host"], logs["host"][-2000:]
            typed = {c["name"]: c for c in images[-1]["controls"]}
            assert typed["TextMultiplayerName"]["text"] == "N" * 24, typed["TextMultiplayerName"]
        if case in ("lobby", "lobby-name"):
            # The Leave/Seats block is centred on the lobby panel the way Start Match is; doubled
            # centres avoid halves. The Players header starts on its rows' left edge and holds its line.
            lobby_shots = [image for image in images
                           if any(c["name"] == "MultiplayerLobbyPanel" for c in image["controls"])]
            assert lobby_shots, "no capture of the lobby panel"
            drawn = {c["name"]: c for c in lobby_shots[-1]["controls"]}
            if case == "lobby":
                result["host_options_geometry"] = host_options_geometry(images)
                result["timing_options_geometry"] = timing_options_geometry(images)
            leave, last, panel = (drawn[name] for name in ("ButtonMultiplayerLeave", "ButtonLobbyOptions", "MultiplayerLobbyPanel"))
            assert not drawn.get("ButtonMultiplayerModerate", {}).get("visible", False), drawn.get("ButtonMultiplayerModerate")
            assert leave["rect"][0] + last["rect"][0] + last["rect"][2] == panel["rect"][0] * 2 + panel["rect"][2], \
                (leave["rect"], last["rect"], panel["rect"])
            header = drawn["LabelLobbyPlayersHeader"]
            seat_rows = [control for control in lobby_shots[-1]["controls"]
                         if re.fullmatch(r"LabelLobbyPlayer\d", control["name"])]
            assert seat_rows and header["text_fits"] and all(
                row["rect"][0] == header["rect"][0] for row in seat_rows), (header, seat_rows)
            start = drawn["ButtonMultiplayerStart"]
            pair_span = last["rect"][0] + last["rect"][2] - leave["rect"][0]
            pair_gap = last["rect"][0] - leave["rect"][0] - leave["rect"][2]
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
            # Saving the name writes the product's own listing default beside it: public.
            result["saved"] = read_settings(runs["host"].cwd / "Userdata/Settings.ini", {"NetworkDisplayName", "NetworkHostGameListing"})
            assert result["saved"] == {"NetworkDisplayName": "Recon7", "NetworkHostGameListing": "Public"}, result["saved"]
            # The host's own row names it the host, in words; ping and delay live in the row's details.
            result["lobby_row"] = next(c["text"] for c in images[-1]["controls"] if c["name"] == "LabelLobbyPlayer0")
            assert "Host" in result["lobby_row"], result["lobby_row"]
            host_setup = {c["name"]: c for c in images[1]["controls"]}
            assert not any(host_setup.get(name, {}).get("visible", False) for name in ("TextHostInputDelay", "LabelHostInputDelayPolicy", "TextHostPort")), host_setup
            # The four setup rows share one value column.
            columns = {host_setup[name]["rect"][0] for name in ("ComboHostActivity", "ComboHostScene", "ComboHostMode", "ComboHostPlayers")}
            assert len(columns) == 1, columns
            # A disabled text box's frame is DimRect at 55% of an enabled TextBox frame.
            timing = {c["name"]: c for c in images[2]["controls"]}
            recovery = {c["name"]: c for c in images[3]["controls"]}
            delay_luma = frame_luma(images[3]["png"], recovery["TextHostRecAutosaveInterval"]["rect"])
            port_luma = frame_luma(images[2]["png"], timing["TextHostNetSlowBound"]["rect"])
            result["delay_frame_luma"] = delay_luma
            result["port_frame_luma"] = port_luma
            assert port_luma > 0, (delay_luma, port_luma)
            ratio = delay_luma / port_luma
            result["delay_frame_luma_ratio"] = ratio
            assert 0.45 <= ratio <= 0.65, (delay_luma, port_luma, ratio)
            # The multiplayer screen's panel centres vertically too; an odd height shifts one pixel,
            # and a panel taller than the viewport clamps to its top edge instead of centring.
            screen_rect = drawn["MultiplayerScreen"]["rect"]
            res_y = size_parts(options.size)[1]
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
            # The host's post-leave menu reads the adopted delay. Its announced leave with no other survivor is
            # the host ending the match: the client's round stops at the leave frame, no election runs and no
            # reconnect is kept or offered.
            lobby = [line for line in logs["host"].splitlines() if "[menu-script] dump_lobby" in line]
            assert lobby and 'input_delay="Input delay: 3 (fixed)"' in lobby[-1], lobby
            reports = {who: json.loads((root / f"{who}-match.json").read_text(encoding="utf-8"))
                       for who in ("host", "client")}
            host_service = reports["host"]["service"]
            assert host_service["status"] == "Match left", host_service["status"]
            host_lockstep = host_service["runner"]["lockstep"]
            departures = re.findall(r"\[net-match\] Host left the match at frame (\d+) \(Match left\)", logs["client"])
            assert len(departures) == 1, departures
            leave_frame = int(departures[0])
            for line in ("is now hosting", "host lost; collecting", "Host lost - arranging handover"):
                assert line not in logs["client"], (line, logs["client"][-4000:])
            client = reports["client"]
            service = client["service"]
            lockstep = service["runner"]["lockstep"]
            assert service["is_host"] is False and service["local_peer_id"] == 2, service["local_peer_id"]
            assert lockstep["host_peer_id"] == host_service["local_peer_id"] == 1, lockstep["host_peer_id"]
            assert lockstep["round_id"] == host_lockstep["round_id"], (lockstep["round_id"], host_lockstep["round_id"])
            assert lockstep["migration_generation"] == 0 and lockstep["migration_boundary"] == 0, lockstep
            assert lockstep["peer_leave_frames"] == {"1": leave_frame}, lockstep["peer_leave_frames"]
            assert 0 < lockstep["completed_simulation_tick"] < leave_frame, (lockstep["completed_simulation_tick"], leave_frame)
            assert client["frames_planned"] == 400 and client["running_ticks"] < 400, client["running_ticks"]
            assert service["state"] == "Completed", service["state"]
            reconnect = service["reconnect"]
            assert reconnect["ticket_stored"] is False and reconnect["ux_attempts"] == 0, reconnect
            result["host_departure"] = {"leave_frame": leave_frame, "round_id": lockstep["round_id"],
                "client_completed_tick": lockstep["completed_simulation_tick"], "client_status": service["status"]}
        if case in ("net-host-left", "net-host-left-early"):
            if case == "net-host-left-early":
                # A dead launch leaves no seat to reclaim, so the departure verdict is the
                # service's own error and status text in the client's lobby dump - with the
                # roster down to the local member.
                verdicts = re.findall(r'dump_lobby state=Failed members=1 .*?error="([^"]*)" status="([^"]*)"',
                                      logs["client"])
                assert verdicts and all(error == "The host left the match" == status
                                        for error, status in verdicts), verdicts
                result["host_departure_status"] = verdicts
            else:
                status = [control["text"] for capture in images if capture["peer"] == "client"
                          for control in capture["controls"] if control["name"] == "LabelMultiplayerLandingStatus"]
                assert status and all("The host left the match" in value and "rematch roster:" not in value for value in status), status
                result["host_departure_status"] = status
            if case == "net-host-left-early":
                # The seat the host held while the client was suspended is what routes the wake-up
                # through the held rejoin and the resync failure, not the migration a Running peer
                # would take on a mid-match departure.
                assert "AI in control" in logs["host"], logs["host"][-2000:]
                assert records["host"].get("injected_termination"), records["host"]
                assert "is now hosting" not in logs["client"], logs["client"][-4000:]
                early = [line for line in logs["client"].splitlines()
                         if "[net-match]" in line and "resync failed" in line]
                assert early, logs["client"][-4000:]
                result["client_startup_failure"] = early[-1]
                client_done = json.loads((probe_root(root, "client") / "client_done.json").read_text(encoding="utf-8"))
                result["client_sim_at_failure"] = client_done.get("sim_frame")
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
            assert picker[0]["text"] == "Skirmish Defense" and picker[0]["dropped"] is False, picker[0]
            assert picker[1]["text"] == "Persistent World" and picker[1]["dropped"] is True, picker[1]
            assert any(row["text"] == "Brain vs Brain" and row["dropped"] is False for row in picker), picker
            listed = allowed_host_activities(host_setup[0])
            picked = host_activity_label({"preset": preset, "module": module}, listed)
            assert any(row["text"] == picked and not row["dropped"] for row in picker), (picked, picker)
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
                if (activity.get("text") == "Brain vs Brain" and activity.get("dropped") is False
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
            assert all(row["text"] == "Brain vs Brain" or row["dropped"] for row in picker[2:]), picker
            result["picker_cycle"] = picker
            result["scene_cycle"] = scenes
            picked_scene = bvb_closed[-1]["text"]
            result["picked_scene"] = picked_scene
            key_scene = combo_name(picked_scene)
            assert scene == key_scene, (scene, key_scene, dumped)
            modes = [next(c for c in image["controls"] if c["name"] == "ComboHostMode")
                     for image in images if image["peer"] == "host"
                     and any(c["name"] == "ComboHostMode" for c in image["controls"])]
            assert any(row["text"] == "Players versus AI" for row in modes), modes
            result["mode_cycle"] = modes
            # The two header rows carry the friendly mode label, and both peers' panels are the
            # same rectangle for the same lobby state - no peer's own status text widens its panel.
            # The host's port-map row is the host's own line, and its height is the only one a panel may add.
            panels, port_map_rows, sentences = {}, {}, {}
            for who in ("host", "client"):
                matches = [image for image in images if image["peer"] == who
                           and any(c["name"] == "LabelLobbyMatchMode" for c in image["controls"])]
                assert len(matches) == 1, (who, [image["json"] for image in matches])
                shot = matches[0]
                assert [shot["activity_preset"], shot["activity_module"]] == [preset, module], shot["json"]
                assert shot.get("scene_name") == key_scene, (who, shot.get("scene_name"), key_scene)
                controls = {c["name"]: c for c in shot["controls"]}
                # The header names the module only to tell apart two loaded activities of one name, as Host a Game does.
                assert controls["LabelLobbyMatch"]["text"] == host_activity_label({"preset": preset, "module": module}, shot.get("activity_table", [])), (who, controls["LabelLobbyMatch"])
                assert controls["LabelLobbyMatchMode"]["text"] == key_scene + " - Players versus AI", (who, controls["LabelLobbyMatchMode"], key_scene)
                assert "Grasslands" not in controls["LabelLobbyMatchMode"]["text"], (who, controls["LabelLobbyMatchMode"])
                for name in ("LabelLobbyMatch", "LabelLobbyMatchMode", "LabelLobbyPlayersHeader"):
                    assert controls[name]["text_fits"] is True, (who, name, controls[name])
                panels[who] = controls["MultiplayerLobbyPanel"]["rect"]
                port_map_rows[who] = bool(controls.get("LabelLobbyPortMap", {}).get("visible"))
                sentences[who] = controls["LabelMultiplayerStatus"]["rect"][3]
            host_panel, client_panel = panels["host"], panels["client"]
            assert not port_map_rows["client"], port_map_rows
            # Each peer reads its own next-action sentence (the host's breaks onto a second line): the height the two
            # sentences differ by and the host's port-map row are the only height one panel may add over the other.
            own = sentences["host"] - sentences["client"]
            assert [host_panel[0], host_panel[2]] == [client_panel[0], client_panel[2]], panels
            assert own + (LOBBY_PORT_MAP_ROW if port_map_rows["host"] else 0) == host_panel[3] - client_panel[3], (panels, sentences)
            assert abs(2 * host_panel[1] + host_panel[3] - 2 * client_panel[1] - client_panel[3]) <= 1, panels
            result["lobby_panel_rects"] = panels
            result["lobby_port_map_rows"] = port_map_rows
            result["key_committed"] = {"preset": preset, "scene": key_scene, "combo": picked_scene}
        if case == "input":
            assert next(c["text"] for c in images[0]["controls"] if c["name"] == "TextMultiplayerName") == "ab"
        if case == "input-parity":
            assert len(images) == 4, len(images)
            projected = [[{key: control[key] for key in ("name", "rect", "text", "enabled", "visible")}
                          for control in capture["controls"]] for capture in images]
            assert all(value == projected[0] for value in projected[1:]), "activation routes produce different controls"
            host_log = logs.get("host") or next(iter(logs.values()))
            enters = [json.loads(line.split("[enter-state] ", 1)[1]) for line in host_log.splitlines()
                      if "[enter-state] " in line]
            result["enter_states"] = enters
            assert enters and all(row.get("name") == "Pushed" for row in enters), enters
        result["pass"] = True
    except Exception as error:
        result["error"] = str(error)
        # The check that failed, by its line in this driver: a message that is only the data it read names no check.
        result["failed_at"] = [f"{frame.lineno}: {frame.line}" for frame in traceback.extract_tb(error.__traceback__)
                               if frame.filename == __file__][-1:]
    finally:
        for run in runs.values():
            try:
                run.close()
            except Exception as error:
                result["pass"] = False
                result.setdefault("cleanup_errors", []).append(str(error))
        receipt = executor.result() if executor else None
        if not receipt and spread_requested and (root / "spread-result.json").is_file():
            receipt = json.loads((root / "spread-result.json").read_text(encoding="utf-8"))
        if receipt:
            result.update(topology="spread", peer_boxes=receipt["peer_boxes"], spread=receipt)
            result["execution_scope"] = "match peers" if paired else "standalone UI"
            result["proof"] = paired and result["pass"] and len(set(result["peer_boxes"].values())) == len(runs)
        result["captures"] = images
        (root / "result.json").write_text(json.dumps(retain_capture_detail(root, result), indent=2) + "\n", encoding="utf-8")
    return result


def capture_checks(value):
    """Prune raw control payloads while preserving every verdict and its scope ancestors."""
    if isinstance(value,list):
        return [kept for child in value if (kept:=capture_checks(child))]
    if not isinstance(value,dict):
        return None
    fields={'pass','passed','ok','status','verdict','required','reason','error','not_applicable','total','failed'}
    result={}
    for key,child in value.items():
        if key in fields or key.endswith('_pass'):
            result[key]=child
        elif isinstance(child,(dict,list)) and (kept:=capture_checks(child)):
            result[key]=kept
    return result


def retain_capture_detail(root, result):
    """Keep every raw control snapshot in a hashed sidecar; verdicts retain their measured checks."""
    path = Path(root)/'capture-detail.jsonl'
    with path.open('w', encoding='utf-8') as stream:
        for capture in result.get('captures', []):
            stream.write(json.dumps(capture) + '\n')
    return {key: value for key, value in result.items() if key != 'captures'} | {
        'capture_detail': dict(path=path.name, sha256=sha(path), count=len(result.get('captures', []))),
        'capture_checks': capture_checks(result.get('captures',[]))}


def self_test():
    """The platform rows, without an engine: nothing Windows-only runs at import, and a case the platform cannot
    drive is refused by name while the rows that ran decide the verdict."""
    import ast
    import tempfile
    from types import SimpleNamespace
    from unittest.mock import patch
    results = []

    def row(name, ok, detail=""):
        results.append(ok)
        print(f"[menu-readback-self-test] {'PASS' if ok else 'FAIL'} {name}" + (f": {detail}" if detail and not ok else ""))

    def import_time_nodes(node):
        for child in ast.iter_child_nodes(node):
            if not isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda)):
                yield child
                yield from import_time_nodes(child)

    tree = ast.parse(Path(__file__).read_text(encoding="utf-8"))
    windows_only = [f"line {node.lineno}: {node.attr}" for node in import_time_nodes(tree)
                    if isinstance(node, ast.Attribute) and node.attr in ("WinDLL", "windll", "OleDLL", "oledll", "WinError")]
    row("nothing-windows-only-at-import", not windows_only, "; ".join(windows_only))
    reason = unavailable_reason("net-host-left-early", "posix")
    row("posix-refuses-the-suspend-case-by-name", bool(reason) and "NtSuspendProcess" in reason, str(reason))
    row("posix-drives-the-other-cases", not any(unavailable_reason(case, "posix") for case in CASES if case != "net-host-left-early"))
    row("windows-drives-every-case", not any(unavailable_reason(case, "nt") for case in CASES))
    ran, refused = {"pass": True, "case": "repair", "size": "960x540"}, unavailable_row("net-host-left-early", "960x540", reason)
    row("ran-rows-decide-with-the-refusal-named", summarize([ran, refused]) == (True, "PASS", [f"net-host-left-early/960x540: {reason}"]))
    row("a-red-row-stays-red", summarize([{**ran, "pass": False}, refused])[:2] == (False, "FAIL"))
    row("nothing-ran-is-unavailable-not-green", summarize([refused])[:2] == (False, "UNAVAILABLE"))
    class NativeHold:
        def __init__(self):
            self.actions = []

        def suspend(self):
            self.actions.append("suspend")

        def resume(self):
            self.actions.append("resume")

    held = NativeHold()
    suspend_run(held)
    resume_run(held)
    row("hold-actions-target-the-native-runner", held.actions == ["suspend", "resume"])
    with tempfile.TemporaryDirectory() as directory, patch.object(sys.modules[__name__], "spread", None), \
            patch.object(sys.modules[__name__], "make_run") as local_factory:
        options = SimpleNamespace(repo=Path(directory), port=49830, size="960x540")
        refused_pair = run_case(options, "host-draft-roundtrip", Path(directory) / "case")
        row("paired-case-refuses-before-any-local-engine", not local_factory.called and not refused_pair["records"]
            and not refused_pair["pass"] and not refused_pair["proof"]
            and refused_pair.get("error") == "paired menu readback requires the shared spread executor")
    print(f"[menu-readback-self-test] {'PASS' if all(results) else 'FAIL'} {sum(results)}/{len(results)}")
    return 0 if all(results) else 1


def planned_cases(case, requested, all_sizes=False):
    rows = []
    for name in CASES if case == 'all' else (case,):
        sizes = [requested]
        if name != 'oracles' and (all_sizes or name in ('net-chat', 'lobby-name', 'live')):
            sizes.extend(size for key, size in SIZE_GATES if key == name and size not in sizes)
        rows.extend((name, size) for size in sizes)
    return rows


def main():
    if "--self-test" in sys.argv[1:]:
        return self_test()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--case", choices=(*CASES, "all"), required=True)
    parser.add_argument("--size", choices=("640x360", "960x540", "1280x720", "1920x1080", "2560x1440", "3840x2160", "960x540@2.6667"), required=True)
    parser.add_argument("--all-sizes", action="store_true",
                        help="also run every SIZE_GATES row; net-chat and lobby-name always do this")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--single-box-smoke", action="store_true", help="explicit local smoke for direct LAN arms; always proof:false")
    parser.add_argument("--harvest-declared", action="store_true", help="write each sweep's controls as drafts for tools/menu_declared; the sweeps then prove nothing")
    parser.add_argument("--base-words", action="store_true", help="write the new lobby, Escape and one-draft cases in the base screens' words, for their RED on the base build")
    if spread:
        spread.add_arguments(parser)
    else:
        parser.add_argument("--spread", action="store_true", help="requires the shared spread executor")
    parser.add_argument("--port", type=int, required=True)
    options = parser.parse_args()
    global BASE_WORDS
    BASE_WORDS = options.base_words
    selected = planned_cases(options.case, options.size, options.all_sizes)
    if not selected:
        parser.error('the selected size partition has no cases')
    if options.single_box_smoke:
        if any(case not in ("host-draft-roundtrip", "net-chat", "lobby-name") for case, _ in selected):
            parser.error("local smoke is restricted to the explicitly direct LAN arms")
        if getattr(options, "peer_boxes", None) or getattr(options, "spread", False):
            parser.error("local smoke cannot declare spread peers")
    if options.dry_run:
        print(json.dumps(dict(cases=selected, engine_count=max(2 if name in PAIRED_CASES else 1 for name, _ in selected))))
        return 0
    if not options.single_box_smoke and any(case in SPREAD_CASES for case, _ in selected):
        if not spread:
            parser.error("selected menu readback requires the shared spread executor")
        if getattr(options, "spread", False) or not getattr(options, "peer_boxes", None):
            parser.error(spread.NO_BOX_NAMED)
    if spread:
        spread.configure(options)
    if Path("D:/mx/LEAD_FAMILY.lock").exists():
        parser.error("LEAD_FAMILY.lock exists; no engine launch")
    if not (any(low <= options.port <= low + 9 for low in (48270, 48380, 48390, 48530, 48540, 48550, 48840, 48850, 49180, 49190))
            or 49440 <= options.port <= 49459 or 49470 <= options.port <= 49478 or 49820 <= options.port <= 49839):
        parser.error("this detector owns ports 48270-48279, 48380-48389, 48390-48399, 48530-48539, 48540-48549, 48550-48559, 48840-48849, 48850-48859, 49180-49199, 49440-49459, 49470-49478 and 49820-49839")
    options.repo = options.repo.resolve()
    options.out.mkdir(parents=True, exist_ok=False)
    options.revision = subprocess.check_output(["git", "-C", str(options.repo), "rev-parse", "HEAD"], text=True).strip()
    options.exe_sha = sha(engine_executable(options.repo))
    requested = options.size

    def sizes_for(case):
        return [size for name, size in selected if name == case]

    rows = []
    for case in CASES if options.case == "all" else (options.case,):
        if case == "oracles":
            if not sizes_for(case):
                continue
            options.size = requested
            for name, command in {"visible": (LANDING, "", "assert_visible ButtonMultiplayerHostGame 0"),
                                  "focus": (LANDING, "", "assert_focus ButtonMultiplayerJoinGame"),
                                  "rect": (LANDING, "", "assert_rect_inside ButtonMultiplayerHostGame ButtonMultiplayerJoinGame"),
                                  "text": (LANDING, "settext TextMultiplayerName " + "W" * 200 + "\n", "assert_text_fits TextMultiplayerName"),
                                  "page": (OPTIONS, "select_settings_page Misc\nwait 3\n", "assert_settings_page Gameplay"),
                                  "page-name": (OPTIONS, "", "select_settings_page Nowhere")}.items():
                rows.append(run_case(options, "landing", options.out / f"oracle-{name}" / options.size, command))
            continue
        if case == "sweep-fault":
            # A screen with one control a player cannot find and one wrongly disabled: the sweep fails and names both.
            for size in sizes_for(case):
                options.size = size
                rows.append(run_case(options, case, options.out / case / size,
                                     (LANDING + SWEEP_MACROS + "dump_host_options\n", "", "sweep MultiplayerLandingPanel label=landing restore=back_landing")))
            continue
        if case == "port-by-hand":
            # The run's port reaches the lobby through Advanced's own port box and its check: a port below 1024 is refused there,
            # and the setup keeps the port it had.
            for size in sizes_for(case):
                options.size = size
                rows.append(run_case(options, case, options.out / case / size,
                                     (LANDING + "activate ButtonMultiplayerHostGame\nwait_ms 400\nassert_substate HostSetup\n", "setup_host_port 80\n", "assert_host_port 80")))
            continue
        for size in sizes_for(case):
            options.size = size
            reason = unavailable_reason(case)
            rows.append(unavailable_row(case, size, reason) if reason else run_case(options, case, options.out / case / size))
    passed, verdict, refused = summarize(rows)
    result = {"pass": passed, "verdict": verdict, "unavailable": refused, "driver_sha256": sha(__file__), "declared_cases": selected,
              "source_revision": options.revision, "exe_sha256": options.exe_sha, "port": options.port,
              "cases": [{key: value for key, value in row.items() if key != 'captures'} |
                        {'capture_checks': capture_checks(row.get('captures',[]))} for row in rows]}
    if options.single_box_smoke:
        result.update(topology="single-box", peer_boxes=[row.get("peer_boxes", {}) for row in rows], proof=False, execution_scope="LAN smoke")
    elif all(case in SPREAD_CASES for case, _ in selected):
        result.update(topology="spread", peer_boxes=[row.get("peer_boxes", {}) for row in rows],
                      proof=passed and all(row.get("proof", False) for row in rows))
    captures = [image for row in rows for image in row.get('captures', [])]
    (options.out / "captures.json").write_text(json.dumps(captures, indent=2) + "\n", encoding="utf-8")
    result['captures_ref'] = dict(path='captures.json', sha256=sha(options.out/'captures.json'), count=len(captures))
    (options.out / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"[menu-readback] {verdict} {options.out / 'result.json'}" + (f" (unavailable here: {'; '.join(refused)})" if refused else ""))
    return 0 if passed else 3 if verdict == "UNAVAILABLE" else 1


if __name__ == "__main__":
    raise SystemExit(main())
