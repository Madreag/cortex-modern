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
         "lobby-name", "net-options", "oracles")
LANDING = "wait 40\nactivate ButtonMainToMultiplayer\nwait 12\nassert_substate Landing\n"
OPTIONS = "wait 40\nactivate ButtonMainToOptions\nwait 8\nassert_screen SettingsScreen\n"
PAGES = ("Video", "Audio", "Input", "Gameplay", "Misc", "Network")
# The network page's own selector names its sub-pages; a script reaches one as "Network:<page>"
# once the network page is up - the same reach a player's tab clicks take.
NETWORK_PAGES = ("Player", "Chat", "Recovery", "Files", "Internet")
NETWORK_TABS = tuple("TabNetPage" + page for page in NETWORK_PAGES)
NETWORK_BOXES = tuple("CollectionBoxNetPage" + page for page in NETWORK_PAGES)
NETWORK_PAGE_BOX = NETWORK_BOXES[0]
# Every control the player page owns, measured where it is drawn. The fixed-delay row belongs to
# the fixed policy, so it is drawn only there - the cue the video page's resolution rows already use.
NETWORK_ROWS = ("LabelNetworkDisplayName", "TextNetworkDisplayName", "LabelNetworkDelayPolicy",
                "RadioNetworkDelayAuto", "RadioNetworkDelayFixed", "LabelNetworkIdleWait",
                "TextNetworkIdleWait", "LabelNetworkIdleWaitHint", "CheckboxNetworkAutoRepair",
                "CheckboxNetworkToasts", "CheckboxNetworkPrediction",
                "LabelMatchStatusWidget", "ComboMatchStatusWidget")
NETWORK_FIXED_ROWS = ("LabelNetworkFixedDelay", "TextNetworkFixedDelay", "LabelNetworkFixedDelayHint")
MAX_INPUT_DELAY_FRAMES = 60  # NetMatchConfigUtil::c_MaxInputDelayFrames, which the hint states.
# The saved preferences a case starts from, and what the page must have written when it ends.
NETWORK_SEED = {"NetworkDisplayName": "ScoutLead", "NetworkHostDelayPolicy": "Auto",
                "NetworkHostIdleWaitMinutes": "10", "NetworkHostAutoRepair": "1", "NetworkInputDelayFrames": "0",
                "NetworkToastsEnabled": "0", "LocalPrediction": "0", "NetworkMatchStatusMode": "Auto"}
NETWORK_SAVED = {"NetworkDisplayName": "WingCmd", "NetworkHostDelayPolicy": "Fixed",
                 "NetworkHostIdleWaitMinutes": "25", "NetworkHostAutoRepair": "0", "NetworkInputDelayFrames": "7",
                 "NetworkToastsEnabled": "1", "LocalPrediction": "1"}
# The Misc page's own rows; the match-status combo moved to the player page, so a dump of this box
# must not name it - the absence is asserted against the whole capture, not a missing-control assert.
MISC_ROWS = ("CheckboxSkipIntro", "CheckboxShowToolTips", "CheckboxShowLoadingScreenProgressReport",
             "CheckboxShowAdvancedPerfStats", "CheckboxMeasureLoadingTime", "CheckboxUseMonospaceConsoleFont",
             "CheckboxDisableFactionBuyMenuThemes", "CheckboxDisableFactionBuyMenuThemeCursors",
             "LabelSceneBackgroundAutoScale", "LabelSceneBackgroundAutoScaleSetting", "SliderSceneBackgroundAutoScale")
MISC_GONE = ("LabelMatchStatusWidget", "ComboMatchStatusWidget")
CHAT_SEED = {"NetworkChatVisible": "1", "NetworkChatSound": "0", "NetworkChatNotify": "1",
             "NetworkChatDefaultScope": "Team", "NetworkChatTextSize": "Large"}
