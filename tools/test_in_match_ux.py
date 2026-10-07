"""Operate the in-match menus by hand on private engine runs: the match pause menu, the Players panel and its moderation
actions, and the single-player pause menu that must not change.

    python tools/test_in_match_ux.py --repo <tree> --out <dir> --case pause|players|sp-pause|all --size 960x540 [--all-sizes]
                                     --port <base> [--peers 2|3|4] [--base] [--compare <earlier sp-pause result.json>]

Every press is hand-shaped: the pause menu's buttons take hand_press on one frame and hand_release on a later one, the
panel's take real mouse-down and mouse-up events. Each run's probes read the controls they press; the checks below judge
what the controls said and did. --base drives a build from before this menu work: it skips the presses on controls that
build does not have and reports every check, so the same list is RED there and GREEN on the tip.
"""

import argparse
import contextlib
import ctypes
import json
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

# An explicitly routed spread run uses the shared interface and its sibling modules.
# Local detector runs keep this tree's original imports and launch behavior.
_interface_parser = argparse.ArgumentParser(add_help=False)
_interface_parser.add_argument("--spread-tools", type=Path)
_interface_options, _ = _interface_parser.parse_known_args()
_interface_root = (_interface_options.spread_tools.resolve() if _interface_options.spread_tools
                   else Path(__file__).resolve().parent)
if not (_interface_root / "spread_peers.py").is_file():
    _interface_parser.error("the shared tools directory has no spread interface")
sys.path.insert(0, str(_interface_root))
from run_sim_test import make_run, engine_executable, file_sha256  # noqa: E402
from test_telemetry_bundle import set_visual_resolution  # noqa: E402
from e2e_video import SCREEN_WATCHES  # noqa: E402
import spread_peers as spread  # noqa: E402

SIZES = ("640x360", "960x540", "1280x720")
NAMES = ("Host", "Ana", "Ben", "Cleo")
NEWCOMER = "Dee"
LIVE_RUNNING = "The match continues while this menu is open"
LIVE_PAUSED = "The match is paused for everyone"
END_HINT = "Only the host can end the match"
# The match rows top to bottom; the ways out of the match come last and set apart.
MATCH_ROW_ORDER = ("ButtonResume", "ButtonPlayers", "ButtonPauseMatch", "ButtonMatchOptions", "ButtonSaveMatch",
                   "ButtonSettings", "ButtonSaveDiagnostics", "ButtonLeaveMatch", "ButtonEndMatch")
MATCH_CAPTIONS = {"ButtonResume": "back to game", "ButtonPlayers": "players", "ButtonPauseMatch": "pause match",
                  "ButtonMatchOptions": "match details", "ButtonSaveMatch": "save match", "ButtonSettings": "settings",
                  "ButtonSaveDiagnostics": "save diagnostics", "ButtonLeaveMatch": "leave match", "ButtonEndMatch": "end match"}
PANEL_TITLE = "PLAYERS  /  The match continues while this panel is open"
PANEL_TITLE_PAUSED = "PLAYERS  /  The match is paused for everyone"
CLIENT_SUMMARY = "Only the host can keep, give away or remove a player's place"
RULES_SUMMARY = "Rules for this round"
LIST_CHANGED = "The list changed, so nothing was done - check the names and press again"
SECOND = "Eve"
# Two players who share a 24-character name, and a newcomer with one as long: every sentence must still be whole.
LONG_SHARED = "Bartholomew Featherstone"
LONG_NEWCOMER = "Maximiliana Wolkensteins"
HELD_BETWEEN_ROUNDS = "Held - the seat is kept"
# The cost budget per frame with four players, median over at least 600 frames: the status box with the panel closed, the panel open.
COST_BUDGET_US = {"closed": 100, "open": 500}
NEW_CASES = ("stale-press", "reach", "open-place", "host-leave-live", "leave-no-ticket", "long-names", "between-rounds", "cost", "leave-press-race",
             "leave-bad-ticket", "away-names")
# The place each newcomer asks for, by stable seat (a peer's is its number less one); any held place otherwise.
APPLY_SEATS = {"reach": {NEWCOMER: "1", SECOND: "2"}, "away-names": {NEWCOMER: "1"}}
# Cases whose newcomers only ask: they are still asking when the host ends the match, and never reach its screen.
ASK_ONLY = ("reach", "long-names", "away-names")
CASE_PEERS = {"stale-press": 4, "reach": 4, "open-place": 3, "host-leave-live": 3, "leave-no-ticket": 2, "long-names": 3, "between-rounds": 2, "cost": 4,
              "leave-press-race": 3, "leave-bad-ticket": 2, "away-names": 3}
# The line a leave confirmation adds when what it does changed under a press.
LEAVE_CHANGED = "so you are still in the match"


def sha(path):
    with Path(path).open("rb") as stream:
        return file_sha256(stream)


def seed_settings(path, values):
    text = path.read_text(encoding="utf-8-sig")
    for name, value in values.items():
        text, count = re.subn(rf"(?m)^([ \t]*{name}[ \t]*=[ \t]*)[^\r\n]*", lambda match: match[1] + str(value), text)
        if count == 0:
            text += f"\n\t{name} = {value}\n"
    path.write_text(text, encoding="utf-8")


def keys(key):
    return [{"op": "key_down", "key": key}, {"op": "key_up", "key": key}]


def menu(command, **extra):
    return {"op": "menu", "command": command, **extra}


def hand(control):
    """A hand's press on a menu button: down on one frame, up on a later one."""
    return [menu(f"hand_press {control}"), {"op": "wait", "renders": 3}, menu(f"hand_release {control}"), {"op": "wait", "renders": 4}]


def confirm_end_match():
    return [{"op": "wait", "screen": "PauseLeaveConfirm", "renders": 4, "scope": "menu"},
            {"op": "assert", "equals": {"service": "Running", "screen": "PauseLeaveConfirm"}, "scope": "menu"},
            menu("hand_press ButtonLeaveConfirm", scope="menu"), {"op": "wait", "renders": 3, "scope": "menu"},
            menu("hand_release ButtonLeaveConfirm", scope="menu")]


def end_match():
    """The host's End match by hand, its last steps: a match launched from the command line has no lobby to land in, so
    neither loop steps the probe once the round has ended; the run's menu script gives the end its few frames."""
    return [menu("hand_press ButtonEndMatch"), {"op": "wait", "renders": 3}, menu("hand_release ButtonEndMatch"), *confirm_end_match(), signal("done"),
            {"op": "finish"}]


def click(control, item=None):
    """A real mouse press on a Players panel control: the button reads pushed before the mouse comes up."""
    down = {"op": "mouse_down", "control": control}
    up = {"op": "mouse_up", "control": control}
    if item is not None:
        down["item"] = up["item"] = item
        return [down, {"op": "wait", "renders": 3}, up, {"op": "wait", "renders": 4}]
    return [down, {"op": "wait", "control": control, "equals": {"pushed": True}}, up, {"op": "wait", "renders": 4}]


def read(control, scope=None, tag=None):
    step = {"op": "assert_control", "control": control}
    if scope:
        step["scope"] = scope
    if tag:
        step["tag"] = tag
    return step


def on_screen(name):
    return [{"op": "wait", "screen": name}, {"op": "wait", "renders": 4}]


def shot(name):
    return {"op": "screenshot", "name": name, "composited": True}


def signal(name, scope=None):
    step = {"op": "signal", "name": name}
    if scope:
        step["scope"] = scope
    return step


def wait_file(path):
    return {"op": "wait_file", "path": str(path)}


def probe_root(root, who):
    return root / f"{who}_probe"


def match_args(port, peers, who, ticks, extra=(), name=None):
    # The match takes its seat names from the command line, not from Settings.ini.
    args = ["-net-match-service-e2e", "-net-port", str(port), "-net-match-peers", str(peers), "-net-match-ticks", str(ticks),
            "-net-match-input-delay", "3", "-net-autosave-seconds", "0", "-num-lua-states", "4", "-net-player-name", name or who, *extra]
    return args + (["-net-host"] if who == NAMES[0] else ["-net-join", "127.0.0.1"])


def pause_probes(root, base):
    """The match pause menu on both peers: every row pressed by hand, both leave confirmations read, the client's leave taken."""
    host, client = NAMES[0], NAMES[1]
    steps = [{"op": "wait", "service": "Running", "sim_at_least": 150}, *keys("Escape"), *on_screen("Pause"),
             menu("dump_host_options"), shot("pause-host-running")]
    if not base:
        steps += [read("LabelMatchLive", "menu", "live-running"), *hand("ButtonPlayers"), {"op": "wait", "panel_open": True},
                  {"op": "wait", "renders": 4}, read("NetworkSeatsTitle", tag="panel-from-pause"), shot("players-from-pause"),
                  *click("NetworkSeatsClose"), {"op": "wait", "panel_open": False}, *keys("Escape"), *on_screen("Pause")]
    steps += [*hand("ButtonPauseMatch")]
    if not base:
        steps += [{"op": "wait", "scope": "menu", "control": "LabelMatchLive", "text_contains": LIVE_PAUSED},
                  read("LabelMatchLive", "menu", "live-paused")]
    else:
        steps += [{"op": "wait", "elapsed_ms": 1500}]
    steps += [menu("dump_host_options"), shot("pause-host-paused"), *hand("ButtonResume"), *on_screen("Gameplay"),
              {"op": "wait", "renders": 6}, shot("status-host-paused"), *keys("Escape"), *on_screen("Pause"), *hand("ButtonPauseMatch")]
    if not base:
        steps += [{"op": "wait", "scope": "menu", "control": "LabelMatchLive", "text_contains": LIVE_RUNNING}]
    else:
        steps += [{"op": "wait", "elapsed_ms": 1500}]
    steps += [*hand("ButtonMatchOptions"), *on_screen("PauseMatchOptions"), menu("dump_host_options"), shot("details-host"),
              *hand("ButtonMatchOptionsClose"), *on_screen("Pause"),
              *hand("ButtonSaveMatch"), {"op": "wait", "scope": "menu", "control": "LabelSaveMatchHint", "text_contains": "Last saved"},
              read("LabelSaveMatchHint", "menu", "save-hint-host"),
              *hand("ButtonSettings"), *on_screen("PauseSettings"), *hand("ButtonBackToMainMenu"), *on_screen("Pause"),
              *hand("ButtonSaveDiagnostics"), {"op": "wait", "elapsed_ms": 1500},
              *hand("ButtonLeaveMatch"), *on_screen("PauseLeaveConfirm"), menu("dump_host_options"), shot("leave-host"),
              *hand("ButtonLeaveCancel"), *on_screen("Pause"), *hand("ButtonResume"), *on_screen("Gameplay"),
              {"op": "wait", "renders": 6}, shot("status-host-live"), signal("host-checked"),
              wait_file(probe_root(root, client) / "left.json"), {"op": "wait", "elapsed_ms": 1500},
              {"op": "wait", "renders": 6}, shot("status-host-held"),
              *keys("Escape"), *on_screen("Pause"), *end_match()]
    host_steps = steps
    steps = [{"op": "wait", "service": "Running", "sim_at_least": 150}, *keys("Escape"), *on_screen("Pause"),
             menu("dump_host_options"), shot("pause-client")]
    if not base:
        steps += [read("LabelEndMatchHint", "menu", "end-hint-client")]
    steps += [*hand("ButtonLeaveMatch"), *on_screen("PauseLeaveConfirm"), menu("dump_host_options"), shot("leave-client"),
              *hand("ButtonLeaveCancel"), *on_screen("Pause"), *hand("ButtonResume"), *on_screen("Gameplay"),
              wait_file(probe_root(root, host) / "host-checked.json"), *keys("Escape"), *on_screen("Pause"),
              *hand("ButtonLeaveMatch"), *on_screen("PauseLeaveConfirm"), *hand("ButtonLeaveConfirm"),
              {"op": "wait", "service": "Completed", "scope": "menu"}, signal("left", "menu"), {"op": "finish"}]
    return {host: {"schema": 1, "timeout_ms": 170000, "steps": host_steps}, client: {"schema": 1, "timeout_ms": 170000, "steps": steps}}


def roster_reads(prefix, rows):
    steps = [read("NetworkSeatsTitle", tag=f"{prefix}-title"), read("NetworkSeatsSummary", tag=f"{prefix}-summary"),
             read("NetworkSeatsRoster", tag=f"{prefix}-roster")]
    for row in range(rows):
        steps += [read(f"NetworkSeatName{row}", tag=f"{prefix}-name{row}"), read(f"NetworkSeatDetail{row}", tag=f"{prefix}-detail{row}")]
    return steps


