"""The Void Wanderers battery: the two scenarios of the 2026-09-14 design, run-only on one tree's executable.

    python tools/vw_battery.py --repo <tree> --out <dir> [--port 43620] [--timeout 900] [--source <user copy>] [--parts sp,mp]
    python tools/vw_battery.py --self-test

Manifests first: the installed package <repo>/Data/VoidWanderers.rte against the recorded content digest (the sha256
of its `sha256sum -b` manifest) and against the user's copy when that is present, and the module manifest (Index.ini's
ModuleName, Version, SupportedGameVersion). Then the two scenarios, the mod never edited:
  sp  -module VoidWanderers.rte -scenario "VoidWanderers.rte/Void Wanderers" -seed 42 -max-ticks 600 -tick-hashes: the
      mod's own "Void Wanderers" preset, so its scripts resolve under their own module name, beside a global script in
      the run's private Userdata that begins the trace and grades it: exit 0, the trace written and passed with 600 tick
      hashes, CF.InitFactions printed, no forbidden line in the console or stdout.
  mp  two peers on the mod's "Void Wanderers" activity (-net-match-service-preset): both exit 0, both match reports
      written, host and client tick hashes identical (tools/compare_sim_traces.py), no forbidden line on either peer.
Every launch goes through run_sim_test.make_run (private desktop, CCCP_HEADLESS=1). Each part writes
<out>/<part>/result.json ({pass, case, error}); <out>/result.json holds all three.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
MODULE = "VoidWanderers.rte"
PRESET = "Void Wanderers"
RECORDED_DIGEST = "d1220ef2b4ac541ab5950ee5b223f9f995c0737e3be7b8196088e8b01cd18c2a"  # the manifest digest of the user's copy
DEFAULT_SOURCE = Path("C:/Users/egerm/Downloads/voidwanderersrte-1ign/VoidWanderers.rte")
TICKS = 600
# The paired e2e scenario's forbidden lines (tools/e2e/mod-void-wanderers.json) plus the engine's own assert text.
FORBIDDEN = re.compile(r"^ERROR:|RTE Aborted|Assertion failed|RTE Assert", re.M)
# The mod's activity never begins a metrics run, so a global script beside it begins the trace and grades it: passed
# when the activity was still running at the cap. -module loads only this userdata module (PresetMan::LoadAllDataModules).
TRACE_MODULE = "UserSavedGames.rte"
TRACE_SCRIPT = "VW Battery Trace"
TRACE_INDEX = f"""DataModule
\tModuleName = Scripted Activity Saves
\tAddGlobalScript = GlobalScript
\t\tPresetName = {TRACE_SCRIPT}
\t\tScriptPath = {TRACE_MODULE}/VWBatteryTrace.lua
\t\tLuaClassName = VWBatteryTrace
"""
TRACE_LUA = f"""function VWBatteryTrace:StartScript()
\tself.ticks = 0;
\tMetricsCollector:BeginRun("VoidWanderers", 0);
end

function VWBatteryTrace:UpdateScript()
\tself.ticks = self.ticks + 1;
end

function VWBatteryTrace:EndScript()
\tMetricsCollector:Record("vw_battery_script_ticks", self.ticks);
\tMetricsCollector:SetResult(self.ticks >= {TICKS - 1});
\tMetricsCollector:EndRun();
end
"""


def manifest_lines(package: Path) -> list[str]:
    """`sha256sum -b` over every file, paths relative to the package's parent, sorted by path."""
    files = sorted((p for p in package.rglob("*") if p.is_file()), key=lambda p: p.relative_to(package.parent).as_posix())
    return [f"{hashlib.sha256(p.read_bytes()).hexdigest()} *{p.relative_to(package.parent).as_posix()}" for p in files]


def digest(lines: list[str]) -> str:
    return hashlib.sha256(("\n".join(lines) + "\n").encode("utf-8")).hexdigest()


def module_fields(package: Path) -> dict:
    text = (package / "Index.ini").read_text(encoding="utf-8-sig", errors="replace")
    return {key: (match[1].strip() if (match := re.search(rf"(?m)^\s*{key}\s*=\s*(.+)$", text)) else None)
            for key in ("ModuleName", "Version", "SupportedGameVersion")}