CHAT_SAVED = {"NetworkChatVisible": "0", "NetworkChatNotify": "0"}
RECOVERY_SEED = {"NetworkAutoReconnect": "1", "NetworkOfferStoredRejoin": "1"}
RECOVERY_SAVED = {"NetworkAutoReconnect": "0", "NetworkOfferStoredRejoin": "0"}
FILES_SEED = {"AutosaveSeconds": "45", "NetworkRecordReplays": "0"}
FILES_SAVED = {"NetworkDiagnosticsDirectory": "D:/diag-lane", "NetworkRecordReplays": "1"}
# The settings reader cuts values at "//", so the directory url persists scheme-less;
# NetDirectoryClient puts https:// back on (NetDirectoryClient.cpp:117).
INTERNET_SEED = {"SessionDirectoryUrl": "dir.example.test/serve",
                 "SessionDirectoryCertSha256": "a" * 64}
INTERNET_SAVED = {"SessionDirectoryUrl": "newdir.example.test/serve",
                  "SessionDirectoryCertSha256": "b" * 64}
# The host's saved session options steer the match; the client's own copy differs and must not.
HOST_OPTIONS = {"NetworkHostDelayPolicy": "Fixed", "NetworkHostIdleWaitMinutes": "25", "NetworkHostAutoRepair": "0"}
CLIENT_OPTIONS = {"NetworkHostDelayPolicy": "Auto", "NetworkHostIdleWaitMinutes": "5", "NetworkHostAutoRepair": "1"}
MATCH_RULES = {"delay_policy": 2, "idle_wait_minutes": 25, "automatic_repair": False}
# A combo box draws its selected item left of the drop-down button, so its text budget is narrower than its rect.
COMBO_BUTTON = 17
FIT_LINE = re.compile(r"assert_text_fits (\w+).*?rect=\[(-?\d+),(-?\d+),(-?\d+),(-?\d+)\].*?available=\[(-?\d+),(-?\d+)\]")
WATCHED = ("ComboBrainlessHumansSpectate", "ComboMatchStatusWidget")
ORDER = ("TextMultiplayerName", "ButtonMultiplayerHostGame", "ButtonMultiplayerJoinGame",
         "ButtonBackToMain", "ButtonSaveDiagnostics")
RESET_INPUT = ("wait 40\nactivate ButtonMainToOptions\nwait 5\nassert_visible TabInputSettings 1\n"
               "activate TabInputSettings\nwait 3\npost_command ButtonP2Clear\npost_command ButtonP2Clear\n"
               "wait 3\npost_command ButtonP3Clear\npost_command ButtonP3Clear\nwait 3\n"
               "post_command ButtonBackToMainMenu\nwait 5\n")
# The match pause menu's own rows, and the single-player rows it must not carry on any peer.
MATCH_ROWS = ("ButtonLeaveMatch", "ButtonPauseMatch", "ButtonSettings", "ButtonSaveDiagnostics", "ButtonResume")
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
        text, count = re.subn(rf"(?m)^(\s*{name}\s*=\s*)[^\r\n]*", lambda match: match[1] + value, text)
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
        menu_step("activate ButtonSettings"), {"op": "wait", "screen": "PauseSettings"},
        menu_step("assert_visible CollectionBoxGameplaySettings 1"),
        *row_checks("TabGameplaySettings", "CollectionBoxSettingsBase"), menu_step("dump_player_options"),
        # The pause twin of the page op: the same reach on the settings menu the pause screen owns.
        menu_step("select_settings_page Misc"), {"op": "wait", "renders": 3},
        menu_step("assert_settings_page Misc"), menu_step("dump_player_options"),
        menu_step("post_command ButtonBackToMainMenu"), {"op": "wait", "screen": "Pause"},
        *pause_rows(), menu_step("dump_host_options"), running]
    if who == "client":
        # The leaver's drive starts only once the host's checks are done, so the clean leave cannot
        # beat the running-match assertions. The menu pump that follows the leave still serves the
        # probe's menu-scope steps, so its finish lands there.
        steps += [{"op": "wait_file", "path": str(probe_root(root, "host") / "done.json")},
                  {"op": "signal", "name": "done"},
                  menu_step("activate ButtonLeaveMatch"), {"op": "wait", "screen": "PauseLeaveConfirm"},
                  menu_step("assert_rect_inside LeaveConfirmBox viewport"),
                  *row_checks("LabelLeaveConfirm", "LeaveConfirmBox"),
                  *row_checks("ButtonLeaveConfirm", "LeaveConfirmBox"),
                  *row_checks("ButtonLeaveCancel", "LeaveConfirmBox"),
                  {"op": "assert_control", "scope": "menu", "control": "LabelLeaveConfirm",
                   "equals": {}, "text_contains": "Leave the match"},
                  menu_step("dump_host_options"), menu_step("activate ButtonLeaveConfirm")]
    else:
        steps += [{"op": "signal", "name": "done"}]
    steps += [{"op": "finish"}]
    return {"schema": 1, "timeout_ms": 90000, "steps": steps}


