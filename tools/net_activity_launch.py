"""Exercise agreed activity rules through two peers and the recorded launch."""
from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess

from compare_sim_traces import strict_compare
import net_lobby_wire
from run_sim_test import make_run

# Magic, envelope version, header size, message type, payload length.
ENVELOPE = "<IHHHHI"


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def stamp():
    return subprocess.check_output(["C:/Program Files/Git/usr/bin/date.exe", "+%Y-%m-%d %H:%M:%S MST"], text=True).strip()


def rules_for(variant):
    rules = dict(activity_module="Base.rte", activity_type="GAScripted", activity_preset="P4 Alpha Duel",
                 scene_module="Base.rte", scene_name="Grasslands", difficulty=80, starting_gold=5000,
                 fog_of_war=True, require_clear_path_to_orbit=True, deploy_units=True,
                 teams=[dict(technology_intent="-All-", technology_module="", ai_skill=50) for _ in range(4)])
    rules["teams"][1] = dict(technology_intent="Coalition.rte", technology_module="Coalition.rte", ai_skill=70)
    if variant == "default":
        rules.update(difficulty=50, starting_gold=0, fog_of_war=False, require_clear_path_to_orbit=False, deploy_units=False)
        rules["teams"][1] = dict(technology_intent="-All-", technology_module="", ai_skill=50)
    elif variant == "infinite":
        rules["starting_gold"] = 1000000000
    elif variant == "site":
        rules["scene_name"] = "Fredeleig Plains"
    elif variant == "stock":
        rules["activity_preset"] = "Skirmish Defense"
        rules["scene_name"] = "Ketanot Hills"
    elif variant == "stock-scene":
        # No standardRules: the request names the site, so a missing scene write stays Grasslands.
        rules["activity_preset"] = "Skirmish Defense"
        rules["scene_name"] = "Ketanot Hills"
        rules.update(difficulty=50, starting_gold=0, fog_of_war=False, require_clear_path_to_orbit=False,
                     deploy_units=False)
        rules["teams"][1] = dict(technology_intent="-All-", technology_module="", ai_skill=50)
    elif variant in ("brains", "brains-auto", "hold-desync", "hold-resync", "resync-skirmish", "brains-longname",
                     "brains-shared", "wire-refusal", "rendezvous-cap"):
        # Skirmish Defense on a site with no brain on it: every human seat has to place its own brain in
        # the setup editor, which is the start a lockstep match has to synchronize.
        rules["activity_preset"] = "Skirmish Defense"
        rules["scene_name"] = "Grasslands"
    if variant == "brains-longname":
        rules["client_name"] = LONG_SEAT_NAME
    if variant == "census":
        # The offline leg is the stock command line launch, and -scenario resolves "Determinism <name>"
        # presets only, so the census runs the one activity both paths can launch. What that offline launch
        # produces: the preset declares no DefaultGoldMediumDifficulty, so ActivityMan::StartActivity falls
        # back to the activity's own team funds, and the CLI path stages its site with units undeployed.
        rules.update(activity_module="Tests.rte", activity_preset="Determinism SimBaseline",
                     difficulty=50, starting_gold=2000, fog_of_war=False,
                     require_clear_path_to_orbit=False, deploy_units=False)
        rules["teams"][1] = dict(technology_intent="-All-", technology_module="", ai_skill=50)
    elif variant == "missing-activity":
        rules["activity_preset"] = "Missing launch activity"
    elif variant == "missing-scene":
        rules["scene_name"] = "Missing launch scene"
    elif variant == "missing-module":
        rules["activity_module"] = "Coalition.rte"
    elif variant == "missing-tech":
        rules["teams"][1].update(technology_intent="Missing.rte", technology_module="Missing.rte")
    return rules


def encode_config(rules, wire, dedicated=False, default=False):
    """Encode the existing L12 MatchConfig packet consumed by NetLobbyProtocol, at that tree's own versions."""
    if struct.calcsize(ENVELOPE) != wire.header_bytes.value:
        raise RuntimeError(f"envelope layout is {struct.calcsize(ENVELOPE)} bytes, {wire.header_bytes.site} says "
                           f"{wire.header_bytes.value}")
    def string(value):
        encoded = value.encode("utf-8")
        return struct.pack("<H", len(encoded)) + encoded
    mode = 1 if default else 2
    roster = [(peer, peer - 1 if default else 0, False, "Host" if peer == 1 else rules.get("client_name", "Client"))
              for peer in range(2 if dedicated else 1, 3)]
    if not default:
        roster.append((0, 1, True, "CPU"))
    horizon = int(rules.get("path_horizon_ticks") or 0)
    reserved = int(dedicated)
    if horizon:
        reserved |= 2
    payload = struct.pack("<HQBBHBBH", wire.config_version.value, 1, 1, 2, 3, mode, 2, reserved)
    for value in (rules["activity_type"], rules["activity_preset"], rules["scene_name"], "PvP" if default else "CoopPvE"):
        payload += string(value)
    payload += struct.pack("<B", len(roster))
    for peer, team, cpu, name in roster:
        payload += struct.pack("<BBBB", peer, team, int(cpu), 0) + string(name)
    payload += struct.pack("<BQQ", 0, 1, 1)
    payload += string(rules["activity_module"]) + string(rules["scene_module"])
    payload += struct.pack("<BIBBBB", rules["difficulty"], rules["starting_gold"], rules["fog_of_war"],
                           rules["require_clear_path_to_orbit"], rules["deploy_units"],
                           int(rules.get("brainless_humans_spectate", True)))
    for team in rules["teams"]:
        payload += string(team["technology_intent"]) + string(team["technology_module"]) + struct.pack("<B", team["ai_skill"])
    payload += struct.pack("<BIBBB", 0, 0, 10, 1, 1)
    if horizon:
        payload += struct.pack("<H", horizon)
    return struct.pack(ENVELOPE, wire.magic.value, wire.version.value, wire.header_bytes.value,
                       wire.match_config_type.value, 0, len(payload)) + payload


def config_refusal(logs, wire, exe_hash):
    """What a peer said when it refused the launch config, beside the versions this run wrote and from where."""
    for peer, log in logs.items():
        reasons = re.findall(r"\[net-match-service-e2e\] setup failed: launch config: (.+)", log)
        if reasons:
            return (f"{peer} refused the launch config: {reasons[0]}; this run wrote {wire.describe()} "
                    f"from the tree under test, against exe {exe_hash[:16]}")
    return None


def report_checks(checks, note=None):
    for name, passed in checks.items():
        print(f"{'PASS' if passed else 'FAIL'} {name}" + ("" if passed or not note else f" ({note})"))


def observations(log):
    return [dict(token.split("=", 1) for token in shlex.split(line.removeprefix("[e2e] rules ")))
            for line in log.splitlines() if line.startswith("[e2e] rules ")]


def seed_observations(log):
    return [dict(token.split("=", 1) for token in shlex.split(line.removeprefix("[e2e] seed ")))
            for line in log.splitlines() if line.startswith("[e2e] seed ")]


ACTIVITY_DEFAULT_FUNDS = 2000


def iostream_float_token(value):
    """The token Main.cpp's [e2e] rules line writes for a float (defaultfloat, precision 6)."""
    return format(float(value), ".6g")


