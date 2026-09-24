"""Two lockstep peers play one fixture and must hold the same world every tick.

fence: the host walks its brain so its preview runs the stride hook of
tools/fixtures/preview_native_fence.lua, which writes a native property and an alias on the world's actor.
method: the same walk runs tools/fixtures/preview_method_fence.lua, whose stride hook resets the Timer the real
script keeps and adds a particle through MovableMan.
outparam: the same walk runs tools/fixtures/preview_outparam_fence.lua, whose stride hook casts a ray into the
file-scope Vector the real script keeps.
const: the same walk runs tools/fixtures/preview_const_mutator.lua, whose preview copy kills the enemies and ends the
activity, both const calls on managers.
argument: the same walk runs tools/fixtures/preview_argument_fence.lua, whose preview copy attaches a spare the real
script keeps.
nilchain: the same walk runs tools/fixtures/preview_nil_chain.lua, whose hook chains on a call a preview drops.
craft: the host's seat flies a landing craft (tools/fixtures/craft_handoff_activity.lua) that hands out its passenger.

Per-tick hashes are compared strictly on every tick both peers recorded, leaving out only the routing subsystem each
machine holds for its own seats, and every per-object simdump row must match outside the same per-peer seat fields;
the first row that differs is named.
"""

import argparse
import hashlib
import json
import re
import sys
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from compare_sim_traces import load_trace, strict_compare
from feel.retained_resume import PER_PEER_SUBSYSTEMS
from run_sim_test import make_run, engine_executable

PEERS = ("host", "client")
PORTS = (49720, 49739)
FIXTURES = Path(__file__).resolve().parent / "fixtures"
INDEX = "DataModule\n\tModuleName = User Scenes\n\tScanFolderContents = 1\n\tIgnoreMissingItems = 1\n"
CRAFT_ACTIVITY = ("\tAddActivity = GAScripted\n\t\tPresetName = Determinism Craft Handoff\n\t\tSceneName = Grasslands\n"
                  "\t\tScriptPath = UserScenes.rte/craft_handoff_activity.lua\n\t\tLuaClassName = CraftHandoff\n"
                  "\t\tTeamOfPlayer1 = 0\n\t\tTeamOfPlayer2 = 1\n\t\tMinTeamsRequired = 2\n\t\tIsTestActivity = 1\n"
                  "\t\tDefaultRequireClearPathToOrbit = 0\n\t\tDefaultFogOfWar = 0\n\t\tDefaultDeployUnits = 0\n")
