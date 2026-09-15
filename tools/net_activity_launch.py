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
        # This activity expects a brain on the site; an empty one sends it into the setup editor, which
        # lockstep refuses, so the stock arm plays it on the site it ships with.
        rules["activity_preset"] = "Skirmish Defense"
        rules["scene_name"] = "Ketanot Hills"
    elif variant == "brains":
        # Skirmish Defense on a site with no brain on it: every human seat has to place its own brain in
        # the setup editor, which is the start a lockstep match has to synchronize.
        rules["activity_preset"] = "Skirmish Defense"
        rules["scene_name"] = "Grasslands"
    elif variant == "census":
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
    roster = [(peer, peer - 1 if default else 0, False, "Host" if peer == 1 else "Client")
              for peer in range(2 if dedicated else 1, 3)]
    if not default:
        roster.append((0, 1, True, "CPU"))
    payload = struct.pack("<HQBBHBBH", 3, 1, 1, 2, 3, mode, 2, int(dedicated))
    for value in (rules["activity_type"], rules["activity_preset"], rules["scene_name"], "PvP" if default else "CoopPvE"):
        payload += string(value)
    payload += struct.pack("<B", len(roster))
    for peer, team, cpu, name in roster:
        payload += struct.pack("<BBBB", peer, team, int(cpu), 0) + string(name)
    payload += struct.pack("<BQQ", 0, 1, 1)
    payload += string(rules["activity_module"]) + string(rules["scene_module"])
    payload += struct.pack("<BIBBB", rules["difficulty"], rules["starting_gold"], rules["fog_of_war"],
                           rules["require_clear_path_to_orbit"], rules["deploy_units"])
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


def placements(log):
    """Every applied placement a peer logged, keyed by seat: what its scene actually holds."""
    applied = {}
    for line in log.splitlines():
        if not line.startswith("[net-match] brain placed: "):
            continue
        row = dict(token.split("=", 1) for token in line.removeprefix("[net-match] brain placed: ").split(" ") if "=" in token)
        applied[row.get("seat")] = row
    return applied


def score_placements(logs, seats=2):
    """Both peers must apply every seat's placement, from that seat's own peer, identically."""
    applied = {peer: placements(log) for peer, log in logs.items()}
    expected_seats = [str(seat) for seat in range(seats)]
    complete = all(sorted(rows) == expected_seats for rows in applied.values())
    # Seat N is issued by peer N+1: a placement each peer computed locally would not carry the other's id.
    issuers = all(rows.get(seat, {}).get("peer") == str(index + 1)
                  for rows in applied.values() for index, seat in enumerate(expected_seats))
    identical = len({json.dumps(rows, sort_keys=True) for rows in applied.values()}) == 1
    return {"pass": bool(complete and issuers and identical), "complete": complete, "issuers": issuers,
            "identical": identical, "applied": applied}


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
    if not 48320 <= options.port <= 48539:
        raise ValueError("port must be in 48320..48539")
    os.environ["CCCP_HEADLESS"] = "1"
    repo, root = options.repo.resolve(), options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    rules = rules_for(options.variant)
    # The census launches the activity preset's own two seats, which is the roster the default config carries.
    default = options.variant in ("default", "census")
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
    common = ["-net-match-service-e2e", "-net-port", str(options.port), "-net-match-peers", "2",
              "-net-match-mode", "pvp" if default else "coop-pve", "-net-match-ticks", "600", "-max-ticks", "600",
              "-net-match-input-delay", "3", "-seed", "42", "-num-lua-states", "4", "-tick-hashes"]
    if options.variant == "brains":
        # Every peer stands in for its own players' DONE in the synchronized setup editor.
        common.append("-net-match-e2e-brain-placement")
    runs, records = {}, {}
    try:
        for peer in ("host", "client"):
            flags = ["-net-dedicated" if options.dedicated else "-net-host", "-net-match-service-config", str(config)] if peer == "host" else ["-net-join", "127.0.0.1"]
            trace, report = root / peer / "trace.json", root / peer / "report.json"
            flags += ["-out", str(trace), "-net-match-report", str(report), "-net-replay-out", str(root / peer / "match.ccreplay")]
            runs[peer] = make_run(repo, [*common, *flags], root / peer, options.timeout,
                                  env={"CCCP_HEADLESS": "1", "CC_SIM_DUMP": "1:600"}, expected=[report])
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
        result["rules"] = {peer: score_rules(log, rules, default) for peer, log in logs.items()}
        for peer in runs:
            checks[peer + "_process"] = records[peer].get("exit_code") == 0 and not records[peer].get("timed_out") and records[peer].get("evidence_complete", False)
            checks[peer + "_rules"] = result["rules"][peer]["pass"]
        if options.variant == "brains":
            result["placements"] = score_placements(logs)
            checks["brains_placed_on_both_peers"] = result["placements"]["pass"]
            # The match must actually leave the setup editor and play, or the traces agree on nothing happening.
            checks["left_setup_editor"] = all("[net-match] brain placed: seat=1" in log for log in logs.values()) and \
                all(record.get("exit_code") == 0 for record in records.values())
        checks["shared_600_ticks"], result["simulation"] = strict_compare(root / "host/trace.json", root / "client/trace.json", 600)
        replay_trace = root / "replay/trace.json"
        replay = make_run(repo, ["-net-replay", str(root / "host/match.ccreplay"), "-tick-hashes", "-out", str(replay_trace),
                                 "-max-ticks", "600", "-seed", "42", "-num-lua-states", "4"], root / "replay", options.timeout,
                          env={"CCCP_HEADLESS": "1", "CC_SIM_DUMP": "1:600"}, expected=[replay_trace])
        try:
            ledger("before_replay")
            result["replay_record"] = replay.start().finish()
            ledger("after_replay")
        finally:
            replay.close()
        checks["replay_process"] = result["replay_record"].get("exit_code") == 0 and not result["replay_record"].get("timed_out")
        checks["replay_exact"], result["replay_simulation"] = strict_compare(root / "host/trace.json", replay_trace, 600)
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