def players_probes(root, peers, base, moderate, cancel=False):
    """The Players panel on every peer; with moderate the host keeps, gives away and removes places by hand. Cancel turns the
    newcomer away (the host's withdrawal closes its join), so with cancel the host cancels the approval instead of letting it
    through, and removes and bans nobody."""
    host = NAMES[0]
    names = NAMES[:peers]
    steps = [{"op": "wait", "service": "Running", "sim_at_least": 150}, {"op": "wait", "elapsed_ms": 1500}, *keys("F6"),
             {"op": "wait", "panel_open": True}, {"op": "wait", "renders": 6}, *roster_reads("host-open", 3), shot("players-host"),
             *click("NetworkSeatsOptions"), {"op": "wait", "control": "NetworkSeatsOptionsText", "equals": {"visible": True}},
             read("NetworkSeatsSummary", tag="host-rules-summary"), read("NetworkSeatsOptions", tag="host-rules-toggle"),
             shot("rules-host"), *click("NetworkSeatsOptions"), {"op": "wait", "renders": 4}, *click("NetworkSeatsClose"),
             {"op": "wait", "panel_open": False}, signal("host-read")]
    for name in names[1:]:
        steps.append(wait_file(probe_root(root, name) / "read.json"))
    if moderate:
        leaver = names[1]
        steps += [wait_file(probe_root(root, leaver) / "left.json"), {"op": "wait", "elapsed_ms": 2500}, {"op": "wait", "renders": 6},
                  shot("status-host-held"), *keys("F6"), {"op": "wait", "panel_open": True},
                  {"op": "wait", "control": "NetworkSeatDetail0", "text_contains": "Held"}, {"op": "wait", "renders": 4},
                  *roster_reads("host-held", 3), shot("players-host-held")]
        for control in ("NetworkSeatWait0", "NetworkSeatSubstitute0", "NetworkSeatCancel0"):
            steps.append(read(control, tag=f"held-{control}"))
        if not base:
            for control in ("NetworkSeatRemove0", "NetworkSeatBan0", "NetworkSeatHint0", "NetworkSeatApplicant0"):
                steps.append(read(control, tag=f"held-{control}"))
        steps += [*click("NetworkSeatWait0"), read("NetworkSeatsStatus", tag="kept-status")]
        if not base:
            # A newcomer asks for the place; the driver freezes it so the approval stays open long enough to cancel.
            steps += [signal("ready-for-newcomer"), {"op": "wait", "control": "NetworkSeatApplicant0", "text_contains": NEWCOMER},
                      {"op": "wait", "renders": 4}, read("NetworkSeatApplicant0", tag="request-list"), shot("players-host-request"),
                      *click("NetworkSeatApplicant0", item=0), read("NetworkSeatSubstitute0", tag="let-caption")]
            if cancel:
                # The driver freezes the newcomer once the host has read the request, so the approval stays open for Cancel.
                steps += [signal("request-seen"), wait_file(root / "newcomer-frozen.json"),
                          *click("NetworkSeatSubstitute0"), read("NetworkSeatSubstitute0", tag="let-armed"), read("NetworkSeatsStatus", tag="let-armed-status"),
                          shot("players-host-let-armed"), *click("NetworkSeatSubstitute0"), read("NetworkSeatsStatus", tag="let-status"),
                          {"op": "wait", "control": "NetworkSeatCancel0", "equals": {"enabled": True}}, read("NetworkSeatCancel0", tag="cancel-caption"),
                          shot("players-host-cancel"), *click("NetworkSeatCancel0"), read("NetworkSeatsStatus", tag="cancel-status"),
                          signal("approval-cancelled"), wait_file(root / "newcomer-thawed.json"), {"op": "wait", "elapsed_ms": 3000},
                          *roster_reads("host-after-cancel", 3), shot("players-host-after-cancel")]
            else:
                steps += [*click("NetworkSeatSubstitute0"), read("NetworkSeatSubstitute0", tag="let-armed"), read("NetworkSeatsStatus", tag="let-armed-status"),
                          shot("players-host-let-armed"), *click("NetworkSeatSubstitute0"), read("NetworkSeatsStatus", tag="let-status"),
                          wait_file(probe_root(root, NEWCOMER) / "seated.json"), {"op": "wait", "elapsed_ms": 1500}, {"op": "wait", "renders": 6},
                          *roster_reads("host-after-join", 3), shot("players-host-after-join")]
            if peers >= 3 and not cancel:
                target = names[2]
                steps += [*click(f"NetworkSeatRemove@{target}"), read(f"NetworkSeatRemove@{target}", tag="remove-armed"),
                          read("NetworkSeatsStatus", tag="remove-armed-status"), shot("players-host-remove-armed"),
                          *click(f"NetworkSeatRemove@{target}"), read("NetworkSeatsStatus", tag="remove-status"),
                          {"op": "wait", "elapsed_ms": 1500}, *roster_reads("host-after-remove", 3), shot("players-host-after-remove"),
                          *click(f"NetworkSeatBan@{NEWCOMER}"), read(f"NetworkSeatBan@{NEWCOMER}", tag="ban-armed"),
                          read("NetworkSeatsStatus", tag="ban-armed-status"), *click(f"NetworkSeatBan@{NEWCOMER}"),
                          read("NetworkSeatsStatus", tag="ban-status"), {"op": "wait", "elapsed_ms": 1500},
                          *roster_reads("host-after-ban", 3), shot("players-host-after-ban")]
        steps += [*click("NetworkSeatsClose"), {"op": "wait", "panel_open": False}, {"op": "wait", "elapsed_ms": 1500}]
    steps += [*keys("Escape"), *on_screen("Pause"), *end_match()]
    probes = {host: {"schema": 1, "timeout_ms": 175000, "steps": steps}}
    for index, name in enumerate(names[1:], start=1):
        steps = [{"op": "wait", "service": "Running", "sim_at_least": 150}, wait_file(probe_root(root, host) / "host-read.json"),
                 *keys("F6"), {"op": "wait", "panel_open": True}, {"op": "wait", "renders": 6},
                 read("NetworkSeatsTitle", tag="client-title"), read("NetworkSeatsSummary", tag="client-summary"),
                 read("NetworkSeatsRoster", tag="client-roster"), shot(f"players-{name.lower()}"),
                 *click("NetworkSeatsOptions"), {"op": "wait", "control": "NetworkSeatsOptionsText", "equals": {"visible": True}},
                 read("NetworkSeatsSummary", tag="client-rules-summary"), *click("NetworkSeatsOptions"), {"op": "wait", "renders": 4},
                 *click("NetworkSeatsClose"), {"op": "wait", "panel_open": False}, signal("read")]
        if moderate and index == 1:
            steps += [*keys("Escape"), *on_screen("Pause"), *hand("ButtonLeaveMatch"), *on_screen("PauseLeaveConfirm"),
                      *hand("ButtonLeaveConfirm"), {"op": "wait", "service": "Completed", "scope": "menu"}, signal("left", "menu")]
        steps.append({"op": "finish"})
        probes[name] = {"schema": 1, "timeout_ms": 175000, "steps": steps}
    if moderate and not base:
        probes[NEWCOMER] = {"schema": 1, "timeout_ms": 175000, "steps": [
            {"op": "wait", "service": "Running", "sim_at_least": 60}, {"op": "wait", "elapsed_ms": 1500}, signal("seated"), {"op": "finish"}]}
    return probes


def status_probes(root, base):
    """The status box on both peers: its reading while everyone plays and while the host has paused the match."""
    host, client = NAMES[0], NAMES[1]
    steps = [{"op": "wait", "service": "Running", "sim_at_least": 240}, {"op": "wait", "elapsed_ms": 1200}, {"op": "wait", "renders": 6},
             read("LabelNetMatchStatus", tag="status-live"), shot("status-live"),
             *keys("F6"), {"op": "wait", "panel_open": True}, {"op": "wait", "renders": 6},
             read("LabelNetMatchStatus", tag="status-panel-open"), shot("status-panel-open"), *click("NetworkSeatsClose"),
             {"op": "wait", "panel_open": False},
             # The command-line match turns the P shortcut off (Main.cpp), so the pause goes through the menu's row.
             *keys("Escape"), *on_screen("Pause"), *hand("ButtonPauseMatch"),
             {"op": "wait", "elapsed_ms": 1500} if base else {"op": "wait", "scope": "menu", "control": "LabelMatchLive", "text_contains": LIVE_PAUSED},
             *hand("ButtonResume"), *on_screen("Gameplay"),
             {"op": "wait", "elapsed_ms": 1500} if base else {"op": "wait", "control": "LabelNetMatchStatus", "text_contains": "Match paused"},
             {"op": "wait", "renders": 6}, read("LabelNetMatchStatus", tag="status-paused"), shot("status-paused"),
             # The panel's title reads the shared pause the way the pause menu does.
             *keys("F6"), {"op": "wait", "panel_open": True}, {"op": "wait", "renders": 6}, read("NetworkSeatsTitle", tag="title-paused"),
             shot("players-paused"), *click("NetworkSeatsClose"), {"op": "wait", "panel_open": False},
             *keys("Escape"), *on_screen("Pause"), *hand("ButtonPauseMatch"),
             {"op": "wait", "elapsed_ms": 1500} if base else {"op": "wait", "scope": "menu", "control": "LabelMatchLive", "text_contains": LIVE_RUNNING},
             *hand("ButtonResume"),
             *on_screen("Gameplay"), signal("host-read"), wait_file(probe_root(root, client) / "read.json"),
             *keys("Escape"), *on_screen("Pause"), *end_match()]
    client_steps = [{"op": "wait", "service": "Running", "sim_at_least": 240}, {"op": "wait", "elapsed_ms": 1200}, {"op": "wait", "renders": 6},
                    read("LabelNetMatchStatus", tag="status-live"), shot("status-live-client"),
                    wait_file(probe_root(root, host) / "host-read.json"), signal("read"), {"op": "finish"}]
    return {host: {"schema": 1, "timeout_ms": 170000, "steps": steps}, client: {"schema": 1, "timeout_ms": 170000, "steps": client_steps}}


def check_status(checks, reads, size, diagnostics):
    compact = int(size.split("x")[1]) < 480
    for who in NAMES[:2]:
        live = reads[who].get("status-live", {}).get("text", "")
        first = live.split("\n")[0]
        checks.check(f"status-{who}-leads-with-the-state", first.startswith("Everyone is connected"), f"first line {first!r}")
        checks.check(f"status-{who}-names-the-panel-key", "F6: Players" in live, f"{live!r}")
        numbers = [token for token in ("PACE", "RTT", "delay ") if token in live]
        if diagnostics:
            # A short screen keeps the tall box while the statistics are on and the panel is closed.
            # Each peer reads its own delay, which the match sizes per sender.
            missing = [token for token in ("\nRTT ", "\nPACE ") if token not in live] + ([] if re.search(r"delay \d+ ticks", live) else ["delay N ticks"])
            checks.check(f"status-{who}-numbers-with-detailed-statistics", not missing, f"missing {missing} in {live!r}")
        else:
            checks.check(f"status-{who}-numbers-behind-detailed-statistics", not numbers, f"numbers shown by default: {numbers}")
            if not compact:
                checks.check(f"status-{who}-names-the-route", "direct connection" in live or "through a relay" in live, f"{live!r}")
    strip = reads[NAMES[0]].get("status-panel-open", {}).get("text", "")
    if compact:
        # The one-line strip a short screen draws beside the open panel: the numbers spell their units.
        if diagnostics:
            checks.check("status-strip-numbers-spell-units", " / delay 3 / RTT " in strip and "\n" not in strip, f"strip {strip!r}")
        else:
            checks.check("status-strip-words-only", strip.startswith("Everyone is connected") and "RTT" not in strip, f"strip {strip!r}")
    paused = reads[NAMES[0]].get("status-paused", {}).get("text", "")
    checks.check("status-host-paused-reads-paused", paused.split("\n")[0].startswith("Match paused - press P to resume"), f"{paused!r}")
    title = reads[NAMES[0]].get("title-paused", {})
    checks.check("panel-title-reads-the-shared-pause", title.get("text") == PANEL_TITLE_PAUSED and title.get("visible"), f"title {title.get('text')!r}")


def repair_probes(root):
    """The host repairs the match from the pause menu by hand: the first press names the cost, the second starts it."""
    host, client = NAMES[0], NAMES[1]
    steps = [{"op": "wait", "service": "Running", "sim_at_least": 200}, wait_file(probe_root(root, client) / "ready.json"),
             *keys("Escape"), *on_screen("Pause"), *hand("ButtonMatchOptions"), *on_screen("PauseMatchOptions"),
             *hand("ButtonMatchRepairNow"), read("LabelMatchRepairHint", "menu", "repair-armed"), shot("repair-armed"),
             *hand("ButtonMatchRepairNow"), {"op": "wait", "elapsed_ms": 500}, signal("pressed"),
             wait_file(probe_root(root, client) / "shot.json"), {"op": "wait", "elapsed_ms": 6000}, signal("done"), {"op": "finish"}]
    client_steps = [{"op": "wait", "service": "Running", "sim_at_least": 200}, signal("ready"),
                    wait_file(probe_root(root, host) / "pressed.json"), {"op": "wait", "elapsed_ms": 400}, {"op": "wait", "renders": 2},
                    shot("repair-wait-client"), signal("shot"), {"op": "finish"}]
    return {host: {"schema": 1, "timeout_ms": 170000, "steps": steps}, client: {"schema": 1, "timeout_ms": 170000, "steps": client_steps}}