def roster_seeded_teams(default=False, dedicated=False):
    """Teams encode_config seats. NetActivitySetup seeds only those teams with the agreed gold."""
    teams = {peer - 1 if default else 0 for peer in range(2 if dedicated else 1, 3)}
    if not default:
        teams.add(1)
    return teams


def expected_seed(rules, default=False, dedicated=False):
    """Expected [e2e] seed tokens: the NetActivitySetup lobby seed before StartActivity.

    Roster-seeded teams carry the agreed starting gold; unseeded teams keep the cloned
    activity default 2000. Form is iostream defaultfloat, same as the seed line.
    """
    seeded = roster_seeded_teams(default, dedicated)
    expected = {}
    for index in range(4):
        funds = rules["starting_gold"] if index in seeded else ACTIVITY_DEFAULT_FUNDS
        expected[f"team{index}.funds"] = iostream_float_token(funds)
    return expected


def post_start_funds(rules, default=False, dedicated=False):
    """GetTeamFunds after StartActivity — the token [e2e] rules prints at sim tick 1."""
    preset = rules["activity_preset"]
    gold = rules["starting_gold"]
    difficulty = rules["difficulty"]
    seeded = roster_seeded_teams(default, dedicated)
    funds = {}
    if preset == "P4 Alpha Duel":
        # P4AlphaDuel.lua:51-52 zeros TEAM_1/TEAM_2; teams 2-3 stay the cloned 2000.
        for index in range(4):
            funds[index] = 0 if index in (0, 1) else ACTIVITY_DEFAULT_FUNDS
    elif preset == "Skirmish Defense":
        # SkirmishDefense.lua:106-107 sets every team to GetStartingGold, then :142-146
        # each CPU team to 100000 when gold>100000 else 80*Difficulty+2000.
        cpu_teams = set() if default else {1}
        for index in range(4):
            funds[index] = (100000 if gold > 100000 else 80 * difficulty + 2000) if index in cpu_teams else gold
    else:
        # SimBaseline.lua has no SetTeamFunds; the seed stands (census).
        for index in range(4):
            funds[index] = gold if index in seeded else ACTIVITY_DEFAULT_FUNDS
    return funds


def expected_observation(rules, default=False, dedicated=False):
    """Expected [e2e] rules tokens.

    gold is the integer GetStartingGold print. teamN.funds is GetTeamFunds after
    StartActivity, in iostream defaultfloat form, per activity script:

    P4 Alpha Duel (default, infinite, site, resync-duel): teams 0-1 are 0
    (P4AlphaDuel.lua:51-52); teams 2-3 stay 2000.
    Skirmish Defense (stock, brains*): every team starting_gold, then each CPU
    team 100000 if gold>100000 else 80*difficulty+2000 (SkirmishDefense.lua:106-107,
    :142-146).
    Determinism SimBaseline (census): no SetTeamFunds; the seed stands.

    The lobby seed itself is scored from the [e2e] seed line, not this printer.
    """
    expected = dict(tick="1", difficulty=str(rules["difficulty"]), gold=str(rules["starting_gold"]),
                    fog=str(int(rules["fog_of_war"])), orbit=str(int(rules["require_clear_path_to_orbit"])),
                    deploy=str(int(rules["deploy_units"])), cpu_team="-1" if default else "1",
                    activity=rules["activity_module"] + "/" + rules["activity_preset"],
                    scene=rules["scene_module"] + "/" + rules["scene_name"])
    funds = post_start_funds(rules, default, dedicated)
    for index, team in enumerate(rules["teams"]):
        expected[f"team{index}.tech"] = team["technology_module"] or "-All-"
        expected[f"team{index}.ai"] = str(team["ai_skill"])
        expected[f"team{index}.funds"] = iostream_float_token(funds[index])
    return expected


LOSS_TEXT = "Your brain has been destroyed!"


def score_p4_loss_text(log):
    """The game-over screen still shows the loss line after 6 s (duration -1, not the 5 s transient)."""
    ended = 'Activity "P4 Alpha Duel" was ended' in log or "[p4-duel] loss-duration=-1" in log
    pinned = "[p4-duel] loss-duration=-1" in log
    still = f"[p4-duel] game-over-loss-still={LOSS_TEXT}" in log
    return {"ended": ended, "pinned": pinned, "still": still, "pass": (not ended) or (pinned and still)}


def score_rules(log, rules, default=False, dedicated=False):
    rows, expected = observations(log), expected_observation(rules, default, dedicated)
    actual = rows[0] if len(rows) == 1 else {}
    differences = {key: {"expected": value, "actual": actual.get(key)} for key, value in expected.items() if actual.get(key) != value}
    return {"pass": len(rows) == 1 and not differences, "observations": rows, "differences": differences}


def score_seed(log, rules, default=False, dedicated=False):
    rows, expected = seed_observations(log), expected_seed(rules, default, dedicated)
    actual = rows[0] if len(rows) == 1 else {}
    differences = {key: {"expected": value, "actual": actual.get(key)} for key, value in expected.items() if actual.get(key) != value}
    return {"pass": len(rows) == 1 and not differences, "observations": rows, "differences": differences}


def placements(log, kind):
    """Every placement line of one kind a peer logged, keyed by seat."""
    prefix = "[net-match] brain placement applied: " if kind == "applied" else "[net-match] brain placed: "
    rows = {}
    for line in log.splitlines():
        if line.startswith(prefix):
            # Preset names carry spaces, so split on the key boundaries, not on every space.
            fields = re.split(r" (?=[a-z_]+=)", line.removeprefix(prefix))
            row = dict(field.split("=", 1) for field in fields if "=" in field)
            rows[row.get("seat")] = row
    return rows


def score_placements(logs, seats=2):
    """Both peers must take every seat's placement off the wire from that seat's own peer, and build the
    identical brain - same preset, same spot, same unique id - before the match starts."""
    expected_seats = [str(seat) for seat in range(seats)]
    scored = {}
    for kind in ("applied", "built"):
        rows = {peer: placements(log, kind) for peer, log in logs.items()}
        complete = all(sorted(seat_rows) == expected_seats for seat_rows in rows.values())
        # Seat N is held by peer N+1: a placement each peer made locally would not carry the other's id.
        issuers = all(seat_rows.get(seat, {}).get("peer") == str(index + 1)
                      for seat_rows in rows.values() for index, seat in enumerate(expected_seats))
        identical = len({json.dumps(seat_rows, sort_keys=True) for seat_rows in rows.values()}) == 1
        scored[kind] = {"complete": complete, "issuers": issuers, "identical": identical, "rows": rows}
    scored["pass"] = all(scored[kind][key] for kind in ("applied", "built") for key in ("complete", "issuers", "identical"))
    return scored


# The harness blocks this driver owns; 48540-48559 belong to the menu readback detector.
PORT_BLOCKS = ((48320, 48539), (48630, 48639), (48640, 48649))


# Each peer holds one seat and places a different brain at a different spot, so a placement that failed to
# cross the wire leaves the peers holding different residents.
EDITOR_SEATS = {"host": dict(player=0, x_fraction=0.30, cls="Actor", preset="Brain Case"),
                "client": dict(player=1, x_fraction=0.70, cls="AHuman", preset="Brain Robot")}