def compare_manifests(repo: Path, source: Path | None, out: Path) -> dict:
    installed = repo / "Data" / MODULE
    result: dict = {"case": "manifests", "installed": str(installed)}
    if not (installed / "Index.ini").is_file():
        result.update({"pass": False, "error": f"{installed} is not an installed module"})
        return result
    lines = manifest_lines(installed)
    (out / "installed-manifest.txt").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    result.update(files=len(lines), content_digest=digest(lines), module=module_fields(installed),
                  recorded_digest=RECORDED_DIGEST)
    problems = []
    if result["content_digest"] != RECORDED_DIGEST:
        problems.append(f"installed content digest {result['content_digest']} is not the recorded {RECORDED_DIGEST}")
    if result["module"]["ModuleName"] != PRESET:
        problems.append(f"Index.ini ModuleName is {result['module']['ModuleName']!r}, not {PRESET!r}")
    if source and (source / "Index.ini").is_file():
        theirs = manifest_lines(source)
        result.update(source=str(source), source_digest=digest(theirs), source_module=module_fields(source))
        only_installed, only_source = sorted(set(lines) - set(theirs)), sorted(set(theirs) - set(lines))
        if only_installed or only_source:
            problems.append(f"installed and source differ: {len(only_installed)} line(s) only installed, "
                            f"{len(only_source)} only in the source; first {(only_installed or only_source)[0]}")
    else:
        result["source"] = f"absent: {source}"
    result.update({"pass": not problems, "error": "; ".join(problems)})
    return result


def forbidden_lines(*paths: Path) -> list[str]:
    hits = []
    for path in paths:
        if path.is_file():
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
            hits += [f"{path.name}: {line.strip()[:200]}" for line in lines if FORBIDDEN.search(line.strip())]
    return hits


def launch(make_run, repo: Path, args: list[str], out: Path, timeout: float, stage=None) -> dict:
    run = make_run(repo, args, out, timeout)
    try:
        if stage:
            stage(Path(run.cwd))
        return run.start().finish()
    finally:
        run.close()


def sp_args(trace: Path) -> list[str]:
    # -scenario prefixes "Determinism " to the module part, which then names no module, so the lookup searches every
    # module for the mod's own preset; a copy defined in another module would run the mod's scripts under that name.
    return ["-module", MODULE, "-scenario", f"{MODULE}/{PRESET}", "-seed", "42", "-max-ticks", str(TICKS),
            "-tick-hashes", "-out", str(trace)]


def stage_trace_script(runtime: Path) -> None:
    module = runtime / "Userdata" / TRACE_MODULE
    module.mkdir()
    (module / "Index.ini").write_text(TRACE_INDEX, encoding="utf-8", newline="\n")
    (module / "VWBatteryTrace.lua").write_text(TRACE_LUA, encoding="utf-8", newline="\n")
    with (runtime / "Userdata" / "Settings.ini").open("a", encoding="utf-8") as settings:
        settings.write(f"\n\tEnableGlobalScript = {TRACE_MODULE}/{TRACE_SCRIPT}\n")


def trace_problems(trace: Path) -> list[str]:
    try:
        run = json.loads(trace.read_text(encoding="utf-8"))["runs"][-1]
    except (ValueError, KeyError, IndexError) as error:
        return [f"unreadable trace: {error!r}"]
    problems = [] if run.get("passed") is True else [f"the trace's run did not pass: passed={run.get('passed')}"]
    if len(run.get("tick_hashes", [])) != TICKS:
        problems.append(f"{len(run.get('tick_hashes', []))} tick hashes, not {TICKS}")
    return problems