def scripts(case, port, root):
    if case == "pause":
        # Each peer's match pause menu is its own local surface, so each peer drives its own probe.
        return ({who: f"wait_file {probe_root(root, who) / 'done.json'} 90\nexit\n" for who in ("host", "client")},
                {who: pause_probe(who, root) for who in ("host", "client")})
    probe = None
    if case == "landing":
        text = LANDING + checks("ButtonMultiplayerHostGame", "MultiplayerLandingPanel")
        text += "assert_visible ButtonMultiplayerCreate 0\n"
        for name in (*ORDER, ORDER[0]):
            text += f"focus_next\nassert_focus {name}\n"
        text += f"focus_previous\nassert_focus {ORDER[-1]}\ndump_host_options\nexit\n"
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
            text += checks(f"Tab{page}Settings", "CollectionBoxSettingsBase") + "dump_player_options\n"
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
                 "post_command CheckboxNetworkAutoRepair\npost_command CheckboxNetworkToasts\n"
                 "post_command CheckboxNetworkPrediction\nwait 3\ndump_player_options\n"
                 "post_command ButtonBackToMainMenu\nwait 5\nassert_screen MainScreen\n")
        text += LANDING + "assert_label TextMultiplayerName " + NETWORK_SAVED["NetworkDisplayName"] + "\n"
        text += checks("LabelMultiplayerNamePrompt", "MultiplayerLandingPanel")
        text += checks("TextMultiplayerName", "MultiplayerLandingPanel")
        text += "dump_host_options\n"
        # The host screen names the saved policy beside its delay box; the page flipped it to Fixed.
        text += ("activate ButtonMultiplayerHostGame\nwait 5\nassert_substate HostSetup\n"
                 "assert_visible LabelHostInputDelayPolicy 1\nassert_label LabelHostInputDelayPolicy (fixed)\n"
                 + checks("LabelHostInputDelayPolicy", "MultiplayerHostPanel") +
                 "dump_host_options\n"
                 "post_command ButtonHostBack\nwait 4\nassert_substate Landing\ndump_host_options\nexit\n")
    elif case == "net-chat":
        # Every chat row is read where it is drawn; the muted-players button stays disabled with its
        # reason until a muted-players store exists.
        text = OPTIONS + net_page("Chat")
        for control in ("CheckboxNetworkChatVisible", "CheckboxNetworkChatSound", "LabelNetworkChatScope",
                        "ComboNetworkChatScope", "CheckboxNetworkChatNotify", "LabelNetworkChatTextSize",
                        "ComboNetworkChatTextSize", "ButtonNetMutedPlayers", "LabelNetMutedReason"):
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
                        "LabelNetRecoveryStatus", "ButtonNetRejoin", "ButtonNetCancelRecovery"):
            text += checks(control, "CollectionBoxNetPageRecovery")
        text += ("assert_enabled ButtonNetRejoin 0\nassert_enabled ButtonNetCancelRecovery 0\n"
                 "assert_label LabelNetRecoveryRecord No recovery record\n"
                 "dump_player_options\n"
                 "post_command CheckboxNetworkAutoReconnect\npost_command CheckboxNetworkOfferRejoin\nwait 3\n"
                 "post_command ButtonBackToMainMenu\nwait 5\nassert_screen MainScreen\nexit\n")
    elif case == "net-files":
        # The autosave rows mirror the stored host option; the diagnostics box commits a folder and
        # the replay pref toggles. The folder buttons are layout-checked, not clicked - they own real
        # OS side effects (a shell window, a clipboard write) a readback run must not take.
        text = OPTIONS + net_page("Files")
        for control in ("LabelNetAutosaveTitle", "LabelNetAutosave", "LabelNetAutosaveIntTitle",
                        "LabelNetAutosaveInterval", "ButtonNetOpenAutosaves", "LabelNetAutosaveInfo",
                        "LabelNetDiagDirTitle", "TextNetworkDiagDir", "ButtonNetCopyDiagPath",
                        "ButtonNetSaveDiagnostics", "CheckboxNetworkRecordReplays"):
            text += checks(control, "CollectionBoxNetPageFiles")
        text += ("assert_label LabelNetAutosave Enabled\n"
                 "assert_label LabelNetAutosaveInterval 45 s\n"
                 "assert_enabled ButtonNetOpenAutosaves 1\nassert_enabled ButtonNetSaveDiagnostics 1\n"
                 "set_text TextNetworkDiagDir " + FILES_SAVED["NetworkDiagnosticsDirectory"] + "\n"
                 "post_command CheckboxNetworkRecordReplays\nwait 3\ndump_player_options\n"
                 "post_command ButtonBackToMainMenu\nwait 5\nassert_screen MainScreen\nexit\n")
    elif case == "net-internet":
        # A refused pin or URL keeps the stored value and says why on the page; a valid pair saves.
        text = OPTIONS + net_page("Internet")
        for control in ("LabelNetDirUrl", "TextNetworkDirUrl", "LabelNetDirPin", "TextNetworkDirPin",
                        "LabelNetDirStatusTitle", "LabelNetDirStatus", "ButtonNetReplays",
                        "ButtonNetConnDetails", "ButtonNetNatRelay", "LabelNetInternetReason"):
            text += checks(control, "CollectionBoxNetPageInternet")
        text += ("assert_enabled ButtonNetReplays 0\nassert_enabled ButtonNetConnDetails 0\n"
                 "assert_enabled ButtonNetNatRelay 0\nassert_label LabelNetDirStatus Configured\n"
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
                 f"settext TextHostPort {port}\nsettext TextHostPlayers 2\n"
                 "activate ButtonMultiplayerCreate\nwait 15\nassert_substate Lobby\n"
                 "dump_host_options\nexit\n")
    elif case == "net-options":
        # Two real peers: the host's saved session options ride the lobby config onto both rosters. A
        # match end quits e2e peers outright, so the host's own pause-menu leave is what pauses the
        # activity and lets its menu loop run the dump while the roster is still up.
        return ({"host": f"dump_lobby\nwait_file {probe_root(root, 'host') / 'done.json'} 90\nexit\n",
                 "client": f"wait_file {probe_root(root, 'host') / 'done.json'} 90\nexit\n"},
                {"host": {"schema": 1, "timeout_ms": 90000, "steps": [
                    {"op": "wait", "sim_at_least": 150},
                    {"op": "assert", "equals": {"service": "Running", "paused": False}, "sim_at_least": 150},
                    {"op": "key_down", "key": "Escape"}, {"op": "key_up", "key": "Escape"},
                    {"op": "wait", "screen": "Pause"}, menu_step("dump_host_options"),
                    {"op": "signal", "name": "done"},
                    menu_step("activate ButtonLeaveMatch"), {"op": "wait", "screen": "PauseLeaveConfirm"},
                    menu_step("activate ButtonLeaveConfirm"), {"op": "finish"}]}})
    elif case == "combo-fit":
        # Reached by the tab control so the measurement runs on a build that has no page op yet.
        text = (OPTIONS + "activate TabVideoSettings\nwait 3\nassert_visible ComboPresetResolution 1\n"
                "assert_text_fits ComboPresetResolution\ndump_player_options\nexit\n")
    elif case == "lobby":
        # The host starts the match, so its lobby hides the ready button the joining peers get.
        text = host_lobby(port) + "assert_visible ButtonMultiplayerReady 0\n"
        text += checks("ButtonMultiplayerLeave", "MultiplayerLobbyPanel")
        text += "assert_enabled ButtonMultiplayerStart 0\ndump_host_options\nexit\n"
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
                assert control["visible"] is True, (path, control)
                assert inside(control["rect"], value["viewport"]), (path, control)
                assert inside(control["rect"], control["parent_rect"]), (path, control)
                if control["parent"]:
                    assert control["parent"] in names, (path, control)
                    assert names[control["parent"]]["rect"] == control["parent_rect"], (path, control)
                x, y, w, h = control["rect"]
                assert source.crop((x, y, x + w, y + h)).size == (w, h), (path, control)
                assert {"name", "rect", "text", "enabled", "visible", "focus"} <= control.keys()
        rows.append({**metadata, **value, "png": str(png), "png_sha256": sha(png),
                     "json": str(path), "json_sha256": sha(path), "dimensions": [width, height]})
    return rows