def host_leave_probes(root, peers):
    """The host leaves by hand; the sentence it read must be what happens to the others. Once the host has left, a client
    may be in the menus (its match ended) or still playing (a new host took it), so its steps from there are menu-scope."""
    host, clients = NAMES[0], list(NAMES[1:peers])
    steps = [{"op": "wait", "service": "Running", "sim_at_least": 200}, *keys("Escape"), *on_screen("Pause"),
             *hand("ButtonLeaveMatch"), *on_screen("PauseLeaveConfirm"), menu("dump_host_options"), shot("leave-host"),
             *hand("ButtonLeaveConfirm"), {"op": "wait", "service": "Completed", "scope": "menu"}, signal("left", "menu"),
             # A client whose match ends with the host's leave has no menu to return to here, so the host waits a fixed span.
             {"op": "wait", "elapsed_ms": 12000, "scope": "menu"}, signal("done", "menu"), {"op": "finish"}]
    probes = {host: {"schema": 1, "timeout_ms": 170000, "steps": steps}}
    for client in clients:
        probes[client] = {"schema": 1, "timeout_ms": 170000, "steps": [
            {"op": "wait", "service": "Running", "sim_at_least": 200},
            {"op": "wait_file", "path": str(probe_root(root, host) / "left.json"), "scope": "menu"},
            {"op": "wait", "elapsed_ms": 8000, "scope": "menu"}, signal("checked", "menu"), {"op": "finish"}]}
    return probes


def check_repair(checks, reads, logs):
    host, client = NAMES[0], NAMES[1]
    armed = reads[host].get("repair-armed", {}).get("text", "")
    checks.check("repair-first-press-names-the-cost", "press again" in armed, f"hint {armed!r}")
    missing = [who for who in (host, client) if "[net-match] resync: match relaunched from the snapshot" not in logs[who]]
    checks.check("repair-second-press-repairs", not missing, f"no snapshot reload logged by {missing}" if missing else "both peers reloaded the snapshot")


TOOK_OVER = r"now hosting|hosting the match alone|Handover complete"
HOST_LEFT = r"Host left the match at frame \d+"


def check_host_leave(checks, captures, logs, peers):
    host, clients = NAMES[0], list(NAMES[1:peers])
    confirm = next((capture for capture in captures[host] if capture["screen"] == "PauseLeaveConfirm"), None)
    label = control_of(confirm, "LabelLeaveConfirm") if confirm else None
    text = label["text"] if label else ""
    plays_on = "Another player becomes the host" in text
    ends = "The match ends for everyone" in text
    took_over = [client for client in clients if re.search(TOOK_OVER, logs[client], re.I)]
    ended = [client for client in clients if client not in took_over and re.search(HOST_LEFT, logs[client])]
    checks.check("leave-text-host-true", (plays_on and took_over and not ended) or (ends and not took_over and len(ended) == len(clients)),
                 f"confirmation {text!r}; a client took the match over: {took_over}; the match ended for: {ended}")


def sp_pause_probe():
    # The title screen's own scene reads as Gameplay too, paused, so the probe waits for the started skirmish: a build with the
    # probe's paused wait waits for it outright, an earlier one by the menu script's seven seconds of waits. Escape goes in at
    # frame 120, as the single-player smoke scene presses it, since the duel ends on its own a few hundred frames in and an
    # ended activity pauses to the scenario screen. Single player's pause menu runs in the menu loop: menu-scope steps.
    return {"schema": 1, "timeout_ms": 120000, "steps": [
        {"op": "wait", "elapsed_ms": 9000, "scope": "menu"}, {"op": "wait", "paused": False, "sim_at_least": 120},
        *keys("Escape"), {"op": "wait", "screen": "Pause", "scope": "menu"},
        {"op": "wait", "elapsed_ms": 600, "scope": "menu"}, menu("dump_host_options"), signal("done", "menu"), {"op": "finish"}]}


SP_MENU = ("wait_ms 2000\nassert_screen MainScreen\nactivate ButtonMainToSkirmish\nwait_ms 1600\nassert_screen ScenarioPicker\n"
           "combo_select ComboBoxActivitySelect P4 Alpha Duel\nwait_ms 700\nselect_scene Grasslands\nwait_ms 900\n"
           "activate ButtonStartActivityConfig\nwait_ms 700\nassert_screen ScenarioConfig\nactivate P1T1Box\nactivate P2T2Box\n"
           "wait_ms 1000\nactivate ButtonStartGame\nwait_file {done} 100\nwait_ms 700\nexit\n")


def dumps(runtime):
    rows = []
    for path in sorted((runtime / "ScreenShots").glob("dump_host_options_*.json"), key=lambda path: int(path.stem.rsplit("_", 1)[1])):
        value = json.loads(path.read_text(encoding="utf-8"))
        rows.append({"screen": value["screen"], "controls": value["controls"], "png": str(path.with_suffix(".png")), "json": str(path)})
    return rows


def tagged_reads(probe, result):
    """Each tagged control read the probe made, by its tag."""
    reads = {}
    for done in result.get("steps", []):
        step = probe["steps"][done["index"]] if done.get("index", -1) < len(probe["steps"]) else {}
        if step.get("tag") and "control" in done.get("observed", {}):
            reads[step["tag"]] = done["observed"]["control"]
    return reads


class Checks:
    def __init__(self):
        self.rows = []

    def check(self, name, ok, detail):
        self.rows.append({"check": name, "pass": bool(ok), "detail": detail})
        print(f"[in-match-ux] {'GREEN' if ok else 'RED'} {name}: {detail}", flush=True)


def control_of(capture, name):
    return next((control for control in capture["controls"] if control["name"] == name), None)


def check_pause(checks, captures, reads, logs, runtimes):
    host, client = NAMES[0], NAMES[1]
    running = next((capture for capture in captures[host] if capture["screen"] == "Pause"), None)
    checks.check("pause-menu-opens-by-key", running is not None, "the host's first dump is on the Pause screen" if running else "no Pause dump")
    if running:
        rows = {name: control_of(running, name) for name in MATCH_ROW_ORDER}
        missing = [name for name, control in rows.items() if control is None]
        checks.check("pause-rows-present", not missing, f"missing {missing}" if missing else "every match row is drawn")
        present = [(control["rect"][1], name) for name, control in rows.items() if control]
        order = [name for _, name in sorted(present)]
        expected = [name for name in MATCH_ROW_ORDER if rows[name]]
        checks.check("pause-back-to-game-first", order[:1] == ["ButtonResume"], f"top row {order[:1]}")
        checks.check("pause-row-order", order == expected and not missing, f"drawn top to bottom {order}")
        if rows["ButtonSaveDiagnostics"] and rows["ButtonLeaveMatch"]:
            gap = rows["ButtonLeaveMatch"]["rect"][1] - (rows["ButtonSaveDiagnostics"]["rect"][1] + rows["ButtonSaveDiagnostics"]["rect"][3])
            checks.check("pause-leave-set-apart", gap >= 10 and order[-2:] == ["ButtonLeaveMatch", "ButtonEndMatch"],
                         f"{gap} px between the last ordinary row and Leave; last rows {order[-2:]}")
        captions = {name: (control["text"] or "").lower() for name, control in rows.items() if control}
        wrong = {name: text for name, text in captions.items() if MATCH_CAPTIONS[name] not in text}
        checks.check("pause-row-captions", not wrong and not missing, f"captions {captions}")
        live = control_of(running, "LabelMatchLive")
        checks.check("pause-says-match-continues", live is not None and live["text"] == LIVE_RUNNING, f"line {live['text'] if live else None!r}")
    paused = [capture for capture in captures[host] if capture["screen"] == "Pause"][1:2]
    live = control_of(paused[0], "LabelMatchLive") if paused else None
    checks.check("pause-says-paused-for-everyone", live is not None and live["text"] == LIVE_PAUSED, f"line while paused {live['text'] if live else None!r}")
    if paused:
        row = control_of(paused[0], "ButtonPauseMatch")
        checks.check("pause-row-reads-resume-match", row is not None and "resume match" in row["text"].lower(), f"pause row {row['text'] if row else None!r}")
    for who in (host, client):
        confirm = next((capture for capture in captures[who] if capture["screen"] == "PauseLeaveConfirm"), None)
        label = control_of(confirm, "LabelLeaveConfirm") if confirm else None
        text = label["text"] if label else ""
        if who == client:
            truthful = "your seat stays yours" in text.lower() and "Rejoin Match" in text and "cannot be reclaimed" not in text
            kept = "[net-reconnect] leave: Left (ticket kept)" in logs[client]
            checks.check("leave-text-client-true", truthful and kept,
                         f"confirmation {text!r}; the service kept the seat's ticket: {kept}")
        else:
            hands_over = "Another player becomes the host" in text
            ends = "The match ends for everyone" in text
            checks.check("leave-text-host-names-an-outcome", hands_over or ends, f"confirmation {text!r}")
        for name in ("LabelLeaveConfirm", "ButtonLeaveConfirm", "ButtonLeaveCancel"):
            control = control_of(confirm, name) if confirm else None
            inside = control is not None and control["rect"][1] + control["rect"][3] <= control["parent_rect"][1] + control["parent_rect"][3]
            checks.check(f"leave-box-{who}-{name}-inside", inside, f"{control['rect'] if control else None} in {control['parent_rect'] if control else None}")
    hint = reads[client].get("end-hint-client")
    checks.check("end-match-says-why-on-a-client", hint is not None and hint.get("text") == END_HINT and hint.get("visible"), f"{hint}")
    from_pause = reads[host].get("panel-from-pause")
    checks.check("players-row-opens-the-panel", from_pause is not None and from_pause.get("visible"), f"panel title read {from_pause}")
    save = reads[host].get("save-hint-host")
    checks.check("save-row-saves", save is not None and "Last saved" in save.get("text", ""), f"{save}")
    bundles = sorted((runtimes[host] / "Telemetry").glob("diag-*.zip")) if (runtimes[host] / "Telemetry").is_dir() else []
    checks.check("save-diagnostics-row-saves", bool(bundles), f"{[path.name for path in bundles]}")
    screens = [capture["screen"] for capture in captures[host]]
    checks.check("match-details-row-opens-details", "PauseMatchOptions" in screens, f"host dump screens {screens}")
    # The pause menu's End match is the only caller of the agreed end (MenuMan -> EndMatchAtAgreedFrame -> CompleteAtAgreedEnd).
    ended = re.search(r"\[net-match\] the host ends the round at frame \d+", logs[host]) is not None
    checks.check("end-match-row-ends-the-match", ended, "the host log names the host's end" if ended else "no host end in the log")


def roster_names(text):
    return [line.split("  /  ")[0].replace(" (you)", "").strip() for line in text.splitlines() if line.strip() and "  /  " in line]


