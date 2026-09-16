"""Execute the reserved-machine verification phases after the explicit build resume."""
from __future__ import annotations

import argparse
import contextlib
import importlib.util
import json
import os
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import sys
import threading
import time

from run_pair import sha, stamp

REPO = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
ROOT = Path("D:/mx/astra-f40-root-fix-20260914")
EXE = REPO / "Cortex Command.exe"
RED_EXE = Path("D:/mx/opus-f40-20260914/bin/fix3-564f8400.exe")
REFERENCE_EXE = Path("D:/mx/opus-post-match-20260913/build/control/Cortex Command.exe")
RED_SHA = "564f8400b99ba4375cb59c67d8ef21bc445c2874840dd2e2c800235fd4b08504"
REFERENCE_SHA = "f538d3de512396805d20e2441006bc332779ac4f4fdbb6be6ed5128d4de0966d"
PIE_CASES = ("next", "prev", "goto", "actor_cancel", "delivery_cancel")
WRITE_CASES = ("buy_menu", "form_squad", "full_inventory")
AK47 = Path("D:/Projects/stage2_p4/fixtures/ak47_fire.ccreplay")
AK47_INPUT = AK47.with_suffix(".txt")
PIE_INPUT = Path("D:/Projects/reviews/takeover-20260909/grok-workers/pie-close-lockstep-20260913/fixtures/pie_open_then_next.txt")
FATAL = re.compile(r"^.*(?:ERROR:|RTE Assert|RTE Abort|stack traceback|\[Test\].*: FAIL).*$", re.M)
LOCK = threading.Lock()


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2), encoding="utf-8")


def footprint():
    size = 0
    for directory, folders, files in os.walk(ROOT):
        folders[:] = [name for name in folders if not os.lstat(Path(directory) / name).st_file_attributes & 0x400]
        for name in files:
            try:
                size += (Path(directory) / name).stat().st_size
            except FileNotFoundError:
                pass
    if size >= 5_000_000_000:
        raise RuntimeError("scratch footprint reached 5 GB")
    return size


def ledger(event, **values):
    row = {"stamp": stamp(), "event": event, **values}
    with LOCK, (ROOT / "exe-ledger.jsonl").open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(row) + "\n")


def swap(source, expected):
    if sha(source) != expected:
        raise RuntimeError("retained executable hash differs: " + str(source))
    ledger("before_swap", source=str(source), source_sha256=expected, destination=str(EXE), destination_sha256=sha(EXE))
    shutil.copy2(source, EXE)
    actual = sha(EXE)
    ledger("after_swap", destination=str(EXE), destination_sha256=actual)
    if actual != expected:
        raise RuntimeError("executable swap did not preserve bytes")


def fixtures():
    names = subprocess.check_output(["git", "ls-files", "Data/Tests.rte", "Data/Base.rte/Activities/P4AlphaDuel.lua",
        "tools/pie_lockstep/fixtures", "tools/pie_writes/fixtures"], cwd=REPO, text=True).splitlines()
    result = {name: sha(REPO / name) for name in names}
    result[str(PIE_INPUT)] = sha(PIE_INPUT)
    result[str(AK47)] = sha(AK47)
    result[str(AK47_INPUT)] = sha(AK47_INPUT)
    return result


def activities():
    text = (REPO / "Data/Tests.rte/Activities.ini").read_text(encoding="utf-8")
    return re.findall(r"^\s*PresetName = (.+)$", text, re.M)


def instrument_runner(expected):
    sys.path.insert(0, str(REPO / "tools"))
    import run_sim_test
    if getattr(run_sim_test, "seat_facts_instrumented_sha", None) == expected:
        return run_sim_test
    original = run_sim_test.make_run

    def make(*args, **kwargs):
        run = original(*args, **kwargs)
        start, finish = run.start, run.finish

        def check(event):
            actual = sha(run.argv[0])
            ledger(event, exe=str(run.argv[0]), sha256=actual, cwd=str(run.cwd), scratch_bytes=footprint())
            if Path(run.argv[0]).resolve() != EXE.resolve() or actual != expected:
                raise RuntimeError("run executable differs from the pinned ruled path")

        def begin():
            check("before_instance")
            return start()

        def end():
            try:
                return finish()
            finally:
                check("after_instance")

        run.start, run.finish = begin, end
        return run

    run_sim_test.make_run = make
    run_sim_test.seat_facts_instrumented_sha = expected
    return run_sim_test