# Exactly 64 characters, the longest lobby name a seat can carry, so the strip's ellipsis is exercised.
LONG_SEAT_NAME = "Client" + "GunnhildrTheVeryPatientBrainPlacerOfKetanotHills" + "X" * 10
# The host's own non-default name: short, so its share of the line always survives the elision.
LONG_HOST_NAME = "Hostable"
REFUSED_PRESET = "No Such Brain"
# A DONE with no brain placed is refused by the editor itself (SceneEditorGUI::DONEEDITING ->
# TestBrainResidence): the seat goes back to installing or picking a brain, stays unready and commits
# nothing. Which of the two stock prompts is on screen depends on the frame the probe reads.
# The net layer's own refusal leads now; the stock editor's prompts follow it on later frames.
PLACE_REFUSED = ("Place your brain in a valid spot first", "Pick what you want to place next",
                 "Click to INSTALL your governor brain")
READY_TEXT = "READY to start"
# The tall box spells the wait out; the one-line strip a short screen gets says it in its own words.
WAIT_BANNER = "to place their brains"
COMPACT_WAIT_BANNER = "TO PLACE"
COMPACT_MAX_HEIGHT = 480
# Both seats, in the order the roster seats them, as the strip names them while neither has placed.
PLACEMENT_NAMES = "Host, Client"
# The client's placement waits on this signal from the host's probe, so the waiting window never
# depends on either peer's pacing.
WAITING_SEEN_SIGNAL = "host_waiting_seen"
# rendezvous-cap: the host holds to this tick before it places and signals, and the client holds to the
# second one after the signal. Each hold is under the engine's 120-tick setup-editor watchdog and the two
# together are past it, so the run passes only when the watchdog counts from the rendezvous.
RENDEZVOUS_CAP_HOST_TICK = 80
RENDEZVOUS_CAP_CLIENT_TICK = 170
EDITOR_CAP_ERROR = "setup editor did not finish within"


def wait_banner(resolution):
    return COMPACT_WAIT_BANNER if resolution and resolution[1] < COMPACT_MAX_HEIGHT else WAIT_BANNER


def compact(resolution):
    return bool(resolution) and resolution[1] < COMPACT_MAX_HEIGHT


def shots(peer, name):
    """The overlay layer and the composited frame (world, editor and overlay), both of one rendered frame."""
    return [{"op": "screenshot_pair", "name": f"{peer}_{name}", "composited_name": f"{peer}_{name}_world"}]


def editor_script(peer, capture, place_after, finish_at_ready=False, wire_refusal=False, resolution=None,
                  host_signal=None, long_names=False, shared_seat=False, place_after_signal=0):
    """The UI probe script that drives this peer's own seat through the setup editor, the way a player does."""
    seat = EDITOR_SEATS[peer]
    player = seat["player"]
    # The stock picker slides in with the editor, and the seat's own message band rides the top rows: the
    # network overlay has to be clear of both. The renders wait is past the picker's slide at any speed.
    steps = [{"op": "wait", "service": "Running"}, {"op": "wait", "editing": True},
             {"op": "wait", "player": player, "picker_open": True}, {"op": "wait", "renders": 12},
             {"op": "assert_net_ui_clear", "player": player, "picker_open": True, "screen_text": True}]
    if shared_seat:
        # This peer also presents the roster's other human seat, so its window is split and the overlay
        # owes both pickers and both message bands the same clearance it owes the seat it drives.
        other = 1 - player
        steps += [{"op": "wait", "player": other, "picker_open": True}, {"op": "wait", "renders": 4},
                  {"op": "assert_net_ui_clear", "player": other, "picker_open": True, "screen_text": True},
                  # A presented seat writing a non-brain object is refused on the roster owner.
                  {"op": "editor_place", "player": other, "x_fraction": 0.50,
                   "class": "HDFirearm", "preset": "Pistol", "module": "Base.rte", "name": "presented_place"},
                  {"op": "assert_editor", "player": other, "name": "presented_place_refusal",
                   "equals": {"placement_refused": True},
                   "screen_text_contains": "can't place"},
                  # The seats panel while the editor holds the world: the strip lifts off its rows.
                  {"op": "key_down", "key": "F6"}, {"op": "key_up", "key": "F6"},
                  {"op": "wait", "panel_open": True}, {"op": "wait", "renders": 4},
                  {"op": "assert_net_ui_clear", "player": player, "picker_open": True, "screen_text": True},
                  {"op": "assert_net_ui_clear", "player": other, "picker_open": True, "screen_text": True},
                  {"op": "key_down", "key": "F6"}, {"op": "key_up", "key": "F6"},
                  {"op": "wait", "panel_open": False}]
    if compact(resolution):
        if long_names:
            # A 64-char seat name cannot survive the strip whole: FitLine ellides it, and the count
            # outlives every fallback the line gives way through.
            steps += [{"op": "assert_control", "control": "LabelNetMatchStatus", "equals": {"visible": True}, "fits": True,
                       "text_contains": "WAITING FOR "},
                      {"op": "assert_control", "control": "LabelNetMatchStatus", "equals": {"visible": True}, "fits": True,
                       "text_contains": "..."},
                      {"op": "assert_control", "control": "LabelNetMatchStatus", "equals": {"visible": True}, "fits": True,
                       "text_contains": " TO PLACE"},
                      {"op": "assert_control", "control": "LabelNetMatchStatus", "equals": {"visible": True}, "fits": True,
                       "text_contains": "0 of 2"}]
        else:
            # An open picker leaves the strip under half a short screen: the metrics tail is what gives way,
            # the seat names stay whole while it can, and the count outlives every fallback. A shared second
            # picker column can take the names with it, so that arm requires the count alone.
            if not shared_seat:
                steps += [{"op": "assert_control", "control": "LabelNetMatchStatus", "equals": {"visible": True}, "fits": True,
                           "text_contains": "WAITING FOR " + PLACEMENT_NAMES + " TO PLACE"}]
            steps += [{"op": "assert_control", "control": "LabelNetMatchStatus", "equals": {"visible": True}, "fits": True,
                       "text_contains": "0 of 2"}]
    elif long_names:
        # The tall box spells the wait out; the host's short name leads and the long one ellides.
        steps += [{"op": "assert_control", "control": "LabelNetMatchStatus", "equals": {"visible": True}, "fits": True,
                   "text_contains": LONG_HOST_NAME + ", "},
                  {"op": "assert_control", "control": "LabelNetMatchStatus", "equals": {"visible": True}, "fits": True,
                   "text_contains": "..."},
                  {"op": "assert_control", "control": "LabelNetMatchStatus", "equals": {"visible": True}, "fits": True,
                   "text_contains": "to place their brains"}]
    if long_names and peer == "host":
        # The join banner keeps its verb: the name that fills the line is what gives way.
        steps += [{"op": "assert_control", "control": "LabelNetMatchToastNewest", "equals": {"visible": True},
                   "fits": True, "text_contains": " joined"}]
    if capture:
        steps += shots(peer, "editor_open")
    if wire_refusal:
        if peer == "host":
            # A placement every peer has to refuse: the preset is not installed anywhere.
            steps += [{"op": "place_brain_command", "player": player, "preset": REFUSED_PRESET, "class": "Actor", "module": "Base.rte"},
                      {"op": "wait", "renders": 30},
                      {"op": "assert_editor", "player": player, "equals": {"ready": False, "submitted": False}}]
        # Both peers shoot the same held tick, inside the sender's 3.5 s banner: the banner is the sender's alone.
        steps += [{"op": "wait", "sim_at_least": 30}]
        if peer == "host":
            # The refusal reason is the longest message a seat's screen carries, so it is read where it lands.
            steps += [{"op": "assert_net_ui_clear", "player": player, "screen_text": True}]
        steps += shots(peer, "wire_refusal")
    # DONE before a brain is placed: refused, so the seat stays unready, commits nothing and is sent back
    # to place a brain.
    steps += [{"op": "editor_done", "player": player},
              {"op": "assert_editor", "player": player, "name": "done_refusal",
               "equals": {"ready": False, "submitted": False, "resident": False}},
              {"op": "assert_net_ui_clear", "player": player, "screen_text": True}]
    if capture:
        steps += shots(peer, "refusal")
    if place_after:
        steps.append({"op": "wait", "sim_at_least": place_after})
    if peer != "host" and host_signal:
        steps.append({"op": "wait_file", "path": str(host_signal)})
        if place_after_signal:
            # The hold the editor watchdog is allowed to count, taken after the rendezvous ends the wait.
            steps.append({"op": "wait", "sim_at_least": place_after_signal})
    steps += [{"op": "editor_place_brain", "player": player, "x_fraction": seat["x_fraction"],
               "class": seat["cls"], "preset": seat["preset"], "module": "Base.rte"},
              # Placing alone commits nothing: the wire only carries the seat's DONE.
              {"op": "assert_editor", "player": player, "equals": {"resident": True, "ready": False, "submitted": False}},
              {"op": "editor_done", "player": player},
              {"op": "wait", "seat_ready": player}]
    if compact(resolution) and peer != "host":
        # The compact strip's delay field has to spell its unit: a bare "D 3" reads as "0 3".
        steps += [{"op": "assert_control", "control": "LabelNetMatchStatus", "equals": {"visible": True},
                   "fits": True, "text_contains": "delay 3"}]
    # The frame the seat's own ready lands on is the one that still says all brains are placed: the editor
    # leaves a tick or two later, so the shot goes before the assertion that reads the same state.
    if capture and peer != "host":
        steps += shots(peer, "ready")
    steps += [{"op": "assert_editor", "player": player, "equals": {"ready": True, "submitted": True}}]
    if peer == "host":
        # The client still holds its placement on the signal below, so a seat is unready here: the
        # strip's count is the wire's report of that, and it only draws while the editor is open.
        steps += [{"op": "assert_control", "control": "LabelNetMatchStatus", "text_contains": "1 of 2",
                   "equals": {"visible": True}},
                  {"op": "assert", "equals": {"editing": True}},
                  {"op": "wait", "player": player, "seat_text_contains": READY_TEXT},
                  {"op": "assert_control", "control": "LabelNetMatchStatus", "text_contains": wait_banner(resolution),
                   "equals": {"visible": True}, "fits": True}]
        if long_names:
            # One seat left, and the name alone still does not fit the line the box or strip gives it.
            steps += [{"op": "assert_control", "control": "LabelNetMatchStatus", "text_contains": "...",
                       "equals": {"visible": True}, "fits": True}]
        if capture:
            steps += shots(peer, "waiting_banner")
        steps.append({"op": "signal", "name": WAITING_SEEN_SIGNAL})
    if not finish_at_ready:
        # The arm that plays on: both seats done, so the editor has closed and the match runs.
        steps.append({"op": "wait", "editing": False})
        if peer == "host":
            steps.append({"op": "assert", "equals": {"editing": False}})
        steps.append({"op": "wait", "sim_at_least": 200})
        if capture:
            steps += shots(peer, "match_started")
        if shared_seat:
            # A presented seat's ActorSelect onto a craft passenger is presentation only.
            steps += [{"op": "actor_select", "player": 1 - player}, {"op": "wait", "renders": 4}]
        # Sample the bound seat's START while the scripted pad holds it, then release.
        steps += [{"op": "pad_down", "button": "start"}, {"op": "wait", "renders": 2},
                  {"op": "assert", "name": "pad_held", "equals": {"editing": False}},
                  {"op": "pad_up", "button": "start"}]
    steps.append({"op": "finish"})
    return {"schema": 1, "timeout_ms": 180000, "steps": steps}