def check_players(checks, reads, peers, logs, base, moderate, cancel=False):
    host = NAMES[0]
    names = list(NAMES[:peers])
    title = reads[host].get("host-open-title", {})
    checks.check("panel-title-players", title.get("text") == PANEL_TITLE, f"title {title.get('text')!r}")
    summary = reads[host].get("host-open-summary", {})
    checks.check("panel-host-summary", summary.get("text") == "Everyone is playing", f"summary {summary.get('text')!r}")
    roster = reads[host].get("host-open-roster", {})
    rows = [reads[host].get(f"host-open-name{row}", {}) for row in range(3)]
    shown = roster_names(roster.get("text", "")) if roster.get("visible") else []
    shown += [row.get("text", "").strip() for row in rows if row.get("visible")]
    checks.check("panel-host-roster-n-of-n", sorted(shown) == sorted(names), f"host panel shows {len(set(shown) & set(names))} of {peers}: {shown}")
    you = roster.get("text", "")
    checks.check("panel-host-marks-itself", f"{host} (you)" in you, f"host line {you!r}")
    rules = reads[host].get("host-rules-summary", {})
    checks.check("panel-rules-for-this-round", rules.get("text") == RULES_SUMMARY, f"options view says {rules.get('text')!r}")
    toggle = reads[host].get("host-rules-toggle", {})
    checks.check("panel-rules-toggle-names-the-way-back", toggle.get("text") == "Back to players", f"toggle {toggle.get('text')!r}")
    for name in names[1:]:
        client_roster = reads[name].get("client-roster", {})
        listed = roster_names(client_roster.get("text", ""))
        checks.check(f"panel-{name}-roster-n-of-n", sorted(listed) == sorted(names), f"{name} sees {listed}")
        checks.check(f"panel-{name}-marks-itself", f"{name} (you)" in client_roster.get("text", ""), f"{client_roster.get('text')!r}")
        client_summary = reads[name].get("client-summary", {})
        checks.check(f"panel-{name}-summary", client_summary.get("text") == CLIENT_SUMMARY, f"{client_summary.get('text')!r}")
        client_rules = reads[name].get("client-rules-summary", {})
        checks.check(f"panel-{name}-rules-for-this-round", client_rules.get("text") == RULES_SUMMARY, f"{client_rules.get('text')!r}")
    if not moderate:
        return
    leaver = names[1]
    name0 = reads[host].get("host-held-name0", {})
    detail0 = reads[host].get("host-held-detail0", {})
    checks.check("held-row-names-the-player", name0.get("text", "").strip() == leaver, f"row {name0.get('text')!r}")
    checks.check("held-row-says-why", "Held - AI in control" in detail0.get("text", "") and "Left " in detail0.get("text", ""), f"detail {detail0.get('text')!r}")
    keep = reads[host].get("held-NetworkSeatWait0", {})
    checks.check("keep-names-whom", keep.get("text") == f"Keep for {leaver}" and keep.get("enabled"), f"{keep.get('text')!r} enabled={keep.get('enabled')}")
    let = reads[host].get("held-NetworkSeatSubstitute0", {})
    checks.check("let-join-names-whom-or-why-not", let.get("text") == "Let someone join" and not let.get("enabled"), f"{let.get('text')!r} enabled={let.get('enabled')}")
    hint = reads[host].get("held-NetworkSeatHint0", {})
    # The reason wraps in its column; read as the sentence it is.
    checks.check("held-row-says-why-actions-are-off", f"Nobody has asked for {leaver}'s place yet" in " ".join(hint.get("text", "").split()), f"hint {hint.get('text')!r}")
    remove = reads[host].get("held-NetworkSeatRemove0", {})
    checks.check("remove-names-whom", remove.get("text") == f"Remove {leaver}" and remove.get("enabled"), f"{remove.get('text')!r}")
    ban = reads[host].get("held-NetworkSeatBan0", {})
    checks.check("ban-names-whom", ban.get("text") == f"Ban {leaver} for this session" and ban.get("enabled"), f"{ban.get('text')!r}")
    kept = reads[host].get("kept-status", {})
    checks.check("keep-operates", f"{leaver}'s place stays theirs" in kept.get("text", ""), f"status {kept.get('text')!r}")
    if base:
        return
    request = reads[host].get("request-list", {})
    checks.check("requests-are-a-list", NEWCOMER in request.get("text", "") and "asks for" in request.get("text", ""), f"list {request.get('text')!r}")
    let_caption = reads[host].get("let-caption", {})
    checks.check("let-names-the-newcomer", let_caption.get("text") == f"Let {NEWCOMER} join" and let_caption.get("enabled"), f"{let_caption.get('text')!r}")
    armed = reads[host].get("let-armed-status", {})
    checks.check("let-says-the-consequence-first", f"{NEWCOMER} takes {leaver}'s place" in armed.get("text", "") and "press again" in armed.get("text", ""), f"{armed.get('text')!r}")
    if cancel:
        offered = reads[host].get("cancel-caption", {})
        checks.check("cancel-is-offered-while-pending", offered.get("text") == "Cancel this approval" and offered.get("enabled"), f"{offered.get('text')!r}")
        cancelled = reads[host].get("cancel-status", {})
        checks.check("cancel-operates", f"{NEWCOMER} will not join" in cancelled.get("text", ""), f"{cancelled.get('text')!r}")
        turned_away = "the host withdrew this substitution" in logs[NEWCOMER]
        checks.check("cancelled-newcomer-is-told", turned_away, f"{NEWCOMER}'s log names the withdrawal: {turned_away}")
        after = reads[host].get("host-after-cancel-roster", {}).get("text", "") + " ".join(
            reads[host].get(f"host-after-cancel-name{row}", {}).get("text", "") for row in range(3))
        checks.check("cancelled-place-stays-held", leaver in after and NEWCOMER not in after, f"after the cancel: {after!r}")
        return
    let_status = reads[host].get("let-status", {})
    checks.check("let-operates", f"{NEWCOMER} is joining in {leaver}'s place" in let_status.get("text", ""), f"{let_status.get('text')!r}")
    joined = [reads[host].get(f"host-after-join-name{row}", {}).get("text", "") for row in range(3)]
    checks.check("newcomer-takes-the-place", any(NEWCOMER in text for text in joined) or NEWCOMER in reads[host].get("host-after-join-roster", {}).get("text", ""), f"rows {joined}")
    if peers < 3:
        return
    target = names[2]
    armed = reads[host].get("remove-armed-status", {})
    checks.check("remove-says-the-consequence-first", f"{target} loses the place" in armed.get("text", "") and "press again" in armed.get("text", ""), f"{armed.get('text')!r}")
    removed = reads[host].get("remove-status", {})
    checks.check("remove-operates", f"{target} was removed" in removed.get("text", ""), f"{removed.get('text')!r}")
    kicked = "removed from this session" in logs[target]
    checks.check("removed-player-is-told", kicked, f"{target}'s log names the removal: {kicked}")
    armed = reads[host].get("ban-armed-status", {})
    checks.check("ban-says-the-consequence-first", "cannot join this session again" in armed.get("text", "") and "press again" in armed.get("text", ""), f"{armed.get('text')!r}")
    banned = reads[host].get("ban-status", {})
    checks.check("ban-operates", f"{NEWCOMER} was banned" in banned.get("text", ""), f"{banned.get('text')!r}")
    told = "banned from this session" in logs[NEWCOMER]
    checks.check("banned-player-is-told", told, f"{NEWCOMER}'s log names the ban: {told}")


def at_leave_confirm(signal_name):
    """A player waits at its own leave confirmation, so its leave lands the moment the host's case wants it."""
    return [*keys("Escape"), *on_screen("Pause"), *hand("ButtonLeaveMatch"), *on_screen("PauseLeaveConfirm"), signal(signal_name)]


def confirm_leave():
    return [*hand("ButtonLeaveConfirm"), {"op": "wait", "service": "Completed", "scope": "menu"}, signal("left", "menu"), {"op": "finish"}]


def open_panel():
    return [*keys("F6"), {"op": "wait", "panel_open": True}, {"op": "wait", "renders": 6}]


def close_and_end():
    return [*click("NetworkSeatsClose"), {"op": "wait", "panel_open": False}, {"op": "wait", "elapsed_ms": 1500}, signal("done-reading"),
            *keys("Escape"), *on_screen("Pause"), *end_match()]


def find_row(name, base):
    """A player's row on the open panel, reached by hand through its pages; a build without pages can only wait for it."""
    if base:
        return [{"op": "wait", "control": f"NetworkSeatName@{name}", "equals": {"visible": True}}]
    return [{"op": "show_row", "name": name}, {"op": "wait", "renders": 2}]


def plays_on(root, host):
    return [{"op": "wait", "service": "Running", "sim_at_least": 150}, wait_file(probe_root(root, host) / "done-reading.json"), {"op": "finish"}]


def stale_press_probes(root, base):
    """A: a press held while the rows change under it, released on the same control. B: a press dragged off and released, the
    rows change, then a click. Each must act on the player the control shows at the release, or on nobody."""
    host, ana, ben, cleo = NAMES
    steps = [{"op": "wait", "service": "Running", "sim_at_least": 150}, wait_file(probe_root(root, cleo) / "at-confirm.json"),
             wait_file(probe_root(root, ana) / "at-confirm.json"), *open_panel(), *roster_reads("a-before", 3),
             *click(f"NetworkSeatRemove@{ben}"), read("NetworkSeatsStatus", tag="a-armed"), read("NetworkSeatName1", tag="a-slot-before"),
             {"op": "mouse_down", "control": "NetworkSeatRemove1"}, {"op": "wait", "control": "NetworkSeatRemove1", "equals": {"pushed": True}},
             signal("a-held"), {"op": "wait", "control": "NetworkSeatName1", "text_contains": ana}, {"op": "wait", "renders": 2},
             read("NetworkSeatRemove1", tag="a-slot-caption"), {"op": "mouse_up", "control": "NetworkSeatRemove1"}, {"op": "wait", "renders": 4},
             read("NetworkSeatsStatus", tag="a-status"), *roster_reads("a-after", 3), shot("stale-press-a"),
             read("NetworkSeatName1", tag="b-slot-before"),
             {"op": "mouse_down", "control": "NetworkSeatRemove1"}, {"op": "wait", "control": "NetworkSeatRemove1", "equals": {"pushed": True}},
             {"op": "mouse_move", "control": "NetworkSeatsTitle"}, {"op": "wait", "renders": 3},
             {"op": "mouse_up", "control": "NetworkSeatsTitle"}, {"op": "wait", "renders": 4}, read("NetworkSeatsStatus", tag="b-after-release"),
             signal("b-released"), {"op": "wait", "control": "NetworkSeatName1", "text_contains": cleo}, {"op": "wait", "renders": 2},
             read("NetworkSeatRemove1", tag="b-slot-caption"), *click("NetworkSeatRemove1"), read("NetworkSeatsStatus", tag="b-status"),
             shot("stale-press-b"), *close_and_end()]
    probes = {host: {"schema": 1, "timeout_ms": 175000, "steps": steps}}
    probes[cleo] = {"schema": 1, "timeout_ms": 175000, "steps": [{"op": "wait", "service": "Running", "sim_at_least": 150}, *at_leave_confirm("at-confirm"),
                                                                 wait_file(probe_root(root, host) / "a-held.json"), *confirm_leave()]}
    probes[ana] = {"schema": 1, "timeout_ms": 175000, "steps": [{"op": "wait", "service": "Running", "sim_at_least": 150}, *at_leave_confirm("at-confirm"),
                                                                wait_file(probe_root(root, host) / "b-released.json"), *confirm_leave()]}
    probes[ben] = {"schema": 1, "timeout_ms": 175000, "steps": plays_on(root, host)}
    return probes


def check_stale_press(checks, reads, logs):
    host, ana, ben, cleo = NAMES
    before = reads[host].get("a-slot-before", {}).get("text", "").strip()
    checks.check("stale-a-pressed-ben", before == ben, f"slot 1 read {before!r} when the press went down")
    caption = reads[host].get("a-slot-caption", {}).get("text", "")
    checks.check("stale-a-control-shows-another-at-release", ana in caption and ben not in caption, f"the pressed control reads {caption!r} at the release")
    status = reads[host].get("a-status", {}).get("text", "")
    flat = " ".join(status.split())
    acted = [who for who in (ana, ben) if "removed from this session" in logs[who]]
    checks.check("stale-a-acts-on-nobody", flat == LIST_CHANGED and not acted and ben not in flat,
                 f"status {status!r}; removed: {acted}")
    caption = reads[host].get("b-slot-caption", {}).get("text", "")
    status = " ".join(reads[host].get("b-status", {}).get("text", "").split())
    shown = caption.replace("Confirm: remove ", "").replace("Remove ", "").strip()
    checks.check("stale-b-acts-on-the-player-shown", bool(shown) and status.startswith(f"{shown} loses the place") and "press again" in status,
                 f"clicked {caption!r}; status {status!r}")
    removed = [who for who in (ana, ben, cleo) if "removed from this session" in logs[who]]
    checks.check("stale-nobody-removed", not removed, f"removed: {removed}")


def reach_probes(root, base):
    """Four players, two away with a request for each place: every other player's Remove reaches its first-press sentence."""
    host, ana, ben, cleo = NAMES
    # The summary counts both requests whichever page their rows are on.
    steps = [{"op": "wait", "service": "Running", "sim_at_least": 150}, wait_file(probe_root(root, ana) / "left.json"),
             wait_file(probe_root(root, ben) / "left.json"), *open_panel(),
             {"op": "wait", "control": "NetworkSeatsSummary", "text_contains": "2 players are away"}, signal("ready-for-newcomer"),
             {"op": "wait", "control": "NetworkSeatsSummary", "text_contains": "2 requests to join"}, {"op": "wait", "renders": 6},
             *roster_reads("reach", 3), *([] if base else [read("NetworkSeatsMore", tag="reach-more")]), shot("reach-requests")]
    for target in (ana, ben, cleo):
        steps += [*find_row(target, base), *click(f"NetworkSeatRemove@{target}"), read("NetworkSeatsStatus", tag=f"reach-{target}"),
                  shot(f"reach-{target.lower()}")]
    steps += close_and_end()
    probes = {host: {"schema": 1, "timeout_ms": 175000, "steps": steps}}
    for who in (ana, ben):
        probes[who] = {"schema": 1, "timeout_ms": 175000, "steps": [{"op": "wait", "service": "Running", "sim_at_least": 150},
                                                                    *at_leave_confirm("at-confirm"), *confirm_leave()]}
    probes[cleo] = {"schema": 1, "timeout_ms": 175000, "steps": plays_on(root, host)}
    for who in (NEWCOMER, SECOND):
        probes[who] = {"schema": 1, "timeout_ms": 175000, "steps": [wait_file(probe_root(root, host) / "done-reading.json"), {"op": "finish"}]}
    return probes


def check_reach(checks, reads):
    host = NAMES[0]
    for target in NAMES[1:]:
        status = " ".join(reads[host].get(f"reach-{target}", {}).get("text", "").split())
        checks.check(f"reach-{target}-remove-names-them", status.startswith(f"{target} loses the place") and "press again" in status, f"status {status!r}")


def open_place_probes(root, base):
    """The host removes a player; a newcomer asks for the opened place; the host lets them in from the panel."""
    host, ana, ben = NAMES[:3]
    place = f"Open place (seat 3)"
    steps = [{"op": "wait", "service": "Running", "sim_at_least": 150}, *open_panel(), *find_row(ben, base),
             *click(f"NetworkSeatRemove@{ben}"), *click(f"NetworkSeatRemove@{ben}"), read("NetworkSeatsStatus", tag="removed"),
             {"op": "wait", "elapsed_ms": 1500}, signal("ready-for-newcomer"),
             {"op": "wait", "control": "NetworkSeatsSummary", "text_contains": "1 request to join"},
             {"op": "wait", "control": f"NetworkSeatApplicant@{place}", "text_contains": NEWCOMER}, {"op": "wait", "renders": 6},
             *roster_reads("open", 3), read(f"NetworkSeatApplicant@{place}", tag="open-requests"), shot("open-place-request"),
             *click(f"NetworkSeatApplicant@{place}", item=0), {"op": "wait", "control": f"NetworkSeatApplicant@{place}", "equals": {"selected": 0}},
             read(f"NetworkSeatSubstitute@{place}", tag="open-let-caption"), *click(f"NetworkSeatSubstitute@{place}"),
             read("NetworkSeatsStatus", tag="open-let-armed"), *click(f"NetworkSeatSubstitute@{place}"), read("NetworkSeatsStatus", tag="open-let"),
             wait_file(probe_root(root, NEWCOMER) / "seated.json"), {"op": "wait", "elapsed_ms": 1500}, {"op": "wait", "renders": 6},
             *roster_reads("open-after", 3), shot("open-place-after"), *close_and_end()]
    probes = {host: {"schema": 1, "timeout_ms": 175000, "steps": steps}}
    for who in (ana, ben):
        probes[who] = {"schema": 1, "timeout_ms": 175000, "steps": plays_on(root, host)}
    probes[NEWCOMER] = {"schema": 1, "timeout_ms": 175000, "steps": [
        {"op": "wait", "service": "Running", "sim_at_least": 60}, {"op": "wait", "elapsed_ms": 1500}, signal("seated"), {"op": "finish"}]}
    return probes


