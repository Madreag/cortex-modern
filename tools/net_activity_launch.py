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
from run_sim_test import make_run


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
        # The site this activity ships with. Its seats still meet the setup editor, so the arm drives it.
        rules["activity_preset"] = "Skirmish Defense"
        rules["scene_name"] = "Ketanot Hills"
    elif variant in ("brains", "brains-auto", "hold-desync", "hold-resync", "resync-skirmish", "brains-longname", "wire-refusal"):
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


def encode_config(rules, dedicated=False, default=False):
    """Encode the existing L12 MatchConfig packet consumed by NetLobbyProtocol."""
    def string(value):
        encoded = value.encode("utf-8")
        return struct.pack("<H", len(encoded)) + encoded
    mode = 1 if default else 2
    roster = [(peer, peer - 1 if default else 0, False, "Host" if peer == 1 else rules.get("client_name", "Client"))
              for peer in range(2 if dedicated else 1, 3)]
    if not default:
        roster.append((0, 1, True, "CPU"))
    payload = struct.pack("<HQBBHBBH", 4, 1, 1, 2, 3, mode, 2, int(dedicated))
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
    return struct.pack("<IHHHHI", 0x344C4343, 4, 16, 3, 0, len(payload)) + payload


def observations(log):
    return [dict(token.split("=", 1) for token in shlex.split(line.removeprefix("[e2e] rules ")))
            for line in log.splitlines() if line.startswith("[e2e] rules ")]


def expected_observation(rules, default=False):
    expected = dict(tick="1", difficulty=str(rules["difficulty"]), gold=str(rules["starting_gold"]),
                    fog=str(int(rules["fog_of_war"])), orbit=str(int(rules["require_clear_path_to_orbit"])),
                    deploy=str(int(rules["deploy_units"])), cpu_team="-1" if default else "1",
                    activity=rules["activity_module"] + "/" + rules["activity_preset"],
                    scene=rules["scene_module"] + "/" + rules["scene_name"])
    for index, team in enumerate(rules["teams"]):
        expected[f"team{index}.tech"] = team["technology_module"] or "-All-"
        expected[f"team{index}.ai"] = str(team["ai_skill"])
    return expected


def score_rules(log, rules, default=False):
    rows, expected = observations(log), expected_observation(rules, default)
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
PORT_BLOCKS = ((48320, 48539), (48630, 48639))


# Each peer holds one seat and places a different brain at a different spot, so a placement that failed to
# cross the wire leaves the peers holding different residents.
EDITOR_SEATS = {"host": dict(player=0, x_fraction=0.30, cls="Actor", preset="Brain Case"),
                "client": dict(player=1, x_fraction=0.70, cls="AHuman", preset="Brain Robot")}
# Exactly 64 characters, the longest lobby name a seat can carry, so the strip's ellipsis is exercised.
LONG_SEAT_NAME = "Client" + "GunnhildrTheVeryPatientBrainPlacerOfKetanotHills" + "X" * 10
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


def wait_banner(resolution):
    return COMPACT_WAIT_BANNER if resolution and resolution[1] < COMPACT_MAX_HEIGHT else WAIT_BANNER


def compact(resolution):
    return bool(resolution) and resolution[1] < COMPACT_MAX_HEIGHT


def shots(peer, name):
    """The overlay layer (the strip and toasts) and the composited frame (world, editor and overlay)."""
    return [{"op": "screenshot", "name": f"{peer}_{name}"},
            {"op": "screenshot", "name": f"{peer}_{name}_world", "composited": True}]


def editor_script(peer, capture, place_after, finish_at_ready=False, wire_refusal=False, resolution=None):
    """The UI probe script that drives this peer's own seat through the setup editor, the way a player does."""
    seat = EDITOR_SEATS[peer]
    player = seat["player"]
    # The stock picker slides in with the editor, and the seat's own message band rides the top rows: the
    # network overlay has to be clear of both. The renders wait is past the picker's slide at any speed.
    steps = [{"op": "wait", "service": "Running"}, {"op": "wait", "editing": True},
             {"op": "wait", "player": player, "picker_open": True}, {"op": "wait", "renders": 12},
             {"op": "assert_net_ui_clear", "player": player, "picker_open": True, "screen_text": True}]
    if compact(resolution):
        # An open picker leaves the strip under half a short screen: the metrics tail is what gives way,
        # the seat names stay whole while it can, and the count outlives every fallback.
        steps += [{"op": "assert_control", "control": "LabelNetMatchStatus", "equals": {"visible": True}, "fits": True,
                   "text_contains": "WAITING FOR " + PLACEMENT_NAMES + " TO PLACE"},
                  {"op": "assert_control", "control": "LabelNetMatchStatus", "equals": {"visible": True}, "fits": True,
                   "text_contains": "0 of 2"}]
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
    steps += [{"op": "editor_place_brain", "player": player, "x_fraction": seat["x_fraction"],
               "class": seat["cls"], "preset": seat["preset"], "module": "Base.rte"},
              # Placing alone commits nothing: the wire only carries the seat's DONE.
              {"op": "assert_editor", "player": player, "equals": {"resident": True, "ready": False, "submitted": False}},
              {"op": "editor_done", "player": player},
              {"op": "wait", "seat_ready": player}]
    # The frame the seat's own ready lands on is the one that still says all brains are placed: the editor
    # leaves a tick or two later, so the shot goes before the assertion that reads the same state.
    if capture and peer != "host":
        steps += shots(peer, "ready")
    steps += [{"op": "assert_editor", "player": player, "equals": {"ready": True, "submitted": True}}]
    if peer == "host":
        # The host places first, so while the client is still placing its screen must carry the stock READY
        # line and its own strip must name who the held world is waiting for.
        steps += [{"op": "assert", "equals": {"editing": True}},
                  {"op": "wait", "player": player, "seat_text_contains": READY_TEXT},
                  {"op": "assert_control", "control": "LabelNetMatchStatus", "text_contains": wait_banner(resolution),
                   "equals": {"visible": True}, "fits": True}]
        if capture:
            steps += shots(peer, "waiting_banner")
    if not finish_at_ready:
        # The arm that plays on: the match leaves the editor and runs.
        steps += [{"op": "wait", "editing": False}, {"op": "wait", "sim_at_least": 200}]
        if capture:
            steps += shots(peer, "match_started")
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