def set_resolution(runtime, width, height):
    path = Path(runtime) / "Userdata" / "Settings.ini"
    settings = path.read_text(encoding="utf-8")
    for name, value in (("ResolutionX", width), ("ResolutionY", height)):
        settings, count = re.subn(rf"(?m)^(\s*{name}\s*=\s*)[^\r\n]*", lambda match: match[1] + str(value), settings)
        if count != 1:
            raise RuntimeError(f"expected one private {name} setting, found {count}")
    path.write_text(settings, encoding="utf-8")


def probe_result(root, peer):
    path = root / (peer + "-ui") / "net-ui-result.json"
    return json.loads(path.read_text(errors="replace")) if path.is_file() else {}


def committed(log):
    """Every commit line a peer logged, keyed by seat, with the path that read the spot."""
    rows = {}
    for line in log.splitlines():
        if line.startswith("[net-match] brain placement committed: "):
            fields = re.split(r" (?=[a-z_]+=)", line.removeprefix("[net-match] brain placement committed: "))
            row = dict(field.split("=", 1) for field in fields if "=" in field)
            rows[row.get("seat")] = row
    return rows


def tick_lines(path, tick="1"):
    """Every per-MO CC_SIM_DUMP line of one tick: the census of what the launch actually placed."""
    if not Path(path).is_file():
        return None
    return [line for line in Path(path).read_text(errors="replace").splitlines() if line.split(" ", 1)[0] == tick]


# THE CENSUS CONTRACT: the census compares what the two launches placed, byte for byte, with exactly the three
# values a seat binding writes normalized away - because the two runs bind their seats in a different order by
# construction, not because their worlds differ. An offline -scenario run's single local human seat is bound
# inside tick 1 (Source/Activities/GameActivity.cpp:1825 "never had a brain" -> Activity::SwitchToActor,
# Source/Entities/Activity.cpp:1091 -> Actor::SetControllerMode, Source/Entities/Actor.cpp:888), and the
# notification runs there and then (Source/Entities/Actor.cpp:901). A lockstep match's controller is wire-owned:
# Controller::SetInputMode leaves the sim-facing mode to the wire (Source/System/Controller.h:246),
# Actor::SetControllerMode skips the notification (Source/Entities/Actor.cpp:896), and the committed frame
# applies both through MovableMan::ApplyLockstepControlHandoffToActor (Source/Managers/MovableMan.cpp:888) an
# input delay later, on every peer at the same tick. Each normalized column is named with the site that writes it.
SEAT_BINDING_COLUMNS = {
    "mode": "Controller::m_InputMode; the wire owns it under lockstep (Source/System/Controller.h:246)",
    "pie": "PieMenu enabled state (field 0), set by DoDisableAnimation on the handoff (Source/Entities/Actor.cpp:909)",
    "atmr": "the new-control timer (field 4 of 5), reset on the handoff (Source/Entities/Actor.cpp:903)",
}
SEAT = "<seat-binding>"
# A seated actor reads CIM_PLAYER (Source/System/Controller.h:99); every other mode is unseated.
SEATED_MODE = "1"