def check_open_place(checks, reads, logs):
    host, ben = NAMES[0], NAMES[2]
    removed = " ".join(reads[host].get("removed", {}).get("text", "").split())
    checks.check("open-place-removal-done", removed == f"{ben} was removed from the match.", f"status {removed!r}")
    requests = reads[host].get("open-requests", {})
    checks.check("open-place-request-shown", NEWCOMER in requests.get("text", "") and requests.get("visible"), f"list {requests.get('text')!r}")
    caption = reads[host].get("open-let-caption", {}).get("text", "")
    checks.check("open-place-let-names-the-newcomer", caption == f"Let {NEWCOMER} join", f"caption {caption!r}")
    armed = " ".join(reads[host].get("open-let-armed", {}).get("text", "").split())
    checks.check("open-place-let-says-the-consequence-first", armed == f"{NEWCOMER} takes this open place - press again to confirm", f"status {armed!r}")
    done = " ".join(reads[host].get("open-let", {}).get("text", "").split())
    checks.check("open-place-let-operates", done.startswith(f"{NEWCOMER} is joining in"), f"status {done!r}")
    after = reads[host].get("open-after-roster", {}).get("text", "") + " ".join(reads[host].get(f"open-after-name{row}", {}).get("text", "") for row in range(3))
    checks.check("open-place-newcomer-plays", NEWCOMER in after, f"after the join: {after!r}")
    # The place is the newcomer's while it comes in: the host's screen watch sees every frame of that.
    left = re.findall(r"^\[text-watch\] violation h15-held-reads-held (.*)$", logs[host], re.M)
    checks.check("open-place-newcomer-never-reads-left", not left, f"{len(left)} frames read a held place as Left; first {left[0][:300] if left else ''!r}")


def host_leave_live_probes(root):
    """The host opens its leave while two others play; one of them leaves; the sentence follows, and the press does what it says."""
    host, ana, ben = NAMES[:3]
    steps = [{"op": "wait", "service": "Running", "sim_at_least": 200}, wait_file(probe_root(root, ben) / "at-confirm.json"),
             *keys("Escape"), *on_screen("Pause"), *hand("ButtonLeaveMatch"), *on_screen("PauseLeaveConfirm"),
             read("LabelLeaveConfirm", "menu", "leave-before"), signal("host-at-confirm"), wait_file(probe_root(root, ben) / "left.json"),
             {"op": "wait", "elapsed_ms": 3000}, {"op": "wait", "renders": 4}, read("LabelLeaveConfirm", "menu", "leave-after"),
             menu("dump_host_options"), shot("leave-host-after"), *hand("ButtonLeaveConfirm"), {"op": "wait", "service": "Completed", "scope": "menu"},
             signal("left", "menu"), {"op": "wait", "elapsed_ms": 12000, "scope": "menu"}, signal("done", "menu"), {"op": "finish"}]
    probes = {host: {"schema": 1, "timeout_ms": 170000, "steps": steps}}
    probes[ben] = {"schema": 1, "timeout_ms": 170000, "steps": [{"op": "wait", "service": "Running", "sim_at_least": 200}, *at_leave_confirm("at-confirm"),
                                                                wait_file(probe_root(root, host) / "host-at-confirm.json"), *confirm_leave()]}
    probes[ana] = {"schema": 1, "timeout_ms": 170000, "steps": [
        {"op": "wait", "service": "Running", "sim_at_least": 200}, {"op": "wait_file", "path": str(probe_root(root, host) / "left.json"), "scope": "menu"},
        {"op": "wait", "elapsed_ms": 8000, "scope": "menu"}, signal("checked", "menu"), {"op": "finish"}]}
    return probes


def check_host_leave_live(checks, reads, logs):
    host, ana = NAMES[0], NAMES[1]
    before = reads[host].get("leave-before", {}).get("text", "")
    after = reads[host].get("leave-after", {}).get("text", "")
    checks.check("host-leave-reads-handover-with-two-others", "Another player becomes the host" in before, f"before {before!r}")
    checks.check("host-leave-sentence-follows-a-drop", "The match ends for everyone" in after, f"after the other player left {after!r}")
    took_over = re.search(TOOK_OVER, logs[ana], re.I) is not None
    ended = re.search(HOST_LEFT, logs[ana]) is not None or "host left with no other survivor" in logs[ana]
    checks.check("host-leave-press-does-what-it-says", ended and not took_over, f"{ana}: the match ended {ended}, taken over {took_over}")


def leave_press_race_probes(root):
    """The host presses Leave's confirmation while it promises a handover; a player leaves before the release. The release must
    not do what the screen no longer says: nothing happens, and the sentence says it changed. The next press leaves."""
    host, ana, ben = NAMES[:3]
    steps = [{"op": "wait", "service": "Running", "sim_at_least": 200}, wait_file(probe_root(root, ben) / "at-confirm.json"),
             *keys("Escape"), *on_screen("Pause"), *hand("ButtonLeaveMatch"), *on_screen("PauseLeaveConfirm"),
             read("LabelLeaveConfirm", "menu", "race-shown"), menu("hand_press ButtonLeaveConfirm"), {"op": "wait", "renders": 3},
             signal("host-pressing"), wait_file(probe_root(root, ben) / "left.json"),
             {"op": "wait", "control": "LabelLeaveConfirm", "scope": "menu", "text_contains": "The match ends for everyone"},
             menu("hand_release ButtonLeaveConfirm"), {"op": "wait", "elapsed_ms": 1500, "scope": "menu"},
             read("LabelLeaveConfirm", "menu", "race-after-release"), menu("dump_host_options"), *hand("ButtonLeaveConfirm"),
             {"op": "wait", "service": "Completed", "scope": "menu"}, signal("left", "menu"), {"op": "wait", "elapsed_ms": 12000, "scope": "menu"},
             signal("done", "menu"), {"op": "finish"}]
    probes = {host: {"schema": 1, "timeout_ms": 170000, "steps": steps}}
    probes[ben] = {"schema": 1, "timeout_ms": 170000, "steps": [{"op": "wait", "service": "Running", "sim_at_least": 200}, *at_leave_confirm("at-confirm"),
                                                                wait_file(probe_root(root, host) / "host-pressing.json"), *confirm_leave()]}
    probes[ana] = {"schema": 1, "timeout_ms": 170000, "steps": [
        {"op": "wait", "service": "Running", "sim_at_least": 200}, {"op": "wait_file", "path": str(probe_root(root, host) / "left.json"), "scope": "menu"},
        {"op": "wait", "elapsed_ms": 8000, "scope": "menu"}, signal("checked", "menu"), {"op": "finish"}]}
    return probes


def check_leave_press_race(checks, reads, logs):
    host, ana = NAMES[0], NAMES[1]
    shown = reads[host].get("race-shown", {}).get("text", "")
    after = reads[host].get("race-after-release", {})
    text = after.get("text", "")
    checks.check("leave-race-pressed-on-a-handover", "Another player becomes the host" in shown, f"at the press {shown!r}")
    checks.check("leave-race-release-does-nothing-when-it-changed", after.get("visible") and "The match ends for everyone" in text and LEAVE_CHANGED in text,
                 f"after the release {text!r} visible {after.get('visible')}")
    took_over = re.search(TOOK_OVER, logs[ana], re.I) is not None
    ended = re.search(HOST_LEFT, logs[ana]) is not None or "host left with no other survivor" in logs[ana]
    checks.check("leave-race-next-press-does-what-it-says", ended and not took_over, f"{ana}: the match ended {ended}, taken over {took_over}")


def leave_bad_ticket_probes(root):
    """A client whose kept ticket no longer loads: its leave must not promise the Rejoin Match the landing will not offer."""
    host, client = NAMES[:2]
    client_steps = [{"op": "wait", "service": "Running", "sim_at_least": 200}, signal("ticket-ready"), wait_file(root / "ticket-damaged.json"),
                    *keys("Escape"), *on_screen("Pause"), *hand("ButtonLeaveMatch"), *on_screen("PauseLeaveConfirm"),
                    read("LabelLeaveConfirm", "menu", "bad-ticket-text"), shot("leave-bad-ticket"), *hand("ButtonLeaveConfirm"),
                    {"op": "wait", "service": "Completed", "scope": "menu"}, {"op": "wait", "elapsed_ms": 5000, "scope": "menu"}, menu("dump_host_options"),
                    signal("left", "menu"), {"op": "finish"}]
    steps = [{"op": "wait", "service": "Running", "sim_at_least": 200}, wait_file(probe_root(root, client) / "left.json"), {"op": "wait", "elapsed_ms": 1500},
             *keys("Escape"), *on_screen("Pause"), *end_match()]
    return {host: {"schema": 1, "timeout_ms": 170000, "steps": steps}, client: {"schema": 1, "timeout_ms": 170000, "steps": client_steps}}


def damage_ticket(root, client, alive):
    """Once the client's match runs, its kept ticket stops loading: its last byte, in the record's seal, is flipped."""
    ready, ticket = probe_root(root, client) / "ticket-ready.json", root / f"{client}.ticket"
    deadline = time.monotonic() + 200
    while not ready.exists() and time.monotonic() < deadline and alive():
        time.sleep(0.2)
    damaged = ready.exists() and ticket.exists()
    if damaged:
        data = bytearray(ticket.read_bytes())
        data[-1] ^= 0xFF
        ticket.write_bytes(bytes(data))
    (root / "ticket-damaged.json").write_text(json.dumps({"damaged": damaged}) + "\n", encoding="utf-8")


def check_leave_bad_ticket(checks, reads, captures, root):
    client = NAMES[1]
    marker = root / "ticket-damaged.json"
    damaged = json.loads(marker.read_text(encoding="utf-8")).get("damaged") if marker.exists() else False
    checks.check("bad-ticket-was-kept-and-damaged", damaged, f"the client's ticket existed and was damaged: {damaged}")
    text = reads[client].get("bad-ticket-text", {}).get("text", "")
    promised = "Rejoin Match" in text or "your seat stays yours" in text
    landing = captures[client][-1] if captures[client] else {"screen": "", "controls": []}
    rejoin = control_of(landing, "ButtonMultiplayerReconnect") or {}
    offered = bool(rejoin.get("visible")) and rejoin.get("text") == "Rejoin Match"
    checks.check("bad-ticket-sentence-matches-the-offer", promised == offered and (promised or "join it again" in text),
                 f"confirmation {text!r} promises Rejoin {promised}; the landing ({landing['screen']}) offers it {offered}")


def away_names_probes(root, base):
    """The host removes Ana; a newcomer asks for her opened place; then Ben leaves. The summary names Ben as the one away."""
    host, ana, ben = NAMES[:3]
    steps = [{"op": "wait", "service": "Running", "sim_at_least": 150}, *open_panel(), *find_row(ana, base),
             *click(f"NetworkSeatRemove@{ana}"), *click(f"NetworkSeatRemove@{ana}"), read("NetworkSeatsStatus", tag="removed"),
             {"op": "wait", "elapsed_ms": 1500}, signal("ready-for-newcomer"),
             {"op": "wait", "control": "NetworkSeatsSummary", "text_contains": "1 request to join"}, signal("ben-may-leave"),
             wait_file(probe_root(root, ben) / "left.json"), {"op": "wait", "control": "NetworkSeatsSummary", "text_contains": "is away"},
             {"op": "wait", "renders": 6}, read("NetworkSeatsSummary", tag="away-summary"), shot("away-names"), *close_and_end()]
    probes = {host: {"schema": 1, "timeout_ms": 175000, "steps": steps}}
    # Ana's match ends when the host removes her: her probe is done once she plays.
    probes[ana] = {"schema": 1, "timeout_ms": 175000, "steps": [{"op": "wait", "service": "Running", "sim_at_least": 100}, {"op": "finish"}]}
    probes[ben] = {"schema": 1, "timeout_ms": 175000, "steps": [{"op": "wait", "service": "Running", "sim_at_least": 150},
                                                                wait_file(probe_root(root, host) / "ben-may-leave.json"), *at_leave_confirm("at-confirm"),
                                                                *confirm_leave()]}
    probes[NEWCOMER] = {"schema": 1, "timeout_ms": 175000, "steps": [wait_file(probe_root(root, host) / "done-reading.json"), {"op": "finish"}]}
    return probes