def scenario_sp(make_run, repo: Path, out: Path, timeout: float) -> dict:
    trace = out / "trace.json"
    record = launch(make_run, repo, sp_args(trace), out / "run", timeout, stage_trace_script)
    stdout = out / "run" / "stdout.log"
    console = Path(str(trace) + ".console.txt")
    text = "\n".join(p.read_text(encoding="utf-8", errors="replace") for p in (stdout, console) if p.is_file())
    problems = []
    if record.get("exit_code") != 0 or record.get("timed_out"):
        problems.append(f"exit_code={record.get('exit_code')} timed_out={record.get('timed_out')}")
    if not trace.is_file():
        problems.append("no trace written")
    else:
        problems += trace_problems(trace)
    if "CF.InitFactions" not in text:
        problems.append("the mod's CF.InitFactions print is absent")
    problems += [f"forbidden line {hit}" for hit in forbidden_lines(stdout, console)]
    return {"case": "sp", "pass": not problems, "error": "; ".join(problems)[:600], "exit_code": record.get("exit_code"),
            "trace": str(trace), "console": str(console)}


def scenario_mp(make_run, repo: Path, out: Path, timeout: float, port: int) -> dict:
    common = ["-module", MODULE, "-net-match-service-e2e", "-net-port", str(port), "-net-match-peers", "2",
              "-net-match-ticks", str(TICKS), "-tick-hashes", "-max-ticks", str(TICKS), "-net-match-input-delay", "3",
              "-net-autosave-seconds", "0", "-net-match-service-module", MODULE, "-net-match-service-preset", PRESET,
              "-net-match-service-scene", PRESET, "-net-match-service-scene-module", MODULE]
    peers = {"host": ["-net-host"], "client": ["-net-join", "127.0.0.1"]}
    records: dict = {}

    def drive(name: str) -> None:
        try:
            records[name] = launch(make_run, repo, [*common, *peers[name], "-net-match-report", str(out / f"{name}-report.json"),
                                                    "-out", str(out / f"{name}-trace.json")], out / name, timeout)
        except Exception as error:  # noqa: BLE001 - a peer that cannot start is this scenario's red
            records[name] = {"error": repr(error)}

    host = threading.Thread(target=drive, args=("host",))
    host.start()
    time.sleep(2.0)
    client = threading.Thread(target=drive, args=("client",))
    client.start()
    host.join()
    client.join()
    problems = []
    for name in peers:
        record = records.get(name, {})
        if record.get("error") or record.get("exit_code") != 0 or record.get("timed_out"):
            problems.append(f"{name} exit_code={record.get('exit_code')} timed_out={record.get('timed_out')} {record.get('error', '')}".strip())
        if not (out / f"{name}-report.json").is_file():
            problems.append(f"{name} wrote no match report")
        problems += [f"{name} forbidden line {hit}" for hit in
                     forbidden_lines(out / name / "stdout.log", Path(str(out / f"{name}-trace.json") + ".console.txt"))]
    compare = subprocess.run([sys.executable, "-B", str(TOOLS / "compare_sim_traces.py"), str(out / "host-trace.json"),
                              str(out / "client-trace.json"), "--expected-ticks", str(TICKS), "--json",
                              str(out / "trace-compare.json")], capture_output=True, text=True, timeout=600)
    (out / "trace-compare.log").write_text(compare.stdout + compare.stderr, encoding="utf-8")
    if compare.returncode != 0:
        tail = (compare.stdout + compare.stderr).strip().splitlines()[-1:] or [""]
        problems.append(f"host and client tick hashes differ or are incomplete: {tail[0][:300]}")
    return {"case": "mp", "pass": not problems, "error": "; ".join(problems)[:600], "port": port,
            "exit_codes": {name: records.get(name, {}).get("exit_code") for name in peers}}


def write_result(directory: Path, result: dict) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(f"[vw-battery] {'PASS' if result['pass'] else 'FAIL'} {result['case']}"
          f"{' ' + result['error'] if result.get('error') else ''}", flush=True)