CASES = {
    "fence": {"ticks": 600, "files": ["preview_native_fence.lua"], "index": "",
              "both": ["-test-script", "UserScenes.rte/preview_native_fence.lua", "-net-local-prediction", "on"],
              "host": ["-input-script", str(FIXTURES / "preview_native_fence.txt")]},
    "method": {"ticks": 600, "files": ["preview_method_fence.lua"], "index": "",
               "both": ["-test-script", "UserScenes.rte/preview_method_fence.lua", "-net-local-prediction", "on"],
               "host": ["-input-script", str(FIXTURES / "preview_native_fence.txt")]},
    "outparam": {"ticks": 600, "files": ["preview_outparam_fence.lua"], "index": "",
                 "both": ["-test-script", "UserScenes.rte/preview_outparam_fence.lua", "-net-local-prediction", "on"],
                 "host": ["-input-script", str(FIXTURES / "preview_native_fence.txt")]},
    "const": {"ticks": 600, "files": ["preview_const_mutator.lua"], "index": "",
              "both": ["-test-script", "UserScenes.rte/preview_const_mutator.lua", "-net-local-prediction", "on"],
              "host": ["-input-script", str(FIXTURES / "preview_native_fence.txt")]},
    "argument": {"ticks": 600, "files": ["preview_argument_fence.lua"], "index": "",
                 "both": ["-test-script", "UserScenes.rte/preview_argument_fence.lua", "-net-local-prediction", "on"],
                 "host": ["-input-script", str(FIXTURES / "preview_native_fence.txt")]},
    "nilchain": {"ticks": 600, "files": ["preview_nil_chain.lua"], "index": "",
                 "both": ["-test-script", "UserScenes.rte/preview_nil_chain.lua", "-net-local-prediction", "on"],
                 "host": ["-input-script", str(FIXTURES / "preview_native_fence.txt")]},
    "craft": {"ticks": 480, "files": ["craft_handoff_activity.lua"], "index": CRAFT_ACTIVITY,
              "both": ["-net-match-service-preset", "Determinism Craft Handoff", "-net-match-service-module", "UserScenes.rte"],
              "host": []},
}
# The simdump fields each machine holds for its own seats: the controller mode a local switch writes first, and the pie menu.
PER_PEER_FIELDS = frozenset({"mode", "pie"})
DESYNC = re.compile(r"^.*(?:\[lockstep\] desync at frame \d+|controller sync failed).*$", re.M)
HOLD = re.compile(r"^\[net-match\] hold peer=.*$", re.M)
FENCE = re.compile(r"\[preview-fence\] preview uid=(\d+) took=(\d+) kept_health=([-\d.]+)->([-\d.]+) kept_x=([-\d.]+)->([-\d.]+)")
METHOD = re.compile(r"\[preview-method\] preview uid=(\d+) timer_before=([-\d.]+) timer_after=([-\d.]+)")
MARK = " Spark Yellow 1 "
CONST = re.compile(r"\[preview-const\] preview uid=(\d+) running=(\w+) killed=(\w+)")
ARGUMENT = re.compile(r"\[preview-argument\] preview uid=(\d+) attached=(\w+)")
NILCHAIN = re.compile(r"\[preview-nil\] preview uid=(\d+)")
NIL_REPORT = re.compile(r"PREVIEW: \S*preview_nil_chain\.lua stopped a preview hook after the dropped call Vector:SetMagnitude")
NIL_ERROR = re.compile(r"ERROR: \S*preview_nil_chain\.lua")
OUTPARAM = re.compile(r"\[preview-outparam\] preview uid=(\d+) hit_before=([-\d.]+) hit_after=([-\d.]+)")
CRAFT = re.compile(r"\[craft-fixture\] (hatch opening|passenger out|passenger played) tick=(\d+)")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def peer_args(case: dict, who: str, port: int, root: Path) -> list:
    ticks = str(case["ticks"])
    args = ["-free-run-sim", "-net-match-service-e2e", "-net-port", str(port), "-net-match-peers", "2",
            "-net-match-ticks", ticks, "-max-ticks", ticks, "-net-match-input-delay", "3", "-net-autosave-seconds", "0",
            "-seed", "42", "-num-lua-states", "4", "-tick-hashes", "-out", str(root / who / "trace.json"),
            "-net-match-report", str(root / who / "report.json"), *case["both"]]
    if who == "host":
        args += case["host"]
    return args + (["-net-host"] if who == "host" else ["-net-join", "127.0.0.1"])


def stage_module(runtime: Path, case: dict) -> None:
    module = Path(runtime) / "Userdata/UserScenes.rte"
    module.mkdir(parents=True, exist_ok=True)
    (module / "Index.ini").write_text(INDEX + case["index"], encoding="utf-8")
    for name in case["files"]:
        (module / name).write_bytes((FIXTURES / name).read_bytes())


def peer_text(root: Path, who: str) -> str:
    text = ""
    for path in (root / who / "stdout.log", root / who / "runtime" / "LogConsole.txt"):
        if path.exists():
            text += path.read_text(encoding="utf-8-sig", errors="replace")
    return text


def run_pair(repo: Path, root: Path, case: dict, port: int, timeout: float) -> dict:
    root.mkdir(parents=True, exist_ok=False)
    runs, records = {}, {}
    try:
        for who in PEERS:
            env = {"CCCP_HEADLESS": "1", "CC_SIM_DUMP": f"1:{case['ticks']}"}
            runs[who] = make_run(repo, peer_args(case, who, port, root), root / who, timeout, env=env)
            stage_module(runs[who].cwd, case)

        def drive(who):
            try:
                records[who] = runs[who].start().finish()
            except Exception as error:
                records[who] = {"error": repr(error)}

        threads = [threading.Thread(target=drive, args=(who,), daemon=True) for who in PEERS]
        threads[0].start()
        threading.Event().wait(2.0)
        threads[1].start()
        for thread in threads:
            thread.join()
    finally:
        for run in runs.values():
            run.close()
    return records


def load_dump(path: Path) -> dict:
    ticks = {}
    with Path(path).open("r", encoding="utf-8", errors="replace") as source:
        for line in source:
            head = line.split(" ", 1)[0]
            if head.isdigit():
                ticks.setdefault(int(head), []).append(line.rstrip("\n"))
    return ticks


def shared_fields(line: str) -> list:
    return [token for token in line.split(" ") if token.split("=", 1)[0] not in PER_PEER_FIELDS]