def check_away_names(checks, reads):
    host, ben = NAMES[0], NAMES[2]
    removed = " ".join(reads[host].get("removed", {}).get("text", "").split())
    checks.check("away-names-removal-done", removed == f"{NAMES[1]} was removed from the match.", f"status {removed!r}")
    summary = " ".join(reads[host].get("away-summary", {}).get("text", "").split())
    checks.check("away-names-the-player-away", summary == f"{ben} is away - 1 request to join", f"summary {summary!r}")


def leave_no_ticket_probes(root):
    """A match with authenticated admission off keeps no ticket: the client's leave must not promise Rejoin Match."""
    host, client = NAMES[:2]
    client_steps = [{"op": "wait", "service": "Running", "sim_at_least": 200}, *keys("Escape"), *on_screen("Pause"), *hand("ButtonLeaveMatch"),
                    *on_screen("PauseLeaveConfirm"), read("LabelLeaveConfirm", "menu", "leave-text"), menu("dump_host_options"), shot("leave-no-ticket"),
                    *confirm_leave()]
    steps = [{"op": "wait", "service": "Running", "sim_at_least": 200}, wait_file(probe_root(root, client) / "left.json"), {"op": "wait", "elapsed_ms": 2500},
             *open_panel(), read("NetworkSeatName0", tag="leaver-name"), read("NetworkSeatDetail0", tag="leaver-detail"), shot("leave-no-ticket-host"),
             *close_and_end()]
    return {host: {"schema": 1, "timeout_ms": 170000, "steps": steps}, client: {"schema": 1, "timeout_ms": 170000, "steps": client_steps}}


def check_leave_no_ticket(checks, reads, logs):
    host, client = NAMES[:2]
    text = reads[client].get("leave-text", {}).get("text", "")
    checks.check("leave-no-ticket-promises-no-rejoin", "Rejoin Match" not in text and "your seat stays yours" not in text and "The AI plays your units" in text,
                 f"confirmation {text!r}")
    kept = "(ticket kept)" in logs[client]
    checks.check("leave-no-ticket-keeps-none", not kept, f"the client's log says a ticket was kept: {kept}")
    detail = reads[host].get("leaver-detail", {}).get("text", "")
    checks.check("leave-no-ticket-ai-plays-the-units", "AI in control" in detail, f"the host reads {detail!r}")


def long_names_probes(root):
    """Two players share a 24-character name and a newcomer has one as long: names keep their seat numbers and every first-press
    sentence and disabled reason is whole, never cut."""
    host, ana, ben = NAMES[:3]
    steps = [{"op": "wait", "service": "Running", "sim_at_least": 150}, wait_file(probe_root(root, ben) / "left.json"), {"op": "wait", "elapsed_ms": 2500},
             *open_panel(), {"op": "wait", "control": "NetworkSeatDetail0", "text_contains": "Held"}, {"op": "wait", "renders": 4},
             *roster_reads("long", 3), read("NetworkSeatHint0", tag="long-why"), read("NetworkSeatRemove0", tag="long-remove"),
             read("NetworkSeatBan0", tag="long-ban"), read("NetworkSeatWait0", tag="long-keep"), shot("long-names-held"), signal("ready-for-newcomer"),
             {"op": "wait", "control": "NetworkSeatApplicant0", "text_contains": LONG_NEWCOMER}, {"op": "wait", "renders": 4},
             *click("NetworkSeatApplicant0", item=0), {"op": "wait", "control": "NetworkSeatApplicant0", "equals": {"selected": 0}},
             *click("NetworkSeatSubstitute0"), {"op": "wait", "renders": 4}, read("NetworkSeatsStatus", tag="long-let-armed"),
             read("NetworkSeatSubstitute0", tag="long-let-caption"), shot("long-names-let"), *close_and_end()]
    probes = {host: {"schema": 1, "timeout_ms": 175000, "steps": steps}}
    probes[ben] = {"schema": 1, "timeout_ms": 175000, "steps": [{"op": "wait", "service": "Running", "sim_at_least": 150}, *at_leave_confirm("at-confirm"),
                                                                *confirm_leave()]}
    probes[ana] = {"schema": 1, "timeout_ms": 175000, "steps": plays_on(root, host)}
    probes[NEWCOMER] = {"schema": 1, "timeout_ms": 175000, "steps": [wait_file(probe_root(root, host) / "done-reading.json"), {"op": "finish"}]}
    return probes


def whole(control):
    """A label read whole: shown, and its wrapped text no taller than the label."""
    return bool(control.get("visible")) and control.get("text_height", 0) <= (control.get("rect") or [0, 0, 0, 0])[3]


def check_long_names(checks, reads):
    host = NAMES[0]
    held = f"{LONG_SHARED} (seat 3)"
    names = [reads[host].get(f"long-name{row}", {}) for row in range(2)]
    texts = [row.get("text", "") for row in names]
    checks.check("long-names-keep-the-seat-number", texts[0].endswith("(seat 3)") and texts[1].endswith("(seat 2)") and all(row.get("visible") for row in names),
                 f"rows {texts}")
    for tag in ("long-remove", "long-ban", "long-keep"):
        caption = reads[host].get(tag, {}).get("text", "")
        checks.check(f"{tag}-keeps-the-seat-number", "(seat 3)" in caption, f"caption {caption!r}")
    why = reads[host].get("long-why", {})
    checks.check("long-names-reason-whole", " ".join(why.get("text", "").split()) == f"Nobody has asked for {held}'s place yet" and whole(why),
                 f"reason {why.get('text')!r} height {why.get('text_height')} in {why.get('rect')}")
    status = reads[host].get("long-let-armed", {})
    expected = f"{LONG_NEWCOMER} takes {held}'s place and {held} cannot return to it - press again to confirm"
    checks.check("long-names-let-sentence-whole", " ".join(status.get("text", "").split()) == expected and whole(status),
                 f"status {status.get('text')!r} height {status.get('text_height')} in {status.get('rect')}")


LANDING = "wait 40\nactivate ButtonMainToMultiplayer\nwait 12\nassert_substate Landing\n"


def between_rounds_plan(root, port):
    """A lobby match ends while a player is away: the rematch lobby's players box says the place is held for them."""
    host, client = NAMES[:2]
    host_script = (LANDING + "activate ButtonMultiplayerHostGame\nwait_ms 400\ncombo_select ComboHostActivity P4 Alpha Duel - Base.rte\nwait_ms 400\n"
                   f"settext TextHostPort {port}\nsettext TextHostPlayers 2\nactivate ButtonMultiplayerCreate\nwait_connected 2 60\n"
                   "wait_remote_ready 60\nwait 3\nactivate ButtonMultiplayerStart\n"
                   f"wait_file {probe_root(root, host) / 'lobby.json'} 150\nwait_ms 1500\nscreenshot between_rounds\nwait 3\n"
                   f"assert_roster_text {client}  /  Team 2  /  {HELD_BETWEEN_ROUNDS}\nwait 3\nexit\n")
    client_script = (LANDING + "activate ButtonMultiplayerJoinGame\nwait_ms 400\nsettext TextJoinAddress 127.0.0.1\n"
                     f"settext TextJoinPort {port}\nactivate ButtonMultiplayerConnect\nwait_connected 2 60\nactivate ButtonMultiplayerReady\n"
                     f"wait_file {probe_root(root, host) / 'lobby.json'} 150\nwait_ms 4000\nexit\n")
    # The lobby's ticks count on the launch counter: the round's own frame says the match is on screen.
    host_steps = [{"op": "wait", "service": "Running", "screen": "Gameplay", "lockstep_frame_at_least": 150}, wait_file(probe_root(root, client) / "left.json"),
                  {"op": "wait", "elapsed_ms": 2500}, *keys("Escape"), *on_screen("Pause"), *hand("ButtonEndMatch"), *confirm_end_match(),
                  {"op": "wait", "service": "Starting", "scope": "menu"}, {"op": "wait", "elapsed_ms": 1500, "scope": "menu"}, signal("lobby", "menu"),
                  {"op": "finish"}]
    client_steps = [{"op": "wait", "service": "Running", "screen": "Gameplay", "lockstep_frame_at_least": 150}, *keys("Escape"), *on_screen("Pause"), *hand("ButtonLeaveMatch"),
                    *on_screen("PauseLeaveConfirm"), *confirm_leave()]
    return {"probes": {host: {"schema": 1, "timeout_ms": 175000, "steps": host_steps}, client: {"schema": 1, "timeout_ms": 175000, "steps": client_steps}},
            "menu": {host: host_script, client: client_script}}


def check_between_rounds(checks, logs):
    host, client = NAMES[:2]
    wanted = f"{client}  /  Team 2  /  {HELD_BETWEEN_ROUNDS}"
    line = next((line for line in logs[host].splitlines() if "assert_roster_text" in line), "")
    passed = re.search(r"assert_roster_text .*PASS", line) is not None
    checks.check("between-rounds-held-reads-held", passed and "Disconnected" not in line, f"{wanted!r}: {line[:400]!r}")


def cost_probes(root):
    """Four players: the status box drawn with the panel closed, then the panel open, each long enough for 600 frames and more."""
    host = NAMES[0]
    steps = [{"op": "wait", "service": "Running", "sim_at_least": 150}, {"op": "wait", "elapsed_ms": 25000}, *open_panel(),
             {"op": "wait", "elapsed_ms": 25000}, *close_and_end()]
    probes = {host: {"schema": 1, "timeout_ms": 175000, "steps": steps}}
    for who in NAMES[1:]:
        probes[who] = {"schema": 1, "timeout_ms": 175000, "steps": plays_on(root, host)}
    return probes


# A frame's cost is summed in whole time; parts_median_us is the same frames summed the way a pass rounded down to whole
# microseconds would, which a build that rounds each pass reports as its only number.
COST_LINE = re.compile(r"\[panel-cost\] (closed|open) players=(\d+) frames=(\d+) median_us=([\d.]+) p99_us=([\d.]+) status_shown=(\d)"
                       r"(?:[^\n]*? parts_median_us=(\d+))?")


def check_cost(checks, logs):
    lines = [match.groups() for match in COST_LINE.finditer(logs[NAMES[0]])]
    for kind in ("closed", "open"):
        # The last full window of each kind with all four players in the match and the status box up.
        rows = [row for row in lines if row[0] == kind and row[1] == "4" and int(row[2]) >= 600 and row[5] == "1"]
        row = rows[-1] if rows else None
        median, p99 = (float(row[3]), float(row[4])) if row else (None, None)
        checks.check(f"cost-{kind}-within-budget", median is not None and median <= COST_BUDGET_US[kind],
                     f"median {median} us, p99 {p99} us over {row[2] if row else 0} frames (budget {COST_BUDGET_US[kind]} us)")
        parts = row[6] if row else None
        checks.check(f"cost-{kind}-summed-whole", parts is not None,
                     f"whole median {median} us; summed from passes rounded down {parts if parts is not None else median} us")


def ntdll():
    return ctypes.WinDLL("ntdll")


