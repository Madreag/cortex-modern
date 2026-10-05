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
import ctypes
import json
import re
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_sim_test import make_run, engine_executable, file_sha256  # noqa: E402
from test_telemetry_bundle import set_visual_resolution  # noqa: E402
from e2e_video import SCREEN_WATCHES  # noqa: E402

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
CLIENT_SUMMARY = "Only the host can keep, give away or remove a player's place"
RULES_SUMMARY = "Rules for this round"


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


def end_match():
    """The host's End match by hand, its last steps: a match launched from the command line has no lobby to land in, so
    neither loop steps the probe once the round has ended; the run's menu script gives the end its few frames."""
    return [menu("hand_press ButtonEndMatch"), {"op": "wait", "renders": 3}, menu("hand_release ButtonEndMatch"), signal("done"),
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


def players_probes(root, peers, base, moderate):
    """The Players panel on every peer; with moderate the host keeps, gives away and removes places by hand."""
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
                      *click("NetworkSeatApplicant0", item=0), read("NetworkSeatSubstitute0", tag="let-caption"),
                      signal("request-seen"), wait_file(root / "newcomer-frozen.json"),
                      *click("NetworkSeatSubstitute0"), read("NetworkSeatSubstitute0", tag="let-armed"), read("NetworkSeatsStatus", tag="let-armed-status"),
                      shot("players-host-let-armed"), *click("NetworkSeatSubstitute0"), read("NetworkSeatsStatus", tag="let-status"),
                      {"op": "wait", "control": "NetworkSeatCancel0", "equals": {"enabled": True}}, read("NetworkSeatCancel0", tag="cancel-caption"),
                      *click("NetworkSeatCancel0"), read("NetworkSeatsStatus", tag="cancel-status"), signal("approval-cancelled"),
                      wait_file(root / "newcomer-thawed.json"), {"op": "wait", "control": "NetworkSeatSubstitute0", "equals": {"enabled": True}},
                      *click("NetworkSeatSubstitute0"), *click("NetworkSeatSubstitute0"), read("NetworkSeatsStatus", tag="let-again-status"),
                      wait_file(probe_root(root, NEWCOMER) / "seated.json"), {"op": "wait", "elapsed_ms": 1500}, {"op": "wait", "renders": 6},
                      *roster_reads("host-after-join", 3), shot("players-host-after-join")]
            if peers >= 3:
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
             *[{"op": "wait_file", "path": str(probe_root(root, client) / "checked.json"), "scope": "menu"} for client in clients],
             signal("done", "menu"), {"op": "finish"}]
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
    started = all("[net-match] resync: match relaunched from the snapshot" in logs[who] for who in (host, client))
    checks.check("repair-second-press-repairs", started, f"the client reloaded the host's snapshot: {started}")


def check_host_leave(checks, captures, logs, peers):
    host, clients = NAMES[0], list(NAMES[1:peers])
    confirm = next((capture for capture in captures[host] if capture["screen"] == "PauseLeaveConfirm"), None)
    label = control_of(confirm, "LabelLeaveConfirm") if confirm else None
    text = label["text"] if label else ""
    plays_on = "Another player becomes the host" in text
    ends = "The match ends for everyone" in text
    took_over = [client for client in clients if re.search(r"now hosting|hosting the match alone|Handover complete", logs[client], re.I)]
    ended = [client for client in clients if re.search(r"the match is over for this seat|PeerLeft:The host left the match", logs[client])]
    checks.check("leave-text-host-true", (plays_on and took_over and not ended) or (ends and not took_over),
                 f"confirmation {text!r}; a client took the match over: {took_over}; the match ended for: {ended}")