def normalize_seat_binding(line):
    """The line with the seat binding's own values replaced, and the raw values it replaced."""
    tokens, replaced = line.split(" "), {}
    for index, token in enumerate(tokens):
        name, sign, value = token.partition("=")
        if not sign or name not in SEAT_BINDING_COLUMNS:
            continue
        if name == "mode":
            normalized = SEAT
        elif name == "pie":
            # Only the enabled state is the binding's; the slice census behind it stays under the compare.
            fields = value.split(":")
            normalized = ":".join([SEAT, *fields[1:]]) if len(fields) > 1 else value
        else:
            # Only the 4th of the actor's five timers is the new-control one; the other four stay.
            fields = value.split("/")
            normalized = "/".join([*fields[:3], SEAT, *fields[4:]]) if len(fields) == 5 else value
        if normalized == value:
            # A column whose shape is not the one this rule was read from is left to the compare.
            continue
        replaced[name] = value
        tokens[index] = name + "=" + normalized
    return " ".join(tokens), replaced


def seated_actors(lines):
    """The uid of every actor a run has already seated at this tick."""
    seated = []
    for line in lines:
        fields = line.split(" ")
        if len(fields) > 2 and fields[1] == "actor" and f"mode={SEATED_MODE}" in fields:
            seated.append(fields[2].removeprefix("uid="))
    return seated


def compare_census_lines(left, right):
    """The contract's compare: the launch census line by line, each line's seat-binding columns aside."""
    if not left:
        return {"pass": False, "reason": "offline census tick-1 lines missing", "offline_lines": 0,
                "match_lines": len(right), "seat_binding_differences": [], "first_difference": None}
    if not right:
        return {"pass": False, "reason": "match census tick-1 lines missing", "offline_lines": len(left),
                "match_lines": 0, "seat_binding_differences": [], "first_difference": None}
    normalized = [[normalize_seat_binding(line) for line in lines] for lines in (left, right)]
    lines = [[line for line, _ in side] for side in normalized]
    first = next((index for index, (a, b) in enumerate(zip(*lines)) if a != b), None)
    if first is None and len(lines[0]) != len(lines[1]):
        first = min(len(lines[0]), len(lines[1]))
    differences = [{"index": index, "column": column, "offline": raw[column],
                    "match": normalized[1][index][1].get(column)}
                   for index, (_, raw) in enumerate(normalized[0]) if index < len(normalized[1])
                   for column in raw if raw[column] != normalized[1][index][1].get(column)]
    return {"pass": lines[0] == lines[1], "offline_lines": len(left), "match_lines": len(right),
            "seat_binding_differences": differences,
            "first_difference": None if first is None else {
                "index": first,
                "offline": left[first] if first < len(left) else None,
                "match": right[first] if first < len(right) else None}}


def census_injection_selftest(left, right, baseline):
    """The compare's own RED: a real census difference must still fail, a seat-binding one must not decide."""
    def line_of(lines, kind):
        index = next((index for index, line in enumerate(lines) if line.split(" ")[1:2] == [kind]), None)
        if index is None:
            raise RuntimeError(f"census injection found no {kind} line to perturb")
        return index
    def inject(lines, kind, column, value, separator=None, field=None):
        index = line_of(lines, kind)
        perturbed = list(lines)
        if separator is not None:
            # Perturb one field of a column the rule normalizes elsewhere: the neighbours must still be compared.
            current = re.search(rf"(?<= ){re.escape(column)}=(\S+)", perturbed[index])
            fields = current[1].split(separator)
            fields[field] = value
            value = separator.join(fields)
        perturbed[index] = re.sub(rf"(?<= ){re.escape(column)}=\S+", f"{column}={value}", perturbed[index], count=1)
        if perturbed[index] == lines[index]:
            raise RuntimeError(f"census injection {column}={value} did not apply to the {kind} line")
        return perturbed
    def fails(perturbed):
        return not compare_census_lines(perturbed, right)["pass"]
    # A moved actor, a different team roster, a different pie census and a different actor timer are real
    # census differences; only the binding's own field of a normalized column is not one.
    checks = {"injected_actor_position_fails": fails(inject(left, "actor", "pos", "0x1.0000000000000p+0,0x1.0000000000000p+0")),
              "injected_team_roster_fails": fails(inject(left, "activity", "t0", "9/9")),
              "injected_pie_slice_count_fails": fails(inject(left, "actor", "pie", "99", ":", 1)),
              "injected_other_actor_timer_fails": fails(inject(left, "actor", "atmr", "0x0.0000000000000p+0", "/", 0)),
              "injected_seat_binding_does_not_decide":
                  compare_census_lines(inject(left, "actor", "mode", "7"), right)["pass"] == baseline}
    return {"pass": all(checks.values()), "checks": checks}


def census_compare(offline_dump, match_dump):
    left, right = tick_lines(offline_dump), tick_lines(match_dump)
    if left is None or right is None:
        return {"pass": False, "reason": "a census dump is missing", "offline_dump": str(offline_dump), "match_dump": str(match_dump)}
    result = compare_census_lines(left, right)
    # The rule is asserted, not assumed: the offline run seats its own actor inside tick 1 and the match,
    # whose binding waits for the committed frame, has seated nothing yet.
    seated = {"offline": seated_actors(left), "match": seated_actors(right)}
    bindings_as_declared = bool(seated["offline"]) and not seated["match"]
    result.update(seat_binding_columns=SEAT_BINDING_COLUMNS, seated_actors=seated,
                  bindings_as_declared=bindings_as_declared,
                  injection_selftest=census_injection_selftest(left, right, result["pass"]))
    result["pass"] = result["pass"] and bindings_as_declared and result["injection_selftest"]["pass"]
    return result