def self_test() -> int:
    """The manifest and the log oracles on a scratch package; no engine."""
    failures = []
    with tempfile.TemporaryDirectory() as scratch:
        package = Path(scratch) / "Data" / MODULE
        (package / "Scripts").mkdir(parents=True)
        (package / "Index.ini").write_text("DataModule\n\tModuleName = Void Wanderers\n\tVersion = 6\n"
                                           "\tSupportedGameVersion = 6.2.2\n", encoding="utf-8")
        (package / "Scripts" / "a.lua").write_text("print('x')\n", encoding="utf-8")
        lines = manifest_lines(package)
        if lines != sorted(lines, key=lambda line: line.split(" *", 1)[1]) or not lines[0].endswith("*VoidWanderers.rte/Index.ini"):
            failures.append(f"manifest lines {lines}")
        if module_fields(package) != {"ModuleName": "Void Wanderers", "Version": "6", "SupportedGameVersion": "6.2.2"}:
            failures.append(f"module fields {module_fields(package)}")
        result = compare_manifests(Path(scratch), package, Path(scratch))
        if result["pass"] or "recorded" not in result["error"]:
            failures.append(f"a package that is not the recorded one passed: {result}")
        log = Path(scratch) / "stdout.log"
        log.write_text("CF.InitFactions\nERROR: attempt to index a nil value\nno error here\n  RTE Aborted: x\n", encoding="utf-8")
        hits = forbidden_lines(log)
        if len(hits) != 2:
            failures.append(f"forbidden lines {hits}")
        args = sp_args(Path(scratch) / "trace.json")
        if args[args.index("-scenario") + 1] != f"{MODULE}/{PRESET}" or args[args.index("-module") + 1] != MODULE:
            failures.append(f"sp does not launch the mod's own preset from its own module: {args}")
        trace = Path(scratch) / "trace.json"
        for run, expected in (({"passed": True, "tick_hashes": ["h"] * TICKS}, 0),
                              ({"passed": True, "tick_hashes": ["h"] * (TICKS - 1)}, 1),
                              ({"passed": False, "tick_hashes": ["h"] * TICKS}, 1)):
            trace.write_text(json.dumps({"runs": [run]}), encoding="utf-8")
            if len(trace_problems(trace)) != expected:
                failures.append(f"trace oracle on passed={run['passed']} hashes={len(run['tick_hashes'])}: {trace_problems(trace)}")
    for failure in failures:
        print(f"[vw_battery self-test] FAIL {failure}")
    print(f"[vw_battery self-test] {'PASS' if not failures else 'FAIL'} {len(failures)} failure(s)")
    return 0 if not failures else 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--port", type=int, default=43620)
    parser.add_argument("--timeout", type=float, default=900)
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--parts", default="sp,mp", help="the scenarios to run, comma-separated; the manifests always run")
    parser.add_argument("--self-test", action="store_true")
    options = parser.parse_args(argv)
    if options.self_test:
        return self_test()
    if options.repo is None or options.out is None:
        parser.error("--repo and --out are required")
    os.environ["CCCP_HEADLESS"] = "1"
    repo, out = options.repo.resolve(), options.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(TOOLS))
    from run_sim_test import make_run  # noqa: PLC0415

    results = [compare_manifests(repo, options.source, out)]
    write_result(out / "manifests", results[0])
    for name, run_scenario in (("sp", lambda d: scenario_sp(make_run, repo, d, options.timeout)),
                               ("mp", lambda d: scenario_mp(make_run, repo, d, options.timeout, options.port))):
        if name not in options.parts.split(","):
            continue
        directory = out / name
        directory.mkdir()
        try:
            result = run_scenario(directory)
        except Exception as error:  # noqa: BLE001 - a scenario that cannot run is its own red
            result = {"case": name, "pass": False, "error": f"driver error: {error!r}"}
        write_result(directory, result)
        results.append(result)
    summary = {"pass": all(r["pass"] for r in results), "case": "vw-battery", "repo": str(repo),
               "passed": sum(1 for r in results if r["pass"]), "total": len(results),
               "error": "; ".join(f"{r['case']}: {r['error']}" for r in results if not r["pass"])[:800]}
    (out / "result.json").write_text(json.dumps({**summary, "parts": results}, indent=2), encoding="utf-8")
    print(f"[vw-battery] {'PASS' if summary['pass'] else 'FAIL'} {summary['passed']}/{summary['total']}", flush=True)
    return 0 if summary["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