def census_compare(offline_dump, match_dump):
    left, right = tick_lines(offline_dump), tick_lines(match_dump)
    if left is None or right is None:
        return {"pass": False, "reason": "a census dump is missing", "offline_dump": str(offline_dump), "match_dump": str(match_dump)}
    first = next((index for index, (a, b) in enumerate(zip(left, right)) if a != b), None)
    if first is None and len(left) != len(right):
        first = min(len(left), len(right))
    return {"pass": left == right, "offline_lines": len(left), "match_lines": len(right),
            "first_difference": None if first is None else {
                "index": first,
                "offline": left[first] if first < len(left) else None,
                "match": right[first] if first < len(right) else None}}


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
    config.write_bytes(encode_config(rules, options.dedicated, default))
    exe_hash = sha(repo / "Cortex Command.exe")
    manifest = dict(stamp=stamp(), repo=str(repo), exe_sha256=exe_hash, rules=rules, variant=options.variant,
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
    editor_driven = options.variant in ("brains", "stock", "hold-desync", "hold-resync", "resync-skirmish",
                                       "brains-longname", "wire-refusal")
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
    try:
        for peer in ("host", "client"):
            flags = ["-net-dedicated" if options.dedicated else "-net-host", "-net-match-service-config", str(config)] if peer == "host" else ["-net-join", "127.0.0.1"]
            trace, report = root / peer / "trace.json", root / peer / "report.json"
            flags += ["-out", str(trace), "-net-match-report", str(report), "-net-replay-out", str(root / peer / "match.ccreplay")]
            env = {"CCCP_HEADLESS": "1", "CC_SIM_DUMP": "1:600"}
            if editor_driven:
                # The client holds the world in the editor long enough for the hold itself to be under test.
                script = root / (peer + "-ui") / "ui-script.json"
                script.parent.mkdir(parents=True, exist_ok=False)
                delay = 0 if options.variant == "resync-skirmish" else ((90 if hold_desync else 45) if peer == "client" else 0)
                script.write_text(json.dumps(editor_script(peer, captures, delay, hold_desync and not hold_resync,
                                                           options.variant == "wire-refusal", resolution), indent=2), encoding="utf-8")
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
    checks, result = {}, {"records": records}
    for peer, log in logs.items():
        for number, line in enumerate(log.splitlines(), 1):
            if line.startswith("[e2e] rules ") or "setup failed:" in line:
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
            for name, passed in checks.items():
                print(f"{'PASS' if passed else 'FAIL'} {name}")
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
            for name, passed in checks.items():
                print(f"{'PASS' if passed else 'FAIL'} {name}")
            return 0 if result["passed"] else 1
        result["rules"] = {peer: score_rules(log, rules, default) for peer, log in logs.items()}
        for peer in runs:
            checks[peer + "_process"] = records[peer].get("exit_code") == 0 and not records[peer].get("timed_out") and records[peer].get("evidence_complete", False)
            checks[peer + "_rules"] = result["rules"][peer]["pass"]
        if editor_driven:
            # Every commit came off the seat's own editor, not the deterministic-spot helper.
            result["commits"] = {peer: committed(log) for peer, log in logs.items()}
            checks["brains_via_editor"] = all(rows and all(row.get("via") == "editor" for row in rows.values())
                                              for rows in result["commits"].values())
            result["probes"] = {peer: probe_result(root, peer) for peer in runs}
            checks["ui_probe_pass"] = all(result["probes"][peer].get("pass") and result["probes"][peer].get("complete") for peer in runs)
            # The refusal the production path shows a player who presses DONE with no brain placed: the seat
            # stays unready and uncommitted, and the stock editor asks for the brain again.
            # The DONE-refusal assertion names itself, so the arm reads that step and no other.
            def refusal_rows(probe):
                named = [index for index, step in enumerate(probe.get("script", {}).get("steps", []))
                         if step.get("name") == "done_refusal"]
                return next((step["observed"]["editor_seats"] for step in probe.get("steps", [])
                             if step["index"] in named), [])
            result["refusals"] = {peer: refusal_rows(result["probes"][peer]) for peer in runs}
            checks["refusal_keeps_seat_unready"] = all(
                rows and all(not row["ready"] and not row["submitted"] and not row["resident"] and
                             any(text in row["screen_text"] for text in PLACE_REFUSED) for row in rows)
                for rows in result["refusals"].values())
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
        result["replay_rules"] = score_rules((root / "replay/stdout.log").read_text(errors="replace"), rules, default)
        checks["replay_rules"] = result["replay_rules"]["pass"]
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
    for name, passed in checks.items():
        print(f"{'PASS' if passed else 'FAIL'} {name}")
    return 0 if result["passed"] else 1