def first_object_divergence(host_dump: Path, client_dump: Path) -> dict:
    """The first per-object row the two dumps disagree on outside the per-peer seat fields."""
    if not Path(host_dump).exists() or not Path(client_dump).exists():
        return {"available": False, "reason": "a simdump is missing"}
    left, right = load_dump(host_dump), load_dump(client_dump)
    common = sorted(set(left) & set(right))
    for tick in common:
        if len(left[tick]) != len(right[tick]):
            return {"available": True, "tick": tick, "census_differs": True, "host_rows": len(left[tick]), "client_rows": len(right[tick])}
        for host_line, client_line in zip(left[tick], right[tick]):
            host_tokens, client_tokens = shared_fields(host_line), shared_fields(client_line)
            if host_tokens != client_tokens:
                fields = [{"host": a, "client": b} for a, b in zip(host_tokens, client_tokens) if a != b]
                return {"available": True, "tick": tick, "census_differs": False,
                        "object": " ".join(host_line.split(" ")[1:4]), "fields": fields[:12]}
    return {"available": True, "tick": None, "identical": True, "common_ticks": len(common)}


def count_rows(path: Path, needle: str) -> int:
    if not Path(path).exists():
        return 0
    with Path(path).open("r", encoding="utf-8", errors="replace") as source:
        return sum(1 for line in source if needle in line)