def invoke(script, argv, out, require_zero=False):
    command = [sys.executable, str(script), *map(str, argv)]
    ledger("driver", command=command, log=str(out))
    previous = sys.argv
    try:
        sys.argv = command[1:]
        with Path(out).open("w", encoding="utf-8") as stream, contextlib.redirect_stdout(stream), contextlib.redirect_stderr(stream):
            try:
                if Path(script).resolve() == (REPO / "tools/run_sim_test.py").resolve():
                    import run_sim_test
                    code = run_sim_test.main()
                else:
                    runpy.run_path(str(script), run_name="__main__")
                    code = 0
            except SystemExit as error:
                code = error.code or 0
    finally:
        sys.argv = previous
    print(f"{script.name}: exit={code} log={out}", flush=True)
    row = {"command": command, "exit": code, "log": str(out)}
    if require_zero and code not in (0, None):
        raise RuntimeError(f"{Path(script).name} exited {code}; comparison did not complete: {out}")
    return row


def single(runner, out, flags, env=None, install=None):
    trace = out / "trace.json"
    run = runner.make_run(REPO, [*flags, "-out", str(trace)], out, timeout=300,
                          env={"CCCP_HEADLESS": "1", **(env or {})}, expected=[trace])
    if install:
        install(Path(run.cwd) / "Userdata/UserScenes.rte")
    try:
        result = run.start().finish()
    finally:
        run.close()
    logs = (out / "stdout.log").read_text(errors="replace")
    console = Path(run.cwd) / "LogConsole.txt"
    if console.exists():
        logs += "\n" + console.read_text(errors="replace")
    summary = {"pass": result.get("exit_code") == 0 and result.get("evidence_complete") is True and not FATAL.search(logs),
               "launch": str(out / "launch.json"), "errors": FATAL.findall(logs), "exe_sha256": result.get("exe_sha256")}
    write_json(out / "fixture-result.json", summary)
    print(f"{out.name}: process_and_errors={summary['pass']}", flush=True)
    return summary


def install_sp(module):
    module.mkdir(exist_ok=True)
    (module / "PieSwitchSP.lua").write_bytes((REPO / "tools/pie_lockstep/fixtures/PieSwitchSP.lua").read_bytes())
    (module / "Index.ini").write_text("DataModule\n\tModuleName = User Scenes\n\tScanFolderContents = 1\n\tIgnoreMissingItems = 1\n"
        "\tAddActivity = GAScripted\n\t\tPresetName = Determinism PieSwitchSP\n\t\tSceneName = Grasslands\n"
        "\t\tScriptPath = UserScenes.rte/PieSwitchSP.lua\n\t\tLuaClassName = PieSwitchSP\n\t\tMinTeamsRequired = 1\n"
        "\t\tIsTestActivity = 1\n\t\tDefaultRequireClearPathToOrbit = 0\n\t\tDefaultFogOfWar = 0\n\t\tDefaultDeployUnits = 0\n", encoding="utf-8")


def alias_activity(preset):
    def install(module):
        module.mkdir(exist_ok=True)
        (module / "Index.ini").write_text("DataModule\n\tModuleName = User Scenes\n\tAddActivity = GAScripted\n"
            f"\t\tCopyOf = {preset}\n\t\tPresetName = Determinism {preset}\n", encoding="utf-8")
    return install


def pie(runner, root, case, port, sp=False, writes=False):
    root.mkdir()
    folder = REPO / "tools" / ("pie_writes" if writes else "pie_lockstep") / "fixtures"
    observer = folder / ("PieWriteObserver.lua" if writes else "PieObserver.lua")
    input_file = folder / (case + ".txt")
    manifest = {"case": case, "sp": sp, "exe": str(EXE), "exe_sha256": sha(EXE), "port": port,
                "fixture_sha256": sha(input_file), "observer_sha256": sha(observer), "driver_sha256": sha(__file__),
                "input_delay": 0 if sp else 3, "observe": True, "debug": False,
                "setup_sha256": sha(REPO / "tools/pie_lockstep/fixtures/PieSwitchSP.lua")}
    write_json(root / "manifest.json", manifest)
    runs, records = [], {}
    try:
        for peer in (("sp",) if sp else ("host", "client")):
            trace = root / (peer + "_trace.json")
            flags = ["-seed", "42", "-max-ticks", "320", "-tick-hashes", "-num-lua-states", "4", "-out", str(trace),
                     "-test-script", "UserScenes.rte/" + observer.name]
            if sp:
                flags += ["-scenario", "PieSwitchSP"]
            else:
                flags += ["-free-run-sim", "-net-match-service-e2e", "-net-port", str(port), "-net-match-ticks", "320",
                          "-net-match-input-delay", "3", "-net-match-report", str(root / (peer + "_report.json"))]
                flags += ["-net-host"] if peer == "host" else ["-net-join", "127.0.0.1"]
                if peer == "host":
                    flags += ["-net-match-e2e-spawn", "ACrab:Crab:Base.rte:920:760:20"]
            if peer != "client":
                flags += ["-input-script", str(input_file)]
            run = runner.make_run(REPO, flags, root / peer, timeout=180,
                env={"CCCP_HEADLESS": "1", "CC_SIM_DUMP": "27:320", "PATH": str(REPO) + os.pathsep + os.environ.get("PATH", "")},
                expected=[trace, Path(str(trace) + ".simdump.txt")])
            module = Path(run.cwd) / "Userdata/UserScenes.rte"
            install_sp(module)
            if not sp:
                (module / "Index.ini").write_text("DataModule\n\tModuleName = User Scenes\n\tScanFolderContents = 1\n\tIgnoreMissingItems = 1\n", encoding="utf-8")
            (module / observer.name).write_bytes(observer.read_bytes())
            (module / "PieCase.lua").write_text("return " + json.dumps(case) + "\n", encoding="utf-8")
            runs.append((peer, run))
            run.start()
            if peer == "host":
                time.sleep(0.75)
        for peer, run in runs:
            result = run.finish()
            records[peer] = {key: result.get(key) for key in ("pid", "exit_code", "timed_out", "evidence_complete", "verdict_lines")}
    finally:
        for _, run in runs:
            run.close()
    write_json(root / "run_result.json", records)
    return records