@spread.managed_case
def run_peers(options, root, case, size, peers, base, moderate=False):
    spread.configure(options)
    root.mkdir(parents=True, exist_ok=False)
    width, height = map(int, size.split("x"))
    port = options.port
    plan = {}
    if case == "pause":
        probes = pause_probes(root, base)
        who_list = list(NAMES[:2])
        ticks = 9000
    elif case == "status":
        probes = status_probes(root, base)
        who_list = list(NAMES[:2])
        ticks = 9000
    elif case == "repair":
        probes = repair_probes(root)
        who_list = list(NAMES[:2])
        ticks = 2400
    elif case == "host-leave":
        probes = host_leave_probes(root, peers)
        who_list = list(NAMES[:peers])
        ticks = 3600
    elif case in NEW_CASES or case == "away-names":
        peers = 3 if case == "away-names" else CASE_PEERS[case]
        who_list = list(NAMES[:peers])
        ticks = 12000
        plan = between_rounds_plan(root, port) if case == "between-rounds" else {}
        probes = plan.get("probes") or {"stale-press": lambda: stale_press_probes(root, base), "reach": lambda: reach_probes(root, base),
                                         "open-place": lambda: open_place_probes(root, base), "host-leave-live": lambda: host_leave_live_probes(root),
                                         "leave-no-ticket": lambda: leave_no_ticket_probes(root), "long-names": lambda: long_names_probes(root),
                                         "cost": lambda: cost_probes(root), "leave-press-race": lambda: leave_press_race_probes(root),
                                         "leave-bad-ticket": lambda: leave_bad_ticket_probes(root), "away-names": lambda: away_names_probes(root, base)}[case]()
    else:
        probes = players_probes(root, peers, base, moderate, options.cancel)
        who_list = list(NAMES[:peers])
        ticks = 12000
    newcomers = [who for who in (NEWCOMER, SECOND) if who in probes]
    names = {who: who for who in who_list + newcomers}
    if case == "long-names":
        names.update({NAMES[1]: LONG_SHARED, NAMES[2]: LONG_SHARED, NEWCOMER: LONG_NEWCOMER})
    if spread.enabled(options) and case == "leave-bad-ticket":
        raise spread.SpreadRefusal("leave-bad-ticket requires a native private-ticket damage lever")
    placement = spread.prepare_case(
        options.repo, root,
        [spread.Peer(who, os="windows" if who in (NAMES[0], NEWCOMER) else "any", engines=1, size=(width, height),
                     reviewed=who == NAMES[0], readback=who == NAMES[0], held=who == NEWCOMER,
                     share_ok=case != "cost", quiet=case == "cost" and who == NAMES[0])
         for who in who_list + newcomers],
        spread.Match(port, parameters={"lane": "in-match", "network": "ice", "directory": getattr(options, "spread_directory", None),
                                       "case": case, "players": peers, "moderate": moderate, "cancel": options.cancel}))
    make_peer_run = placement.make_run if placement else make_run
    runs, records = {}, {}
    menu_done = probe_root(root, NAMES[0]) / "done.json"
    # The screen checks every scene carries: layout, duplicate lines, the held lines, the seat rows.
    watches = root / "screen-watches.txt"
    watches.write_text(SCREEN_WATCHES, encoding="utf-8")
    for who in who_list + newcomers:
        directory = probe_root(root, who)
        directory.mkdir()
        if placement:
            # Remote startup is outside the script; its original waits count the joined round.
            probes[who]["activate_phase"] = "Running"
            probes[who]["sim_clock"] = "lockstep"
        (directory / "probe.json").write_text(json.dumps(probes[who], indent=2) + "\n", encoding="utf-8")
        script = root / f"{who}-menu.txt"
        lobby = case == "between-rounds"
        script.write_text(plan["menu"][who] if lobby else f"wait_file {menu_done} 300\nwait_ms 4000\nexit\n", encoding="utf-8")
        # A newcomer asks for whichever place is free to give: the order the others joined in decides who holds which seat.
        extra = ["-net-h4-apply", APPLY_SEATS.get(case, {}).get(who, "65535")] if who in newcomers else []
        # The command-line match reloads a live snapshot only with this lever, as the readback's repair case runs it.
        if case == "repair":
            extra = [*extra, "-net-match-e2e-resync"]
        # A match with authenticated admission off keeps no ticket for a leave.
        if case == "leave-no-ticket":
            extra = [*extra, "-net-no-reconnect-admission", "1"]
        # The client's ticket where the case can damage it.
        if case == "leave-bad-ticket" and who == NAMES[1]:
            extra = [*extra, "-net-reconnect-ticket", str(root / f"{who}.ticket")]
        match_peers = peers if case in ("players", "host-leave", "away-names", *NEW_CASES) else 2
        args = ["-menu-script", str(script)] if lobby else ["-menu-script", str(script), *match_args(port, match_peers, who if who not in newcomers else "joiner", ticks, extra, name=names[who])]
        diagnostics = "1" if case == "status" and options.diagnostics else "0"
        env = {"CCCP_HEADLESS": "1", "CC_TEST_NET_UI_SCRIPT": str(directory / "probe.json"), "CCCP_TEST_SCREEN_WATCHES": str(watches)}
        if case == "cost" and who == NAMES[0]:
            env["CC_TEST_PANEL_COST"] = "1"
            env["CCCP_TEST_DRAW_PHASES"] = "1"
        runs[who] = make_peer_run(options.repo, args, root / who, 420, env=env)
        set_visual_resolution(runs[who], width, height)
        seed_settings(runs[who].cwd / "Userdata/Settings.ini", {"NetworkDisplayName": names[who], "NetworkMatchStatusMode": "Always",
                                                                 "NetworkShowDiagnostics": diagnostics, "NetworkIceEnable": "1" if placement else "0"})
        if placement:
            seed_settings(runs[who].cwd / "Userdata/Settings.ini", {"NetworkConnectionMode": "Automatic", "NetworkHostRelayMode": "Directory",
                                                                      "NetworkStunServers": "stun.l.google.com:19302,stun.cloudflare.com:3478,stun.nextcloud.com:443",
                                                                      "NetworkPortMapEnable": "0", "SessionDirectoryInstallKey": f"in-match-{who}-install"})

    def drive(who):
        try:
            records[who] = runs[who].start().finish()
            if not placement:
                records[who]["topology"] = spread.TOPOLOGY_LOCAL
                spread.write_json(runs[who].out / "record.json", records[who])
        except Exception as error:  # the verdict names it
            records[who] = {"error": repr(error), "topology": "spread" if placement else spread.TOPOLOGY_LOCAL}

    threads = {}
    for index, who in enumerate(who_list):
        threads[who] = threading.Thread(target=drive, args=(who,))
        threads[who].start()
        if who == NAMES[0]:
            time.sleep(2.0)
        elif index + 1 < len(who_list):
            # Seats follow the order the clients reach the host: one at a time, so every size plays the same teams (the
            # AI that keeps a held place can win the duel for its team before the case ends).
            log = runs[who].out / "stdout.log"
            deadline = time.monotonic() + 90
            while time.monotonic() < deadline and threads[who].is_alive():
                if log.exists() and "[net-route] RouteAllowed" in log.read_text(encoding="utf-8", errors="replace"):
                    break
                time.sleep(0.2)
            time.sleep(1.0)
    if newcomers:
        # The newcomer asks for the held place once the host has kept it; frozen once the host has read the request so the
        # approval stays open long enough for Cancel, then thawed to take the place.
        ready = probe_root(root, NAMES[0]) / "ready-for-newcomer.json"
        deadline = time.monotonic() + 200
        while not ready.exists() and time.monotonic() < deadline and threads[NAMES[0]].is_alive():
            time.sleep(0.2)
        for who in newcomers[1:] if ready.exists() else ():
            threads[who] = threading.Thread(target=drive, args=(who,))
            threads[who].start()
        if ready.exists():
            threads[NEWCOMER] = threading.Thread(target=drive, args=(NEWCOMER,))
            threads[NEWCOMER].start()
            seen = probe_root(root, NAMES[0]) / "request-seen.json"
            while not seen.exists() and time.monotonic() < deadline and threads[NAMES[0]].is_alive():
                time.sleep(0.1)
            if seen.exists() and runs[NEWCOMER].process is not None:
                if placement:
                    runs[NEWCOMER].suspend()
                else:
                    ntdll().NtSuspendProcess(ctypes.c_void_p(runs[NEWCOMER].process))
                (root / "newcomer-frozen.json").write_text("{}\n", encoding="utf-8")
                cancelled = probe_root(root, NAMES[0]) / "approval-cancelled.json"
                while not cancelled.exists() and time.monotonic() < deadline and threads[NAMES[0]].is_alive():
                    time.sleep(0.1)
                if placement:
                    runs[NEWCOMER].resume()
                else:
                    ntdll().NtResumeProcess(ctypes.c_void_p(runs[NEWCOMER].process))
                (root / "newcomer-thawed.json").write_text("{}\n", encoding="utf-8")
    if case == "leave-bad-ticket":
        damage_ticket(root, NAMES[1], threads[NAMES[1]].is_alive)
    for thread in threads.values():
        thread.join()
    for run in runs.values():
        run.close()
    logs = {who: "\n".join((runs[who].out / leaf).read_text(encoding="utf-8", errors="replace")
                           for leaf in ("stdout.log", "stderr.log") if (runs[who].out / leaf).exists()) for who in runs}
    results = {}
    for who in runs:
        path = probe_root(root, who) / "net-ui-result.json"
        results[who] = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {"error": "no probe result"}
    captures = {who: dumps(runs[who].cwd) for who in runs}
    pictures = root / "pictures"
    pictures.mkdir()
    for who in runs:
        for png in sorted((runs[who].cwd / "ScreenShots").glob("*.png")):
            if not png.name.startswith("dump_"):
                shutil.copy2(png, pictures / f"{who.lower()}-{png.name}")
    checks = Checks()
    for who in runs:
        result = results[who]
        # The command-line match has no menu to return to: a client whose match ended with the host's leave stops its probe there.
        ended_with_host = (case in ("host-leave", "host-leave-live", "leave-press-race") and who != NAMES[0] and
                           (re.search(HOST_LEFT, logs[who]) is not None or "host left with no other survivor" in logs[who])
                           and re.search(TOOK_OVER, logs[who], re.I) is None)
        # A newcomer whose approval the host cancelled is turned away before it plays: cancelled-newcomer-is-told reads it.
        turned_away = case == "players" and options.cancel and who == NEWCOMER and "the host withdrew this substitution" in logs[who]
        # A newcomer the case only lets ask is still asking when the host ends the match; its log says so.
        still_asking = case in ASK_ONLY and who in (NEWCOMER, SECOND) and "the host left while this player was applying for a seat" in logs[who]
        checks.check(f"probe-{who}-complete", (result.get("pass") and result.get("complete")) or ended_with_host or turned_away or still_asking,
                     f"error {result.get('error')!r} at step {len(result.get('steps', []))}" + (" - its match ended with the host's leave" if ended_with_host else "")
                     + (" - turned away by the host's cancel" if turned_away else "") + (" - still asking when the host ended the match" if still_asking else ""))
    reads = {who: tagged_reads(probes[who], results[who]) for who in runs}
    for who in runs:
        if (case == "players" and options.cancel and who == NEWCOMER) or (case in ASK_ONLY and who in (NEWCOMER, SECOND)):
            continue
        armed = re.findall(r"^\[text-watch\] armed (.*)$", logs[who], re.M)
        offences = re.findall(r"^\[text-watch\] violation (\S+) (.*)$", logs[who], re.M)
        checks.check(f"screen-watches-{who}", bool(armed) and not offences,
                     f"{len(armed)} armed; offences {[(name, detail[:240]) for name, detail in offences[:4]]}")
    if case == "pause":
        check_pause(checks, captures, reads, logs, {who: runs[who].cwd for who in runs})
    elif case == "status":
        check_status(checks, reads, size, options.diagnostics)
    elif case == "repair":
        check_repair(checks, reads, logs)
    elif case == "host-leave":
        check_host_leave(checks, captures, logs, peers)
    elif case == "stale-press":
        check_stale_press(checks, reads, logs)
    elif case == "reach":
        check_reach(checks, reads)
    elif case == "open-place":
        check_open_place(checks, reads, logs)
    elif case == "host-leave-live":
        check_host_leave_live(checks, reads, logs)
    elif case == "leave-no-ticket":
        check_leave_no_ticket(checks, reads, logs)
    elif case == "long-names":
        check_long_names(checks, reads)
    elif case == "between-rounds":
        check_between_rounds(checks, logs)
    elif case == "cost":
        check_cost(checks, logs)
    elif case == "leave-press-race":
        check_leave_press_race(checks, reads, logs)
    elif case == "leave-bad-ticket":
        check_leave_bad_ticket(checks, reads, captures, root)
    elif case == "away-names":
        check_away_names(checks, reads)
    else:
        check_players(checks, reads, peers, logs, base, moderate, options.cancel)
    row = {"case": case, "size": size, "peers": peers, "moderate": moderate, "base": base, "checks": checks.rows,
           "pass": all(row["pass"] for row in checks.rows), "pictures": sorted(str(path) for path in pictures.glob("*.png")),
           "records": {who: {key: record.get(key) for key in ("exit_code", "timed_out", "error")} for who, record in records.items()},
           "topology": spread.TOPOLOGY_LOCAL, "proof": False}
    if placement:
        receipt = placement.result()
        row.update(topology="spread", peer_boxes=receipt["peer_boxes"], identities=receipt["identities"],
                   executable_hashes=receipt["executable_hashes"], refusals=receipt["refusals"], sharing=receipt["sharing"])
        row["proof"] = (row["pass"] and set(receipt["identities"]) == set(runs) and
                        all(records.get(who, {}).get("box") == receipt["peer_boxes"][who] and
                            records[who].get("exe_sha256") == receipt["executable_hashes"][who] for who in runs))
    return row


def run_sp_pause(options, root, size):
    root.mkdir(parents=True, exist_ok=False)
    width, height = map(int, size.split("x"))
    directory = probe_root(root, "sp")
    directory.mkdir()
    (directory / "probe.json").write_text(json.dumps(sp_pause_probe(), indent=2) + "\n", encoding="utf-8")
    script = root / "sp-menu.txt"
    script.write_text(SP_MENU.format(done=directory / "done.json"), encoding="utf-8")
    run = make_run(options.repo, ["-menu-script", str(script)], root / "sp", 240,
                   env={"CCCP_HEADLESS": "1", "CC_TEST_NET_UI_SCRIPT": str(directory / "probe.json")})
    set_visual_resolution(run, width, height)
    try:
        record = run.start().finish()
    finally:
        run.close()
    captured = dumps(run.cwd)
    menu_rows = []
    if captured:
        for control in captured[-1]["controls"]:
            menu_rows.append({key: control.get(key) for key in ("name", "rect", "text", "enabled", "visible", "parent")})
    pictures = root / "pictures"
    pictures.mkdir()
    for png in sorted((run.cwd / "ScreenShots").glob("*.png")):
        shutil.copy2(png, pictures / png.name)
    checks = Checks()
    checks.check("sp-pause-captured", bool(captured) and captured[-1]["screen"] == "Pause", f"{len(captured)} dumps")
    snapshot = {"size": size, "screen": captured[-1]["screen"] if captured else None, "controls": menu_rows}
    (root / "sp-pause.json").write_text(json.dumps(snapshot, indent=2) + "\n", encoding="utf-8")
    if options.compare:
        earlier = json.loads(Path(options.compare).read_text(encoding="utf-8"))
        before = next((row for row in earlier.get("sp_pause", []) if row["size"] == size), None)
        same = before is not None and before["controls"] == menu_rows
        diff = []
        if before is not None and not same:
            old = {row["name"]: row for row in before["controls"]}
            new = {row["name"]: row for row in menu_rows}
            diff = [name for name in sorted(set(old) | set(new)) if old.get(name) != new.get(name)]
        checks.check("sp-pause-unchanged", same, "every row, its order, rect and text equal the earlier build's" if same else f"differs: {diff}")
    return {"case": "sp-pause", "size": size, "checks": checks.rows, "pass": all(row["pass"] for row in checks.rows), "snapshot": snapshot,
            "pictures": sorted(str(path) for path in pictures.glob("*.png")), "record": {key: record.get(key) for key in ("exit_code", "timed_out")}}