def launch(options):
    if Path("D:/mx/LEAD_FAMILY.lock").exists():
        raise RuntimeError("family lock exists; launch is deferred")
    if not any(low <= options.port <= high for low, high in PORT_BLOCKS):
        raise ValueError("port must be in " + " or ".join(f"{low}..{high}" for low, high in PORT_BLOCKS))
    os.environ["CCCP_HEADLESS"] = "1"
    repo, root = options.repo.resolve(), options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    rules = rules_for(options.variant)
    # The census launches the activity preset's own two seats, which is the roster the default config carries.
    default = options.variant in ("default", "census", "resync-duel")
    refusal = options.variant.startswith("missing-")
    config = root / "launch-config.bin"
    wire = net_lobby_wire.read(repo)
    config.write_bytes(encode_config(rules, wire, options.dedicated, default))
    exe_hash = sha(repo / "Cortex Command.exe")
    manifest = dict(stamp=stamp(), repo=str(repo), exe_sha256=exe_hash, rules=rules, variant=options.variant, wire=wire.as_json(),
                    dedicated=options.dedicated, port=options.port, detector_sha256=sha(__file__), config_sha256=sha(config),
                    codec_driver_sha256=sha(Path(__file__).with_name("test_net_activity_options.py")),
                    compare_sha256=sha(Path(__file__).with_name("compare_sim_traces.py")))
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    def ledger(event):
        actual = sha(repo / "Cortex Command.exe")
        with (root / "exe-ledger.jsonl").open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(dict(stamp=stamp(), event=event, exe_sha256=actual)) + "\n")
        if actual != exe_hash:
            raise RuntimeError("executable changed during launch case")
    # The setup editor is driven through the UI probe's own seam, so the arm commits the way a player does.
    editor_driven = options.variant in ("brains", "stock", "stock-scene", "hold-desync", "hold-resync", "resync-skirmish",
                                       "brains-longname", "brains-shared", "wire-refusal", "rendezvous-cap")
    shared_seat = options.variant == "brains-shared"
    places_brains = editor_driven or options.variant == "brains-auto"
    # resync-duel is the control: the same perturbation and heal on an activity that never opens the editor.
    hold_resync = options.variant in ("hold-resync", "resync-duel", "resync-skirmish")
    # Both arms perturb one peer inside the hold; the resync one heals from the host snapshot and plays on.
    hold_desync = options.variant == "hold-desync" or hold_resync
    resolution = getattr(options, "resolution", None)
    captures = bool(getattr(options, "captures", False))
    common = ["-net-match-service-e2e", "-net-port", str(options.port), "-net-match-peers", "2",
              "-net-match-mode", "pvp" if default else "coop-pve", "-net-match-ticks", "600", "-max-ticks", "600",
              "-net-match-input-delay", "3", "-seed", "42", "-num-lua-states", "4"]
    if not hold_desync:
        # The live desync exchange only samples when the run is not recording the offline trace.
        common.append("-tick-hashes")
    if options.variant == "brains-auto":
        # The seatless path: every peer stands in for its own players' DONE at the deterministic spot.
        common.append("-net-match-e2e-brain-placement")
    runs, records = {}, {}
    host_flags = []
    try:
        for peer in ("host", "client"):
            if peer == "host":
                flags = ["-net-dedicated"] if options.dedicated else ["-net-host"]
                if options.variant == "stock-scene":
                    flags += ["-net-match-service-preset", rules["activity_preset"],
                              "-net-match-service-module", rules["activity_module"],
                              "-net-match-service-scene", rules["scene_name"],
                              "-net-match-service-scene-module", rules["scene_module"]]
                else:
                    flags += ["-net-match-service-config", str(config)]
            else:
                flags = ["-net-join", "127.0.0.1"]
            if peer == "host":
                host_flags = list(flags)
            if options.variant == "brains-longname":
                # Each peer announces its own seat name; the flagless service default stays Host/Client.
                flags += ["-net-player-name", LONG_HOST_NAME if peer == "host" else LONG_SEAT_NAME]
            if shared_seat:
                # This peer presents the roster's other human seat too: one window, two split screens.
                flags.append("-net-match-e2e-shared-seat")
            trace, report = root / peer / "trace.json", root / peer / "report.json"
            flags += ["-out", str(trace), "-net-match-report", str(report), "-net-replay-out", str(root / peer / "match.ccreplay")]
            env = {"CCCP_HEADLESS": "1", "CC_SIM_DUMP": "1:600"}
            if editor_driven:
                # The client holds the world in the editor long enough for the hold itself to be under test.
                script = root / (peer + "-ui") / "ui-script.json"
                script.parent.mkdir(parents=True, exist_ok=False)
                # resync-skirmish seats both place past the injected desync (tick 50, resync ~tick 60):
                # the resync has to land while the seats are still placing, so the hold outlasts it.
                # wire-refusal places at the rendezvous: the 120-tick editor cap is a harness
                # safety net over the shared place phase, not an extra client hold.
                if options.variant == "wire-refusal":
                    delay = 0
                elif options.variant == "rendezvous-cap":
                    # The host is the slow probe: it holds the editor, then places and signals.
                    delay = RENDEZVOUS_CAP_HOST_TICK if peer == "host" else 0
                elif options.variant == "resync-skirmish":
                    delay = 90
                else:
                    delay = (90 if hold_desync else 45) if peer == "client" else 0
                script.write_text(json.dumps(editor_script(peer, captures, delay, hold_desync and not hold_resync,
                                                           options.variant == "wire-refusal", resolution,
                                                           host_signal=root / "host-ui" / (WAITING_SEEN_SIGNAL + ".json"),
                                                           long_names=options.variant == "brains-longname",
                                                           shared_seat=shared_seat,
                                                           place_after_signal=(RENDEZVOUS_CAP_CLIENT_TICK
                                                                              if options.variant == "rendezvous-cap" else 0)),
                                             indent=2), encoding="utf-8")
                env["CC_TEST_NET_UI_SCRIPT"] = str(script)
            if hold_desync and peer == "host":
                # One genuine divergence inside the hold: the held ticks' own hashes have to catch it.
                flags.append("-determinism-selftest-perturb")
            if hold_resync:
                # Heal from the host's snapshot instead of stopping, so the editor itself is resynced mid-placement.
                flags.append("-net-match-e2e-resync")
            runs[peer] = make_run(repo, [*common, *flags], root / peer, options.timeout, env=env, expected=[report])
            if resolution:
                set_resolution(runs[peer].cwd, *resolution)
        ledger("before_pair")
        for run in runs.values():
            run.start()
        with ThreadPoolExecutor(max_workers=2) as pool:
            pending = {peer: pool.submit(run.finish) for peer, run in runs.items()}
            records = {peer: result.result() for peer, result in pending.items()}
        ledger("after_pair")
    finally:
        for run in runs.values():
            run.close()
    logs = {peer: (root / peer / "stdout.log").read_text(errors="replace") for peer in runs}
    checks, result = {}, {"records": records, "wire": wire.as_json()}
    if options.variant == "stock-scene":
        result["stock_scene_request"] = {"standard_rules": False, "flags": host_flags,
                                         "scene_name": rules["scene_name"], "scene_module": rules["scene_module"],
                                         "activity_preset": rules["activity_preset"]}
        checks["stock_scene_request_has_no_standard_rules"] = "-net-match-service-config" not in host_flags
        checks["stock_scene_request_names_scene"] = ("-net-match-service-scene" in host_flags
                                                    and rules["scene_name"] in host_flags)
    # A config the engine turned down leaves the other peer with a bare timeout, so the refusal leads every failure.
    result["config_refusal"] = None if refusal else config_refusal(logs, wire, exe_hash)
    for peer, log in logs.items():
        for number, line in enumerate(log.splitlines(), 1):
            if line.startswith("[e2e] rules ") or line.startswith("[e2e] seed ") or "setup failed:" in line:
                print(f"{root / peer / 'stdout.log'}:{number}: {line}")
    if refusal:
        reasons = {peer: re.findall(r"\[net-match-service-e2e\] setup failed: (.+)", log) for peer, log in logs.items()}
        checks["both_refuse_same_reason"] = len(reasons["host"]) == len(reasons["client"]) == 1 and reasons["host"] == reasons["client"]
        checks["no_peer_launches"] = all(not observations(log) for log in logs.values())
        checks["bounded_refusal"] = all(record.get("exit_code") == 1 and not record.get("timed_out") for record in records.values())
        result["refusals"] = reasons
    else:
        if hold_resync:
            # A resync taken while the seats are still placing: every peer comes back with the same
            # placements and the same id base, so the brains they build carry identical unique ids.
            result["resync"] = {peer: re.findall(r"\[net-match\] resync: (.+)", log) for peer, log in logs.items()}
            checks["resync_ran_in_the_editor"] = all(any("reloading from the host snapshot" in line for line in lines)
                                                     for lines in result["resync"].values()) and \
                all("[net-match] brain placed:" not in log.split("resync: ")[0] for log in logs.values())
            if editor_driven:
                result["placements"] = score_placements(logs)
                checks["identical_brains_after_resync"] = result["placements"]["pass"]
            checks["both_peers_played_on"] = all(record.get("exit_code") == 0 and not record.get("timed_out") for record in records.values())
            result["probes"] = {peer: probe_result(root, peer) for peer in runs}
            result.update(checks=checks, passed=all(checks.values()), exe_sha256=exe_hash)
            (root / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
            report_checks(checks, result["config_refusal"])
            return 0 if result["passed"] else 1
        if hold_desync:
            # The whole arm: a divergence injected at tick 50, while the world is held in the setup editor,
            # must be named by the live checksum exchange before the hold ends at tick 90.
            reported = {peer: re.findall(r"sim state diverged at tick (\d+)", log) for peer, log in logs.items()}
            ticks = sorted(int(tick) for found in reported.values() for tick in found)
            result["hold_desync"] = {"reported": reported, "first_tick": ticks[0] if ticks else None,
                                     "perturbed_tick": 50, "client_places_after": 90}
            checks["desync_named_within_30_ticks"] = bool(ticks) and 50 < ticks[0] <= 80
            # No brain was ever built, so the world was still held in the setup editor when it was named.
            checks["named_before_the_match_started"] = all("[net-match] brain placed:" not in log for log in logs.values())
            # Evidence only: the peers are stopped by the desync itself, so a probe script cannot finish.
            result["probes"] = {peer: probe_result(root, peer) for peer in runs}
            result.update(checks=checks, passed=all(checks.values()), exe_sha256=exe_hash)
            (root / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
            report_checks(checks, result["config_refusal"])
            return 0 if result["passed"] else 1
        result["rules"] = {peer: score_rules(log, rules, default, options.dedicated) for peer, log in logs.items()}
        result["seed"] = {peer: score_seed(log, rules, default, options.dedicated) for peer, log in logs.items()}
        for peer in runs:
            checks[peer + "_process"] = records[peer].get("exit_code") == 0 and not records[peer].get("timed_out") and records[peer].get("evidence_complete", False)
            checks[peer + "_rules"] = result["rules"][peer]["pass"]
            checks[peer + "_seed"] = result["seed"][peer]["pass"]
        if options.variant in ("default", "resync-duel"):
            result["loss_text"] = {peer: score_p4_loss_text(log) for peer, log in logs.items()}
            checks["loss_text_after_6s"] = all(row["pass"] for row in result["loss_text"].values())
        if editor_driven:
            # Every commit came off the seat's own editor, not the deterministic-spot helper.
            result["commits"] = {peer: committed(log) for peer, log in logs.items()}
            checks["brains_via_editor"] = all(rows and all(row.get("via") == "editor" for row in rows.values())
                                              for rows in result["commits"].values())
            result["probes"] = {peer: probe_result(root, peer) for peer in runs}
            checks["ui_probe_pass"] = all(result["probes"][peer].get("pass") and result["probes"][peer].get("complete") for peer in runs)
            # The host signals and the client waits on it: each peer passes exactly one rendezvous, and the
            # engine's editor watchdog counts its ticks from that line, not from the start of the hold.
            result["rendezvous"] = {peer: [line for line in log.splitlines() if line.startswith("[net-ui-probe] rendezvous ")]
                                    for peer, log in logs.items()}
            checks["rendezvous_counted"] = all(len(lines) == 1 and WAITING_SEEN_SIGNAL in lines[0]
                                               for lines in result["rendezvous"].values())
            if not checks["rendezvous_counted"]:
                for peer, lines in result["rendezvous"].items():
                    print(f"{root / peer / 'stdout.log'}: rendezvous lines {lines}")
            if options.variant == "rendezvous-cap":
                # The editor phase is past the 120-tick watchdog end to end, and under it on either side of
                # the rendezvous: an engine that counts from the editor's first tick fails here.
                capped = {peer: [line for line in log.splitlines() if EDITOR_CAP_ERROR in line]
                          for peer, log in logs.items()}
                result["editor_cap"] = {"host_hold_tick": RENDEZVOUS_CAP_HOST_TICK,
                                        "client_hold_tick": RENDEZVOUS_CAP_CLIENT_TICK, "lines": capped}
                checks["editor_cap_counted_from_the_rendezvous"] = not any(capped.values())
                if not checks["editor_cap_counted_from_the_rendezvous"]:
                    for peer, lines in capped.items():
                        print(f"{root / peer / 'stdout.log'}: {lines}")
            def pad_held(probe):
                named = [index for index, step in enumerate(probe.get("script", {}).get("steps", []))
                         if step.get("name") == "pad_held"]
                return next((step.get("observed", {}) for step in probe.get("steps", [])
                             if step.get("index") in named), {})
            def pad_bound_row(observed):
                seat = observed.get("bound_seat") or 0
                rows = observed.get("controllers") or []
                if seat:
                    return next((row for row in rows if row.get("seat") == seat), None), seat
                started = next((row for row in rows if row.get("start")), None)
                return started, (started.get("seat") if started else 0)
            if not (hold_desync and not hold_resync):
                result["pad_held"] = {peer: pad_held(result["probes"][peer]) for peer in runs}
                leaks = []
                for observed in result["pad_held"].values():
                    row, seat = pad_bound_row(observed)
                    if row and (row.get("start") or row.get("moved")):
                        leaks.append(seat)
                named = leaks[0] if leaks else "N"
                checks[f"a scripted pad start moved seat {named}'s controller"] = not leaks
            # The refusal the production path shows a player who presses DONE with no brain placed: the seat
            # stays unready and uncommitted, and the stock editor asks for the brain again.
            # The DONE-refusal assertion names itself, so the arm reads that step and no other. A peer
            # presenting a second seat reads only the seat it drives - the other stays the roster's own.
            def refusal_rows(probe, peer):
                named = [index for index, step in enumerate(probe.get("script", {}).get("steps", []))
                         if step.get("name") == "done_refusal"]
                rows = next((step["observed"]["editor_seats"] for step in probe.get("steps", [])
                             if step["index"] in named), [])
                return [row for row in rows if row.get("player") == EDITOR_SEATS[peer]["player"]]
            result["refusals"] = {peer: refusal_rows(result["probes"][peer], peer) for peer in runs}
            checks["refusal_keeps_seat_unready"] = all(
                rows and all(not row["ready"] and not row["submitted"] and not row["resident"] and
                             any(text in row["screen_text"] for text in PLACE_REFUSED) for row in rows)
                for rows in result["refusals"].values())
            if shared_seat:
                def presented_refusal(probe, peer):
                    named = [index for index, step in enumerate(probe.get("script", {}).get("steps", []))
                             if step.get("name") == "presented_place_refusal"]
                    rows = next((step["observed"]["editor_seats"] for step in probe.get("steps", [])
                                 if step["index"] in named), [])
                    return [row for row in rows if row.get("player") == 1 - EDITOR_SEATS[peer]["player"]]
                result["presented_refusals"] = {peer: presented_refusal(result["probes"][peer], peer) for peer in runs}
                checks["presented_place_refused"] = all(
                    rows and all(row.get("placement_refused") is True and "can't place" in row.get("screen_text", "")
                                 for row in rows)
                    for rows in result["presented_refusals"].values())
            reports = {peer: json.loads((root / peer / "report.json").read_text(errors="replace")) for peer in runs}
            result["toasts"] = {peer: reports[peer].get("ui", {}).get("toasts", []) for peer in runs}
            checks["placement_toasts"] = all(sum(1 for toast in result["toasts"][peer] if toast.get("kind") == "brain_placed") == 2 for peer in runs)
            waits = [step for step in result["probes"]["host"].get("steps", []) if step.get("op") == "assert_control"]
            checks["waiting_banner_shown"] = any(wait_banner(resolution) in json.dumps(step["observed"].get("control", {})) for step in waits)
            if captures or options.variant == "wire-refusal":
                result["captures"] = sorted([str(path) for peer in runs for path in (root / (peer + "-ui")).glob("*.png")] +
                                            [str(path) for peer in runs for path in (root / peer / "runtime" / "ScreenShots").glob("*.png")])
            if options.variant == "wire-refusal":
                # Every peer refuses the same command, and only the peer that sent it gets the banner.
                refused = {peer: re.findall(r"brain placement refused: seat=(\d+) peer=(\d+) reason=(.+)", log) for peer, log in logs.items()}
                result["wire_refusals"] = refused
                checks["wire_refusal_on_both_peers"] = all(len(rows) == 1 and rows[0][0] == "0" and REFUSED_PRESET in rows[0][2]
                                                           for rows in refused.values())
                # Only the peer that sent it gets the banner; the other peers refuse it in silence.
                checks["wire_refusal_banner_on_the_sender"] = \
                    any(toast.get("kind") == "brain_refused" and REFUSED_PRESET in toast.get("text", "") for toast in result["toasts"]["host"]) and \
                    not any(REFUSED_PRESET in toast.get("text", "") for toast in result["toasts"]["client"])
        if not places_brains:
            # An activity that puts its own residents on the site never meets the synchronized editor, and
            # no placement crosses the wire for it.
            entered = {peer: json.loads((root / peer / "report.json").read_text(errors="replace")).get("entered_editor") for peer in runs}
            result["entered_editor"] = entered
            checks["activity_placed_its_own_brains"] = all(value is False for value in entered.values()) and                 all("[net-match] brain placement" not in log for log in logs.values())
        if places_brains:
            result["placements"] = score_placements(logs)
            checks["brains_placed_on_both_peers"] = result["placements"]["pass"]
            # The raw per-MO dump of every tick, held ones included, compared byte for byte beside the
            # subsystem hashes: it carries the fields the hashes leave out.
            host_dump, client_dump = root / "host/trace.json.simdump.txt", root / "client/trace.json.simdump.txt"
            checks["host_client_dump_bytes"] = host_dump.is_file() and client_dump.is_file() and host_dump.read_bytes() == client_dump.read_bytes()
            # The match must actually leave the setup editor and play, or the traces agree on nothing happening.
            checks["left_setup_editor"] = all("[net-match] brain placed: seat=1" in log for log in logs.values()) and \
                all(record.get("exit_code") == 0 for record in records.values())
        checks["shared_600_ticks"], result["simulation"] = strict_compare(root / "host/trace.json", root / "client/trace.json", 600)
        replay_trace = root / "replay/trace.json"
        replay = make_run(repo, ["-net-replay", str(root / "host/match.ccreplay"), "-tick-hashes", "-out", str(replay_trace),
                                 "-max-ticks", "600", "-seed", "42", "-num-lua-states", "4"], root / "replay", options.timeout,
                          env={"CCCP_HEADLESS": "1", "CC_SIM_DUMP": "1:600"}, expected=[replay_trace])
        # The playback runs the configuration the peers ran: a window size the peers did not have is a
        # different run, not a replay of theirs.
        if resolution:
            set_resolution(replay.cwd, *resolution)
        try:
            ledger("before_replay")
            result["replay_record"] = replay.start().finish()
            ledger("after_replay")
        finally:
            replay.close()
        checks["replay_process"] = result["replay_record"].get("exit_code") == 0 and not result["replay_record"].get("timed_out")
        checks["replay_exact"], result["replay_simulation"] = strict_compare(root / "host/trace.json", replay_trace, 600)
        # No raw-dump gate against the replay: a single-peer playback binds the other seat's actors to its own
        # controller, and the dump carries that mode. replay_exact compares the on-wire subsystems, which is
        # the comparison that means anything here.
        replay_log = (root / "replay/stdout.log").read_text(errors="replace")
        result["replay_rules"] = score_rules(replay_log, rules, default, options.dedicated)
        result["replay_seed"] = score_seed(replay_log, rules, default, options.dedicated)
        checks["replay_rules"] = result["replay_rules"]["pass"]
        checks["replay_seed"] = result["replay_seed"]["pass"]
        if options.variant == "census":
            # The offline arm: the same preset launched through the stock command line scenario path, whose
            # setup the match must match. A retained reference build is passed as --offline-repo.
            offline_repo = Path(getattr(options, "offline_repo", None) or repo).resolve()
            offline_trace = root / "offline/trace.json"
            offline = make_run(offline_repo, ["-scenario", rules["activity_preset"], "-max-ticks", "1", "-tick-hashes",
                                              "-out", str(offline_trace), "-seed", "42", "-num-lua-states", "4"],
                               root / "offline", options.timeout,
                               env={"CCCP_HEADLESS": "1", "CC_SIM_DUMP": "1:1"}, expected=[offline_trace])
            try:
                result["offline_record"] = offline.start().finish()
            finally:
                offline.close()
            offline_log = (root / "offline/stdout.log").read_text(errors="replace")
            checks["offline_process"] = result["offline_record"].get("exit_code") == 0 and not result["offline_record"].get("timed_out")
            result["census"] = census_compare(root / "offline/trace.json.simdump.txt", root / "host/trace.json.simdump.txt")
            result["census"]["offline_exe_sha256"] = sha(offline_repo / "Cortex Command.exe")
            # Evidence, not a gate: a reference build has no rules print, and the command line scenario path
            # never runs the technology combo the menu does, so its team tech stays unset.
            result["census"]["rules_rows"] = {"offline": observations(offline_log), "match": observations(logs["host"])}
            checks["census_tick1_dump"] = result["census"]["pass"]
        if options.baseline:
            for peer in ("host", "client", "replay"):
                left, right = options.baseline / peer / "trace.json.simdump.txt", root / peer / "trace.json.simdump.txt"
                checks[peer + "_default_dump_bytes"] = left.is_file() and right.is_file() and left.read_bytes() == right.read_bytes()
    result.update(checks=checks, passed=all(checks.values()), exe_sha256=exe_hash)
    (root / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    report_checks(checks, result["config_refusal"])
    return 0 if result["passed"] else 1