def compatibility(runner, root):
    results = {}
    for preset in activities():
        name = preset.removeprefix("Determinism ").replace(" ", "_")
        results[name] = single(runner, root / name, ["-scenario", preset, "-seed", "42", "-max-ticks", "1400", "-tick-hashes", "-num-lua-states", "4"],
            install=None if preset.startswith("Determinism ") else alias_activity(preset))
    results["p4_sp"] = single(runner, root / "p4_sp", ["-scenario", "P4 Alpha Duel", "-seed", "42", "-max-ticks", "600", "-tick-hashes", "-num-lua-states", "4"], install=alias_activity("P4 Alpha Duel"))
    results["sp_identity"] = single(runner, root / "sp_identity", ["-scenario", "PieSwitchSP", "-seed", "42", "-max-ticks", "320",
        "-tick-hashes", "-num-lua-states", "4", "-input-script", str(PIE_INPUT)], {"CC_SIM_DUMP": "27:320"}, install_sp)
    results["ak47"] = single(runner, root / "ak47", ["-net-replay", str(AK47), "-tick-hashes", "-max-ticks", "700", "-input-script", str(AK47_INPUT)])
    for name in ("PreviewCompat", "PreviewModuleCompat", "F15Retirement/LateStateGain"):
        results[name] = single(runner, root / name.replace("/", "_"), ["-net-replay", str(AK47), "-tick-hashes", "-max-ticks", "700",
            "-input-script", str(AK47_INPUT), "-test-script", "Tests.rte/" + name + ".lua", "-local-prediction-invariance", "200:1:1", "-lpinv-overlay-links", "l"])
    for case in PIE_CASES:
        for sp in (True, False):
            name = "pie_" + case + ("_sp" if sp else "_net")
            results[name] = pie(runner, root / name, case, 43580, sp)
        results[case + "_detector"] = invoke(REPO / "tools/pie_lockstep/verify_pie_close.py",
            [case, root / ("pie_" + case + "_net"), "--reference", root / ("pie_" + case + "_sp"), "--out", root / (case + "_score.json")], root / (case + "_score.log"))
    for case in WRITE_CASES:
        results["writes_" + case] = pie(runner, root / ("writes_" + case), case, 43581, writes=True)
        results[case + "_detector"] = invoke(REPO / "tools/pie_writes/verify_peer_pie.py",
            [case, root / ("writes_" + case), "--out", root / (case + "_score.json")], root / (case + "_score.log"))
    write_json(root / "compatibility.json", results)


def pair_arms(root, reference=False):
    for offset, arm in enumerate(("reseat", "lua", "damage", "snapshot", "screens")):
        if reference and arm in ("reseat", "damage"):
            continue
        invoke(HERE / "run_pair.py", [arm, root / arm, "--port", 43570 + offset], root / (arm + ".log"),
               require_zero=True)
        pair = root / arm / "e2e/snapshot_p5"
        saves = [pair / peer / "runtime/Userdata/UserSavedGames.rte" / f"p5snap_p{number}.ccsave"
                 for number, peer in enumerate(("host", "client"), 1)]
        if not all(path.is_file() for path in saves):
            raise RuntimeError("missing pair snapshot for world compare: " + arm)
        invoke(HERE / "check_world.py", [*saves, "--out", root / arm / "world-result.json", *(["--damage"] if arm == "damage" else [])], root / (arm + "_world.log"))