def every_probe(root, base):
    """Every case's probes, as the runner builds them."""
    plans = {"pause": pause_probes(root, base), "players": players_probes(root, 3, base, True),
             "players-cancel": players_probes(root, 3, base, True, cancel=True), "status": status_probes(root, base),
             "repair": repair_probes(root), "host-leave": host_leave_probes(root, 3), "sp-pause": {"sp": sp_pause_probe()},
             "stale-press": stale_press_probes(root, base), "reach": reach_probes(root, base), "open-place": open_place_probes(root, base),
             "host-leave-live": host_leave_live_probes(root), "leave-no-ticket": leave_no_ticket_probes(root),
             "long-names": long_names_probes(root), "between-rounds": between_rounds_plan(root, 46610)["probes"], "cost": cost_probes(root)}
    return plans


def hand_shaped(steps):
    """Each press and its release are separate steps with a wait between them, never one step or one frame."""
    pressed = {}
    for index, step in enumerate(steps):
        op, command = step.get("op"), step.get("command", "")
        if op == "mouse_down":
            pressed[step["control"]] = index
        elif op == "mouse_up":
            down = pressed.pop(step["control"], None)
            # A release may land on another control (a press dragged off); its own press is the last one still down.
            if down is None and pressed:
                down = pressed.pop(next(reversed(pressed)))
            if down is None or not any(between.get("op") == "wait" for between in steps[down + 1:index]):
                return False
        elif op == "menu" and command.startswith("hand_press "):
            pressed[command.split()[1]] = index
        elif op == "menu" and command.startswith("hand_release "):
            down = pressed.pop(command.split()[1], None)
            if down is None or not any(between.get("op") == "wait" for between in steps[down + 1:index]):
                return False
    return not pressed


def self_test():
    """The driver's own rows: no engine, no files written."""
    rows = []

    def row(name, ok):
        rows.append(bool(ok))
        print(f"[in-match-ux-self-test] {'PASS' if ok else 'FAIL'} {name}")

    root = Path("self-test")
    for base in (False, True):
        for case, probes in every_probe(root, base).items():
            for who, probe in probes.items():
                steps = probe["steps"]
                label = f"{case}{'-base' if base else ''}/{who}"
                row(f"{label}-ends-with-finish", steps and steps[-1].get("op") == "finish")
                row(f"{label}-presses-like-a-hand", hand_shaped(steps))
    row("every-new-case-has-its-players", set(CASE_PEERS) == set(NEW_CASES))
    row("a-same-step-click-is-not-hand-shaped", not hand_shaped([{"op": "mouse_down", "control": "X"}, {"op": "mouse_up", "control": "X"}]))
    row("a-press-never-released-is-not-hand-shaped", not hand_shaped([{"op": "mouse_down", "control": "X"}, {"op": "wait", "renders": 3}]))
    # The cost check reads the last full window of each kind with four players and the status box up.
    log = ("[panel-cost] closed players=1 frames=600 median_us=900.0 p99_us=990.0 status_shown=0 parts_median_us=850\n"
           "[panel-cost] closed players=4 frames=600 median_us=80.4 p99_us=150.2 status_shown=1 update_median_us=30.1 updates=9 parts_median_us=61\n"
           "[panel-cost] open players=4 frames=600 median_us=620 p99_us=900 status_shown=1\n")
    checks = Checks()
    check_cost(checks, {NAMES[0]: log})
    verdicts = {row_["check"]: row_["pass"] for row_ in checks.rows}
    row("cost-reads-the-four-player-windows", verdicts == {"cost-closed-within-budget": True, "cost-closed-summed-whole": True,
                                                           "cost-open-within-budget": False, "cost-open-summed-whole": False})
    # The stale press: nobody acted on is green; the pressed player removed or named is red.
    host, ana, ben, cleo = NAMES
    reads = {host: {"a-slot-before": {"text": ben}, "a-slot-caption": {"text": f"Remove {ana}"}, "a-status": {"text": LIST_CHANGED},
                    "b-slot-caption": {"text": f"Remove {cleo}"}, "b-status": {"text": f"{cleo} loses the place and the AI keeps playing it - press again to remove"}}}
    logs = {who: "" for who in NAMES}
    checks = Checks()
    check_stale_press(checks, reads, logs)
    row("stale-press-green-when-nobody-is-acted-on", all(row_["pass"] for row_ in checks.rows))
    reads[host]["a-status"] = {"text": f"{ben} was removed from the match."}
    logs[ben] = "removed from this session"
    checks = Checks()
    check_stale_press(checks, reads, logs)
    row("stale-press-red-when-the-pressed-player-is-removed", not all(row_["pass"] for row_ in checks.rows))
    # The leave pressed as a handover: the release that found it changed does nothing and says so; one that leaves is red.
    changed = f"Leave the match?\nThe match ends for everyone.\nWhat leaving does changed as you pressed, {LEAVE_CHANGED}."
    reads = {host: {"race-shown": {"text": "Leave the match?\nAnother player becomes the host and the match plays on."},
                    "race-after-release": {"text": changed, "visible": True}}}
    logs = {who: "" for who in NAMES}
    logs[ana] = "Host left the match at frame 900"
    checks = Checks()
    check_leave_press_race(checks, reads, logs)
    row("leave-race-green-when-the-release-waits", all(row_["pass"] for row_ in checks.rows))
    reads[host].pop("race-after-release")
    checks = Checks()
    check_leave_press_race(checks, reads, logs)
    row("leave-race-red-when-the-release-leaves", not all(row_["pass"] for row_ in checks.rows))
    # The client's sentence against the landing: a promise the landing does not keep is red.
    with tempfile.TemporaryDirectory() as folder:
        root = Path(folder)
        (root / "ticket-damaged.json").write_text('{"damaged": true}', encoding="utf-8")
        landing = {"screen": "MainScreen", "controls": []}
        honest = "Leave the match?\nThe AI plays your units for the rest of the match.\nTo come back, join it again from the Multiplayer screen and ask the host for a place."
        checks = Checks()
        check_leave_bad_ticket(checks, {ana: {"bad-ticket-text": {"text": honest}}}, {ana: [landing]}, root)
        row("bad-ticket-green-when-no-rejoin-is-promised-or-offered", all(row_["pass"] for row_ in checks.rows))
        promise = "Leave the match?\nThe AI plays your units and your seat stays yours.\nRejoin Match on the Multiplayer screen brings you back while the match runs."
        checks = Checks()
        check_leave_bad_ticket(checks, {ana: {"bad-ticket-text": {"text": promise}}}, {ana: [landing]}, root)
        row("bad-ticket-red-when-rejoin-is-promised-and-not-offered", not all(row_["pass"] for row_ in checks.rows))
    # The away summary names the player away, never the opened place before it.
    reads = {host: {"removed": {"text": f"{ana} was removed from the match."}, "away-summary": {"text": f"{ben} is away - 1 request to join"}}}
    checks = Checks()
    check_away_names(checks, reads)
    row("away-names-green-when-the-player-away-is-named", all(row_["pass"] for row_ in checks.rows))
    reads[host]["away-summary"] = {"text": "Open place (seat 2) is away - 1 request to join"}
    checks = Checks()
    check_away_names(checks, reads)
    row("away-names-red-when-the-opened-place-is-named", not all(row_["pass"] for row_ in checks.rows))
    print(f"[in-match-ux-self-test] {'PASS' if all(rows) else 'FAIL'} {sum(rows)}/{len(rows)}")
    return 0 if all(rows) else 1


def main():
    if "--self-test" in sys.argv[1:]:
        return self_test()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--case", choices=("pause", "players", "status", "repair", "host-leave", "sp-pause", *NEW_CASES, "away-names", "all"), required=True)
    parser.add_argument("--diagnostics", action="store_true", help="the status case with detailed network statistics on")
    parser.add_argument("--size", choices=SIZES, default="960x540")
    parser.add_argument("--all-sizes", action="store_true")
    parser.add_argument("--peers", type=int, choices=(2, 3, 4), default=3)
    parser.add_argument("--moderate", action="store_true", help="the host keeps, gives away, removes and bans places by hand")
    parser.add_argument("--cancel", action="store_true", help="with --moderate: the host cancels the approval instead")
    parser.add_argument("--base", action="store_true", help="drive a build from before this menu work")
    parser.add_argument("--compare", type=Path, help="an earlier sp-pause result.json to hold the single-player menu against")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--port-block", help="lane-owned game ports LO-HI for spread mode")
    parser.add_argument("--dry-run", action="store_true", help="print each probe's step count and stop")
    parser.add_argument("--spread-tools", type=Path, help="the canonical shared tools directory for an explicitly routed spread run")
    spread.add_arguments(parser)
    options = parser.parse_args()
    spread.configure(options)
    if options.case == "away-names" and not spread.enabled(options):
        parser.error("away-names requires spread mode")
    if options.dry_run:
        root = Path("dry")
        plans = {"pause": pause_probes(root, options.base), "players": players_probes(root, options.peers, options.base, options.moderate)}
        print(json.dumps({case: {who: len(probe["steps"]) for who, probe in probes.items()} for case, probes in plans.items()}))
        return 0
    if options.port_block:
        if not spread.enabled(options):
            parser.error("--port-block requires spread mode")
        count = len(SIZES if options.all_sizes else (options.size,)) * (6 + len(NEW_CASES) if options.case == "all" else 1)
        try:
            spread.check_port_block(options.port, options.port_block, count)
        except ValueError as error:
            parser.error(str(error))
    elif not 1024 < options.port < 50000:
        parser.error("take the port from the lane's own block below 50000")
    options.repo = options.repo.resolve()
    options.out.mkdir(parents=True, exist_ok=False)
    sizes = SIZES if options.all_sizes else (options.size,)
    rows = []
    for size in sizes:
        cases = ("pause", "players", "status", "repair", "host-leave", "sp-pause", *NEW_CASES) if options.case == "all" else (options.case,)
        for case in cases:
            if case == "sp-pause":
                rows.append(run_sp_pause(options, options.out / f"sp-pause-{size}", size))
            else:
                peers = 3 if case == "away-names" else CASE_PEERS.get(case, options.peers if case in ("players", "host-leave") else 2)
                root = options.out / f"{case}-{size}-{peers}p{'-diag' if case == 'status' and options.diagnostics else ''}"
                try:
                    directory_context = contextlib.nullcontext(None)
                    if spread.enabled(options):
                        from e2e.directory import serve
                        directory_context = serve(options.out / f"directory-{case}-{size}", options.port + 1,
                                                  block=(options.port + 1, options.port + 1))
                    with directory_context as directory:
                        options.spread_directory = directory
                        rows.append(run_peers(options, root, case, size, options.peers, options.base, options.moderate and case == "players"))
                except spread.SpreadRefusal as error:
                    if not spread.enabled(options):
                        raise
                    receipt = spread.read_json(root / "spread-result.json", {})
                    rows.append({"case": case, "size": size, "peers": peers, "base": options.base, "pass": False,
                                 "checks": [], "records": receipt.get("records", {}), "pictures": [], "topology": "spread", "proof": False,
                                 "peer_boxes": receipt.get("peer_boxes", {}), "error": str(error), "status": "HARNESS BLOCKED",
                                 "refusals": receipt.get("refusals", []), "refused_peer": receipt.get("refused_peer"),
                                 "refused_box": receipt.get("refused_box"), "reason": receipt.get("reason", str(error))})
            options.port += 1
    result = {"pass": all(row["pass"] for row in rows), "revision": subprocess.check_output(["git", "-C", str(options.repo), "rev-parse", "HEAD"], text=True).strip(),
              "exe_sha256": sha(engine_executable(options.repo)), "driver_sha256": sha(__file__), "base": options.base, "rows": rows,
              "sp_pause": [row["snapshot"] for row in rows if row["case"] == "sp-pause"]}
    multiplayer = [row for row in rows if row["case"] != "sp-pause"]
    if multiplayer:
        result.update(topology="spread" if all(row["topology"] == "spread" for row in multiplayer) else spread.TOPOLOGY_LOCAL,
                      proof=all(row["proof"] for row in multiplayer),
                      peer_boxes={f"{row['case']}/{row['size']}": row.get("peer_boxes", {}) for row in multiplayer})
    (options.out / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    red = [f"{row['case']}/{row['size']}: {check['check']}" for row in rows for check in row["checks"] if not check["pass"]]
    red += [f"{row['case']}/{row['size']}: {row['error']}" for row in rows if row.get("error")]
    print(f"[in-match-ux] {'PASS' if result['pass'] else 'FAIL'} {options.out / 'result.json'}" + (f" red: {red}" if red else ""))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