def score(root: Path, case_name: str, exe_sha256: str, records: dict) -> dict:
    case = CASES[case_name]
    result = {"case": case_name, "ticks": case["ticks"], "exe_sha256": exe_sha256, "per_peer_excluded": sorted(PER_PEER_SUBSYSTEMS),
              "records": {who: {key: record.get(key) for key in ("pid", "exit_code", "timed_out", "error")} for who, record in records.items()}}
    result["processes"] = {who: bool(records.get(who, {}).get("exit_code") == 0 and not records.get(who, {}).get("timed_out")) for who in PEERS}
    traces = {who: root / who / "trace.json" for who in PEERS}
    texts = {who: peer_text(root, who) for who in PEERS}
    result["desync_lines"] = {who: DESYNC.findall(texts[who])[:4] for who in PEERS}
    # A seat held for being slow replays through a rejoin, so its trace is no longer one live pass.
    result["hold_lines"] = {who: HOLD.findall(texts[who])[:4] for who in PEERS}
    try:
        lengths = {who: len(load_trace(traces[who])[0]) for who in PEERS}
    except (OSError, ValueError) as error:
        lengths = None
        passed, comparison = False, {"reasons": [f"trace unreadable as one pass: {error}"]}
    if lengths:
        result["trace_ticks"] = lengths
        # Every tick both peers recorded is compared; a full run must also reach the case's length on both.
        passed, comparison = strict_compare(traces["host"], traces["client"], min(lengths.values()), prefix=True, per_peer=PER_PEER_SUBSYSTEMS)
        passed = passed and all(length == case["ticks"] for length in lengths.values())
    result["simulation"] = comparison
    result["objects"] = first_object_divergence(root / "host" / "trace.json.simdump.txt", root / "client" / "trace.json.simdump.txt")
    if case_name == "fence":
        rows = [dict(zip(("uid", "took", "health_before", "health_after", "x_before", "x_after"), match)) for match in FENCE.findall(texts["host"])]
        result["previews"] = {"hook_runs": len(rows), "rows": rows[:12],
                              "clone_took_both": sum(1 for row in rows if row["took"] == "3"),
                              "kept_unchanged": sum(1 for row in rows if row["health_before"] == row["health_after"] and row["x_before"] == row["x_after"])}
        # A preview that never ran the hook proves nothing, and every run must leave the world's actor as it was.
        fixture_ok = bool(rows) and result["previews"]["clone_took_both"] == len(rows) and result["previews"]["kept_unchanged"] == len(rows)
    elif case_name == "method":
        rows = [dict(zip(("uid", "before", "after"), match)) for match in METHOD.findall(texts["host"])]
        marks = {who: count_rows(root / who / "trace.json.simdump.txt", MARK) for who in PEERS}
        result["previews"] = {"hook_runs": len(rows), "rows": rows[:12], "timer_kept": sum(1 for row in rows if row["before"] == row["after"])}
        result["commit_marks"] = marks
        # The preview's reset and particle are dropped, and the committed hook's particles stand on both peers alike.
        fixture_ok = bool(rows) and result["previews"]["timer_kept"] == len(rows) and marks["host"] > 0 and marks["host"] == marks["client"]
    elif case_name == "const":
        rows = [dict(zip(("uid", "running", "killed"), match)) for match in CONST.findall(texts["host"])]
        result["previews"] = {"hook_runs": len(rows), "rows": rows[:12],
                              "dropped": sum(1 for row in rows if row["running"] == "true" and row["killed"] == "nil")}
        # The preview's kill and end are dropped, so nobody dies and the activity runs on after every preview.
        fixture_ok = bool(rows) and result["previews"]["dropped"] == len(rows)
    elif case_name == "argument":
        rows = [dict(zip(("uid", "attached"), match)) for match in ARGUMENT.findall(texts["host"])]
        result["previews"] = {"hook_runs": len(rows), "rows": rows[:12], "never_attached": sum(1 for row in rows if row["attached"] == "false")}
        # The preview's attach of the kept spare is dropped, so the spare is never taken by a preview's copy.
        fixture_ok = bool(rows) and result["previews"]["never_attached"] == len(rows)
    elif case_name == "nilchain":
        rows = NILCHAIN.findall(texts["host"])
        reports = {who: len(NIL_REPORT.findall(texts[who])) for who in PEERS}
        errors = {who: len(NIL_ERROR.findall(texts[who])) for who in PEERS}
        result["previews"] = {"hook_runs": len(rows), "reports": reports, "errors": errors}
        # Every preview run of the hook fails on the dropped call's nil; it is reported once, and the real hook never fails.
        fixture_ok = len(rows) > 1 and reports["host"] == 1 and reports["client"] == 0 and errors["host"] == 0 and errors["client"] == 0
    elif case_name == "outparam":
        rows = [dict(zip(("uid", "before", "after"), match)) for match in OUTPARAM.findall(texts["host"])]
        result["previews"] = {"hook_runs": len(rows), "rows": rows[:12], "value_kept": sum(1 for row in rows if row["before"] == row["after"])}
        # The preview's cast writes a copy, so the Vector the real script keeps reads the same after it.
        fixture_ok = bool(rows) and result["previews"]["value_kept"] == len(rows)
    else:
        events = {who: [(name, int(tick)) for name, tick in CRAFT.findall(texts[who])] for who in PEERS}
        result["craft"] = events
        out = [tick for name, tick in events["host"] if name == "passenger out"]
        played = {who: [tick for name, tick in events[who] if name == "passenger played"] for who in PEERS}
        # The seat took the passenger on every peer at the same tick, and the comparison runs 200 ticks past the exit.
        fixture_ok = bool(out) and all(played.values()) and played["host"] == played["client"] and case["ticks"] >= out[0] + 200
    result["fixture_ok"] = fixture_ok
    # The tick hash leaves out some per-object state (limb transforms, actor timers), so every dumped row must match too.
    objects_identical = result["objects"].get("identical") is True
    result["passed"] = bool(passed and objects_identical and fixture_ok and all(result["processes"].values())
                            and not any(result["desync_lines"].values()) and not any(result["hold_lines"].values()))
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("case", choices=sorted(CASES))
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=PORTS[0])
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--score-only", action="store_true", help="score an existing run directory without launching")
    options = parser.parse_args()
    if not PORTS[0] <= options.port <= PORTS[1]:
        parser.error(f"port outside this driver's block {PORTS[0]}-{PORTS[1]}")
    case = CASES[options.case]
    exe = engine_executable(options.repo)
    if options.score_only:
        previous = json.loads((options.out / "result.json").read_text(encoding="utf-8"))
        identity, records, unchanged = previous["exe_sha256"], previous["records"], previous.get("binary_unchanged")
    else:
        identity = sha256_file(exe)
        records = run_pair(options.repo, options.out, case, options.port, options.timeout)
        unchanged = sha256_file(exe) == identity
    result = score(options.out, options.case, identity, records)
    result["binary_unchanged"] = unchanged
    result["passed"] = bool(result["passed"] and unchanged)
    (options.out / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    comparison = result["simulation"]
    print(f"{'PASS' if result['passed'] else 'FAIL'} {options.case}: compared={comparison.get('compared_ticks')} "
          f"first_divergence={comparison.get('first_divergence')} subsystems={comparison.get('divergent_subsystems')} "
          f"trace_ticks={result.get('trace_ticks')} fixture_ok={result['fixture_ok']} processes={result['processes']} "
          f"desync={result['desync_lines']} holds={result['hold_lines']}")
    objects = result["objects"]
    if objects.get("available") and objects.get("tick"):
        print(f"first differing object: tick {objects['tick']} {objects.get('object', 'census')} {json.dumps(objects.get('fields', []))}")
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