def cpu_arms(root):
    results = {}
    for arm, port in (("stock", 43589), ("p4", 43590), ("script", 43586)):
        invoke(HERE / "run_cpu.py", [arm, root / ("cpu_" + arm), "--port", port], root / ("cpu_" + arm + ".log"))
        results[arm] = json.loads((root / ("cpu_" + arm) / "cpu-result.json").read_text(encoding="utf-8"))
    stock_placement = results["stock"]["cpu_facts"]["needs_brain_placement"]
    summary = {"stamp": stamp(), "selected_activity": "P4 Alpha Duel" if stock_placement else "Skirmish Defense",
        "stock_requires_brain_placement": stock_placement,
        "pass": results["p4"]["pass"] and results["script"]["pass"] and (results["stock"]["pass"] or stock_placement),
        "results": results}
    write_json(root / "cpu-suite.json", summary)
    return summary


def validate_cpu_red(root, suite):
    checks, quotes = {}, {}
    for peer, write_team in (("host", 1), ("client", 2)):
        rows = suite["results"]["p4"]["cpu_facts"]["rows"][peer]
        first = [row for row in rows if row["phase"] == "first_tick"]
        checks[peer + "_p4_first_tick_no_cpu"] = len(first) == 1 and first[0]["running"] == 1 and first[0]["tick"] == 1 and first[0]["aiTeams"] == [] and first[0]["flags"] == [[0, 0], [1, 0]] and first[0]["legacy"] == -1
        written = [row for row in suite["results"]["script"]["cpu_facts"]["rows"][peer] if row["phase"] == "first_tick"]
        checks[peer + "_per_peer_lua_assignment"] = len(written) == 1 and written[0]["legacy"] == write_team
        directory = root / "cpu_p4/e2e/humans_vs_cpu" / peer
        quotes[peer] = []
        for path in (directory / "stdout.log", directory / "runtime/LogConsole.txt"):
            if path.is_file():
                quotes[peer].extend({"path": str(path), "line": number, "text": line} for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1) if "[cpu-facts] phase=first_tick" in line)
        checks[peer + "_raw_quote_present"] = bool(quotes[peer])
    write_json(root / "cpu-red-detection.json", {"checks": checks, "quotes": quotes})
    if not all(checks.values()):
        raise RuntimeError("the CPU roster/Lua assignment RED was not reproduced; inspect cpu-red-detection.json")


def validate_red(root):
    world = json.loads((root / "reseat/world-result.json").read_text(encoding="utf-8"))
    seats = json.loads((root / "lua/seat-result.json").read_text(encoding="utf-8"))
    checks = {"unequal_world": world["checks"]["complete_world_block_equal"] is False,
              "host_original_brain": world["host"]["brains"] == [1048577, 1048596],
              "client_reseated_brain": world["client"]["brains"] == [1048577, 1048787],
              "lua_detected": seats["pass"] is False}
    for peer, team in (("host", 0), ("client", 1)):
        rows = seats["rows"].get(peer, [])
        checks[peer + "_only_local_human"] = len(rows) == 4 and rows[0][1:4] == [1, 1, team] and rows[0][5] == 101 and all(row[1:3] == [0, 0] for row in rows[1:])
        directory = root / "screens/e2e/snapshot_p5" / peer
        screen_log = "\n".join(path.read_text(errors="replace") for path in (directory / "stdout.log", directory / "runtime/LogConsole.txt") if path.is_file())
        checks[peer + "_screen_guard_red"] = "[screen-facts] begin" in screen_log and "[screen-facts] pass remote=1" not in screen_log
    write_json(root / "red-detection.json", checks)
    if not all(checks.values()):
        raise RuntimeError("the required RED defects were not reproduced; inspect red-detection.json")