def sp_pause_probe():
    # Single player's pause menu runs in the menu loop, which steps only menu-scope steps.
    return {"schema": 1, "timeout_ms": 120000, "steps": [
        {"op": "wait", "screen": "Gameplay", "sim_at_least": 120}, *keys("Escape"), {"op": "wait", "screen": "Pause", "scope": "menu"},
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


def check_players(checks, reads, peers, logs, base, moderate):
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
    checks.check("held-row-says-why-actions-are-off", f"Nobody has asked for {leaver}'s place yet" in hint.get("text", ""), f"hint {hint.get('text')!r}")
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
    cancel = reads[host].get("cancel-caption", {})
    checks.check("cancel-is-offered-while-pending", cancel.get("text") == "Cancel this approval" and cancel.get("enabled"), f"{cancel.get('text')!r}")
    cancelled = reads[host].get("cancel-status", {})
    checks.check("cancel-operates", f"{NEWCOMER} will not join" in cancelled.get("text", ""), f"{cancelled.get('text')!r}")
    again = reads[host].get("let-again-status", {})
    checks.check("let-operates", f"{NEWCOMER} is joining in {leaver}'s place" in again.get("text", ""), f"{again.get('text')!r}")
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


def ntdll():
    return ctypes.WinDLL("ntdll")


def run_peers(options, root, case, size, peers, base, moderate=False):
    root.mkdir(parents=True, exist_ok=False)
    width, height = map(int, size.split("x"))
    port = options.port
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
    else:
        probes = players_probes(root, peers, base, moderate)
        who_list = list(NAMES[:peers])
        ticks = 12000
    runs, records = {}, {}
    menu_done = probe_root(root, NAMES[0]) / "done.json"
    # The screen checks every scene carries: layout, duplicate lines, the held lines, the seat rows.
    watches = root / "screen-watches.txt"
    watches.write_text(SCREEN_WATCHES, encoding="utf-8")
    for who in who_list + ([NEWCOMER] if NEWCOMER in probes else []):
        directory = probe_root(root, who)
        directory.mkdir()
        (directory / "probe.json").write_text(json.dumps(probes[who], indent=2) + "\n", encoding="utf-8")
        script = root / f"{who}-menu.txt"
        script.write_text(f"wait_file {menu_done} 300\nwait_ms 4000\nexit\n", encoding="utf-8")
        # The newcomer asks for whichever place is free to give: the order the others joined in decides who holds which seat.
        extra = ["-net-h4-apply", "65535"] if who == NEWCOMER else []
        # The command-line match reloads a live snapshot only with this lever, as the readback's repair case runs it.
        if case == "repair":
            extra = [*extra, "-net-match-e2e-resync"]
        args = ["-menu-script", str(script), *match_args(port, peers if case in ("players", "host-leave") else 2, who if who != NEWCOMER else "joiner", ticks, extra, name=who)]
        diagnostics = "1" if case == "status" and options.diagnostics else "0"
        env = {"CCCP_HEADLESS": "1", "CC_TEST_NET_UI_SCRIPT": str(directory / "probe.json"), "CCCP_TEST_SCREEN_WATCHES": str(watches)}
        runs[who] = make_run(options.repo, args, root / who, 420, env=env)
        set_visual_resolution(runs[who], width, height)
        seed_settings(runs[who].cwd / "Userdata/Settings.ini", {"NetworkDisplayName": who, "NetworkMatchStatusMode": "Always",
                                                                 "NetworkShowDiagnostics": diagnostics, "NetworkIceEnable": "0"})

    def drive(who):
        try:
            records[who] = runs[who].start().finish()
        except Exception as error:  # the verdict names it
            records[who] = {"error": repr(error)}

    threads = {}
    for who in who_list:
        threads[who] = threading.Thread(target=drive, args=(who,))
        threads[who].start()
        if who == NAMES[0]:
            time.sleep(2.0)
    if NEWCOMER in runs:
        # The newcomer asks for the held place once the host has kept it; frozen once the host has read the request so the
        # approval stays open long enough for Cancel, then thawed to take the place.
        ready = probe_root(root, NAMES[0]) / "ready-for-newcomer.json"
        deadline = time.monotonic() + 200
        while not ready.exists() and time.monotonic() < deadline and threads[NAMES[0]].is_alive():
            time.sleep(0.2)
        if ready.exists():
            threads[NEWCOMER] = threading.Thread(target=drive, args=(NEWCOMER,))
            threads[NEWCOMER].start()
            seen = probe_root(root, NAMES[0]) / "request-seen.json"
            while not seen.exists() and time.monotonic() < deadline and threads[NAMES[0]].is_alive():
                time.sleep(0.1)
            if seen.exists() and runs[NEWCOMER].process is not None:
                ntdll().NtSuspendProcess(ctypes.c_void_p(runs[NEWCOMER].process))
                (root / "newcomer-frozen.json").write_text("{}\n", encoding="utf-8")
                cancelled = probe_root(root, NAMES[0]) / "approval-cancelled.json"
                while not cancelled.exists() and time.monotonic() < deadline and threads[NAMES[0]].is_alive():
                    time.sleep(0.1)
                ntdll().NtResumeProcess(ctypes.c_void_p(runs[NEWCOMER].process))
                (root / "newcomer-thawed.json").write_text("{}\n", encoding="utf-8")
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
        checks.check(f"probe-{who}-complete", result.get("pass") and result.get("complete"),
                     f"error {result.get('error')!r} at step {len(result.get('steps', []))}")
    reads = {who: tagged_reads(probes[who], results[who]) for who in runs}
    for who in runs:
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
    else:
        check_players(checks, reads, peers, logs, base, moderate)
    return {"case": case, "size": size, "peers": peers, "moderate": moderate, "base": base, "checks": checks.rows,
            "pass": all(row["pass"] for row in checks.rows), "pictures": sorted(str(path) for path in pictures.glob("*.png")),
            "records": {who: {key: record.get(key) for key in ("exit_code", "timed_out", "error")} for who, record in records.items()}}


def run_sp_pause(options, root, size):
    root.mkdir(parents=True, exist_ok=False)
    width, height = map(int, size.split("x"))
    directory = probe_root(root, "sp")
    directory.mkdir()
    (directory / "probe.json").write_text(json.dumps(sp_pause_probe(), indent=2) + "\n", encoding="utf-8")
    script = root / "sp-menu.txt"
    script.write_text(SP_MENU.format(done=directory / "done.json"), encoding="utf-8")
    # Launched as the single-player smoke scene launches its skirmish (tools/e2e/sp-smoke.json), whose Escape opens this menu.
    play_input = options.repo / "tools/e2e/play-input.txt"
    run = make_run(options.repo, ["-input-script", str(play_input), "-menu-script", str(script)], root / "sp", 240,
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--case", choices=("pause", "players", "status", "repair", "host-leave", "sp-pause", "all"), required=True)
    parser.add_argument("--diagnostics", action="store_true", help="the status case with detailed network statistics on")
    parser.add_argument("--size", choices=SIZES, default="960x540")
    parser.add_argument("--all-sizes", action="store_true")
    parser.add_argument("--peers", type=int, choices=(2, 3, 4), default=3)
    parser.add_argument("--moderate", action="store_true", help="the host keeps, gives away, removes and bans places by hand")
    parser.add_argument("--base", action="store_true", help="drive a build from before this menu work")
    parser.add_argument("--compare", type=Path, help="an earlier sp-pause result.json to hold the single-player menu against")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--dry-run", action="store_true", help="print each probe's step count and stop")
    options = parser.parse_args()
    if options.dry_run:
        root = Path("dry")
        plans = {"pause": pause_probes(root, options.base), "players": players_probes(root, options.peers, options.base, options.moderate)}
        print(json.dumps({case: {who: len(probe["steps"]) for who, probe in probes.items()} for case, probes in plans.items()}))
        return 0
    if not 1024 < options.port < 50000:
        parser.error("take the port from the lane's own block below 50000")
    options.repo = options.repo.resolve()
    options.out.mkdir(parents=True, exist_ok=False)
    sizes = SIZES if options.all_sizes else (options.size,)
    rows = []
    for size in sizes:
        cases = ("pause", "players", "status", "repair", "host-leave", "sp-pause") if options.case == "all" else (options.case,)
        for case in cases:
            if case == "sp-pause":
                rows.append(run_sp_pause(options, options.out / f"sp-pause-{size}", size))
            else:
                rows.append(run_peers(options, options.out / f"{case}-{size}-{options.peers if case in ('players', 'host-leave') else 2}p{'-diag' if case == 'status' and options.diagnostics else ''}", case, size,
                                      options.peers, options.base, options.moderate and case == "players"))
            options.port += 1
    result = {"pass": all(row["pass"] for row in rows), "revision": subprocess.check_output(["git", "-C", str(options.repo), "rev-parse", "HEAD"], text=True).strip(),
              "exe_sha256": sha(engine_executable(options.repo)), "driver_sha256": sha(__file__), "base": options.base, "rows": rows,
              "sp_pause": [row["snapshot"] for row in rows if row["case"] == "sp-pause"]}
    (options.out / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    red = [f"{row['case']}/{row['size']}: {check['check']}" for row in rows for check in row["checks"] if not check["pass"]]
    print(f"[in-match-ux] {'PASS' if result['pass'] else 'FAIL'} {options.out / 'result.json'}" + (f" red: {red}" if red else ""))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