def run_case(options, case, root, failing=None):
    root.mkdir(parents=True, exist_ok=False)
    texts, probes = scripts(case, options.port, root)
    if failing:
        prelude, setup, assertion = failing
        texts, probes = {"host": prelude + setup + assertion + "\nexit\n"}, {}
    inputs = root / "input.txt"
    inputs.write_text(INPUT_SCRIPT, encoding="utf-8")
    paired = case in ("pause", "live", "net-options")
    seeded = {} if failing else seeds(case)
    runs, records, argv, images = {}, {}, {}, []
    result = {"pass": False, "case": case, "scripts": {}, "records": records, "probes": {}, "seeds": seeded}
    try:
        for who in (("host", "client") if paired else ("host",)):
            script = root / f"{who}-menu.txt"
            script.write_text(texts[who], encoding="utf-8")
            result["scripts"][str(script)] = sha(script)
            args = ["-menu-script", str(script)]
            if paired:
                args += ["-net-match-service-e2e", "-net-port", str(options.port), "-net-match-peers", "2",
                         "-net-match-ticks", "400", "-net-match-input-delay", "3", "-net-autosave-seconds", "0",
                         "-input-script", str(inputs), "-net-match-report", str(root / f"{who}-match.json")]
                args += ["-net-host"] if who == "host" else ["-net-join", "127.0.0.1"]
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
            if who in seeded:
                seed_settings(runs[who].cwd / "Userdata/Settings.ini", seeded[who])

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
                assert record.get("exit_code") == 0, record
                assert "[menu-script] FAILED:" not in logs[who], logs[who][-3000:]
                images += captures(run.cwd, {"source_revision": options.revision,
                    "executable": str(options.repo / "Cortex Command.exe"), "exe_sha256": options.exe_sha,
                    "os": os.name, "configuration": "Final", "argv": record.get("argv", argv[who]),
                    "peer": who, "case": case, "logical_size": options.size})
        if not failing:
            assert images, "no paired dumps/PNGs"
            # A peer scripted to dump must have written one: the global count passed on its peer's captures.
            scripted = {who: texts[who] + json.dumps(probes.get(who, {})) for who in runs}
            silent = [who for who, script in scripted.items()
                      if "dump_" in script and not any(image["peer"] == who for image in images)]
            assert not silent, f"no readback capture from {silent}"
        for who in probes:
            observation = json.loads((probe_root(root, who) / "net-ui-result.json").read_text(encoding="utf-8"))
            result["probes"][who] = observation
            assert observation["pass"] and observation["complete"], (who, observation)
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
            expected_screens = {"host": ["Pause", "PauseSettings", "PauseSettings", "Pause"],
                                "client": ["Pause", "PauseSettings", "PauseSettings", "Pause", "PauseLeaveConfirm"]}
            for who in ("host", "client"):
                peer = [capture for capture in images if capture["peer"] == who]
                assert [capture["screen"] for capture in peer] == expected_screens[who], (who, peer)
                assert [capture["settings_page"] for capture in peer[1:3]] == ["Gameplay", "Misc"], (who, peer)
                pauses = [capture for capture in peer if capture["screen"] == "Pause"]
                for capture in (pauses[0], pauses[-1]):
                    drawn = {control["name"] for control in capture["controls"]}
                    assert set(MATCH_ROWS) <= drawn, (who, sorted(drawn))
                    assert not set(SINGLE_PLAYER_ROWS) & drawn, (who, sorted(drawn))
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
            # Recorded, not asserted: the stock skin's own content box is what overflows, and it is not this detector's to change.
            result["text_overflow"] = [[capture["settings_page"], control["name"], control["text_measure"]]
                                       for capture in images for control in capture["controls"]
                                       if control.get("text_fits") is False]
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
                     "LabelNetworkIdleWait", "CheckboxNetworkAutoRepair", "CheckboxNetworkToasts",
                     "LabelMatchStatusWidget")
            deltas = [after[b]["rect"][1] - after[a]["rect"][1] for a, b in zip(pitch, pitch[1:])]
            assert deltas == [20] * 6, deltas
            without_fixed = pitch[:2] + pitch[3:]
            closed = [rows[b]["rect"][1] - rows[a]["rect"][1] for a, b in zip(without_fixed, without_fixed[1:])]
            assert closed == [20] * 5, closed
            # The landing's name row shares the Host/Join block's centre line; doubled centres avoid halves.
            landing = {c["name"]: c for c in images[-1]["controls"]}
            prompt, box = landing["LabelMultiplayerNamePrompt"]["rect"], landing["TextMultiplayerName"]["rect"]
            host_button, join_button = landing["ButtonMultiplayerHostGame"]["rect"], landing["ButtonMultiplayerJoinGame"]["rect"]
            assert prompt[0] + box[0] + box[2] == host_button[0] + join_button[0] + join_button[2], (prompt, box, host_button, join_button)
            assert next(c["text"] for c in images[-1]["controls"] if c["name"] == "TextMultiplayerName") == NETWORK_SAVED["NetworkDisplayName"]
            # The host screen's own readback: the label sits beside the delay box and names the saved policy.
            policy = next((c for c in images[-2]["controls"] if c["name"] == "LabelHostInputDelayPolicy"), None)
            assert policy and policy["text"] == "(fixed)", policy
        if case in ("net-chat", "net-recovery", "net-files", "net-internet", "misc-page"):
            # One capture per page case: the sub-page's own rows, all fitted, and nothing the case
            # names as disabled enabled in the draw.
            sub_page = {"net-chat": "Network:Chat", "net-recovery": "Network:Recovery",
                        "net-files": "Network:Files", "net-internet": "Network:Internet",
                        "misc-page": "Misc"}[case]
            assert [capture["settings_page"] for capture in images] == [sub_page], [c["settings_page"] for c in images]
            rows = {control["name"]: control for control in images[0]["controls"]}
            expected = {"net-chat": ("CheckboxNetworkChatVisible", "CheckboxNetworkChatSound", "ComboNetworkChatScope",
                                     "CheckboxNetworkChatNotify", "ComboNetworkChatTextSize",
                                     "ButtonNetMutedPlayers", "LabelNetMutedReason"),
                        "net-recovery": ("CheckboxNetworkAutoReconnect", "CheckboxNetworkOfferRejoin",
                                         "LabelNetLastHost", "LabelNetRecoveryRecord", "LabelNetRecoveryStatus",
                                         "ButtonNetRejoin", "ButtonNetCancelRecovery"),
                        "net-files": ("LabelNetAutosave", "LabelNetAutosaveInterval", "ButtonNetOpenAutosaves",
                                      "LabelNetAutosaveInfo", "TextNetworkDiagDir", "ButtonNetCopyDiagPath",
                                      "ButtonNetSaveDiagnostics", "CheckboxNetworkRecordReplays"),
                        "net-internet": ("TextNetworkDirUrl", "TextNetworkDirPin", "LabelNetDirStatus",
                                         "ButtonNetReplays", "ButtonNetConnDetails", "ButtonNetNatRelay",
                                         "LabelNetInternetReason"),
                        "misc-page": MISC_ROWS}[case]
            assert set(expected) <= rows.keys(), (case, sorted(rows))
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
        if case == "lobby-name":
            assert next(c["text"] for c in images[0]["controls"] if c["name"] == "TextMultiplayerName") == NETWORK_SEED["NetworkDisplayName"]
            result["saved"] = read_settings(runs["host"].cwd / "Userdata/Settings.ini", {"NetworkDisplayName"})
            assert result["saved"] == {"NetworkDisplayName": "Recon7"}, result["saved"]
            # The seeded policy is automatic, so the lobby row must not call the delay fixed.
            result["lobby_row"] = next(c["text"] for c in images[-1]["controls"] if c["name"] == "LabelLobbyPlayer0")
            assert "(auto" in result["lobby_row"] and "(fixed)" not in result["lobby_row"], result["lobby_row"]
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
    parser.add_argument("--port", type=int, required=True)
    options = parser.parse_args()
    if Path("D:/mx/LEAD_FAMILY.lock").exists():
        parser.error("LEAD_FAMILY.lock exists; no engine launch")
    if not any(low <= options.port <= low + 9 for low in (48270, 48380, 48530, 48540, 48550, 48840)):
        parser.error("this detector owns ports 48270-48279, 48380-48389, 48530-48539, 48540-48549, 48550-48559 and 48840-48849")
    options.repo = options.repo.resolve()
    options.out.mkdir(parents=True, exist_ok=False)
    options.revision = subprocess.check_output(["git", "-C", str(options.repo), "rev-parse", "HEAD"], text=True).strip()
    options.exe_sha = sha(options.repo / "Cortex Command.exe")
    rows = []
    for case in CASES if options.case == "all" else (options.case,):
        if case == "oracles":
            for name, command in {"visible": (LANDING, "", "assert_visible ButtonMultiplayerHostGame 0"),
                                  "focus": (LANDING, "", "assert_focus ButtonMultiplayerJoinGame"),
                                  "rect": (LANDING, "", "assert_rect_inside ButtonMultiplayerHostGame ButtonMultiplayerJoinGame"),
                                  "text": (LANDING, "settext TextMultiplayerName " + "W" * 200 + "\n", "assert_text_fits TextMultiplayerName"),
                                  "page": (OPTIONS, "select_settings_page Misc\nwait 3\n", "assert_settings_page Gameplay"),
                                  "page-name": (OPTIONS, "", "select_settings_page Nowhere")}.items():
                rows.append(run_case(options, "landing", options.out / f"oracle-{name}" / options.size, command))
        else:
            rows.append(run_case(options, case, options.out / case / options.size))
    result = {"pass": all(row["pass"] for row in rows), "driver_sha256": sha(__file__),
              "source_revision": options.revision, "exe_sha256": options.exe_sha, "port": options.port, "cases": rows}
    (options.out / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    (options.out / "captures.json").write_text(json.dumps([image for row in rows for image in row["captures"]], indent=2) + "\n", encoding="utf-8")
    print(f"[menu-readback] {'PASS' if result['pass'] else 'FAIL'} {options.out / 'result.json'}")
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