def gates(root):
    invoke(HERE / "reconnect.py", ["--build-manifest", ROOT / "reconnect-build.json"], root / "reconnect.log",
           require_zero=True)
    invoke(REPO / "tools/run_selftests.py", ["--repo", REPO, "--out", root / "selftests", "--timeout", 300],
           root / "selftests.log", require_zero=True)
    invoke(REPO / "tools/run_sim_test.py", ["--repo", REPO, "--out", root / "script_graph", "--timeout", 300, "--",
        "-script-graph-selftest", "-num-lua-states", 4], root / "script_graph.log", require_zero=True)
    invoke(REPO / "tools/test_global_callbacks.py", ["--repo", REPO, "--recording", AK47, "--out", root / "global_callbacks"],
           root / "global_callbacks.log", require_zero=True)
    pair = root / "snapshot/e2e/snapshot_p5"
    saves = [pair / peer / "runtime/Userdata/UserSavedGames.rte" / f"p5snap_p{number}.ccsave"
             for number, peer in enumerate(("host", "client"), 1)]
    if not all(path.is_file() for path in saves):
        raise RuntimeError("missing snapshot pair for peer_roundtrip compare")
    invoke(REPO / "tools/test_snapshot_roundtrip.py", [*saves, "--repo", REPO, "--recording", AK47,
        "--shared-pair", "--out", root / "peer_roundtrip"], root / "peer_roundtrip.log", require_zero=True)
    invoke(HERE / "compare_offline.py", [ROOT], root / "offline-comparison.log", require_zero=True)
    for stage in ("reference", "red"):
        for case in PIE_CASES:
            invoke(REPO / "tools/pie_lockstep/compare_sp.py", [case, ROOT / stage / ("pie_" + case + "_sp"), root / ("pie_" + case + "_sp"),
                "--out", root / (case + "_against_" + stage + ".json")], root / (case + "_against_" + stage + ".log"),
                   require_zero=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", choices=("plan", "red", "reference", "green"))
    args = parser.parse_args()
    ROOT.mkdir(parents=True, exist_ok=True)
    if args.stage == "plan":
        plan = {"stamp": stamp(), "root": str(ROOT), "ports": [43570, 43571, 43572, 43573, 43580, 43581],
                "activities": activities(), "pie_cases": PIE_CASES, "write_cases": WRITE_CASES,
                "fixtures": fixtures(), "reference_exe": str(REFERENCE_EXE), "reference_actual_sha256": sha(REFERENCE_EXE),
                "red_exe": str(RED_EXE), "red_actual_sha256": sha(RED_EXE),
                "commands": ["python tools/seat_facts/phase_b.py red", "python tools/seat_facts/phase_b.py reference",
                             "pwsh -NoProfile -File tools/seat_facts/build.ps1", "python tools/seat_facts/phase_b.py green"]}
        target = ROOT / "phase-b-plan.json"
        if target.exists():
            parser.error("phase-b-plan.json already exists")
        write_json(target, plan)
        print(json.dumps(plan, indent=2))
        return 0
    plan = json.loads((ROOT / "phase-b-plan.json").read_text(encoding="utf-8"))
    if fixtures() != plan["fixtures"]:
        raise RuntimeError("an existing fixture differs from the Phase A inventory")
    cpu_plan = json.loads((ROOT / "cpu-plan.json").read_text(encoding="utf-8"))
    if any(sha(path) != digest for path, digest in cpu_plan["retained_inputs"].items()):
        raise RuntimeError("a retained CPU fixture/oracle differs from the item 3 inventory")
    if args.stage in ("red", "reference") and sha(EXE) != RED_SHA:
        raise RuntimeError("the RED executable is not on the ruled path")
    if args.stage == "green":
        build = json.loads((ROOT / "build.json").read_text(encoding="utf-8-sig"))
        if build["exit_code"] != 0 or build["exe_sha256"] != sha(EXE) or sha(EXE) in (RED_SHA, REFERENCE_SHA):
            raise RuntimeError("the repaired executable has no matching successful build record")
    root = ROOT / args.stage
    root.mkdir(exist_ok=False)
    os.environ["CCCP_HEADLESS"] = "1"
    os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
    try:
        if args.stage == "reference":
            swap(REFERENCE_EXE, REFERENCE_SHA)
        expected = sha(EXE)
        runner = instrument_runner(expected)
        from win32_test_runner import firewall_allows_inbound
        if firewall_allows_inbound(EXE) is not True:
            raise RuntimeError("firewall_allow_rule_present failed for " + str(EXE))
        write_json(root / "manifest.json", {"stamp": stamp(), "exe_sha256": expected, "fixtures": fixtures(), "driver_sha256": sha(__file__)})
        pair_arms(root, args.stage == "reference")
        cpu_suite = cpu_arms(root)
        if args.stage == "red":
            validate_red(root)
            validate_cpu_red(root, cpu_suite)
        compatibility(runner, root)
        if args.stage == "green":
            gates(root)
        write_json(root / "completed.json", {"stamp": stamp(), "exe_sha256": sha(EXE), "fixtures_unchanged": fixtures() == plan["fixtures"], "scratch_bytes": footprint()})
    finally:
        if args.stage == "reference":
            swap(RED_EXE, RED_SHA)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
