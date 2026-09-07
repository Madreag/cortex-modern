"""Compare uninterrupted playbacks with memory and file checkpoint continuations."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import uuid

if __package__:
    from .compare_sim_traces import strict_compare
    from .run_sim_test import make_run
else:
    from compare_sim_traces import strict_compare
    from run_sim_test import make_run


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run_case(repo, recording, script, out, ticks, capture=None, mode=None, lua_states=None, hold_path_callbacks_until=None, hold_path_results_until=None, global_script=None):
    trace = out / "trace.json"
    dump = Path(str(trace) + ".simdump.txt")
    args = ["-net-replay", recording, "-tick-hashes", "-max-ticks", ticks, "-out", trace]
    if script:
        args += ["-test-script", "UserScenes.rte/ScriptState/" + script.name]
    if capture:
        args += ["-rollback-fidelity-probe", f"{capture}:30", "-rollback-fidelity-probe-mode", mode]
    if lua_states is not None:
        args += ["-num-lua-states", lua_states]
    environment = {"CC_SIM_DUMP": f"{ticks - 1}:{ticks - 1}"}
    if hold_path_callbacks_until is not None:
        environment["CC_TEST_ASYNC_PATH_DELIVERY_TICK"] = str(hold_path_callbacks_until)
    if hold_path_results_until is not None:
        environment["CC_TEST_ASYNC_PATH_PUBLICATION_TICK"] = str(hold_path_results_until)
    run = make_run(repo, args, out, 180, environment, [trace, dump])
    if script:
        target = Path(run.cwd) / "Userdata/UserScenes.rte/ScriptState" / script.name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(script, target)
        (target.parent.parent / "Index.ini").write_text("DataModule\n\tModuleName = User Scenes\n\tScanFolderContents = 1\n\tIgnoreMissingItems = 1\n")
    if global_script:
        module = Path(run.cwd) / "Userdata/UserScenes.rte"
        target = module / "ScriptState" / global_script.name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(global_script, target)
        (module / "Index.ini").write_text("DataModule\n\tModuleName = User Scenes\n\tIgnoreMissingItems = 1\n"
            "\tAddGlobalScript = GlobalScript\n\t\tPresetName = Checkpoint Global\n"
            f"\t\tScriptPath = UserScenes.rte/ScriptState/{global_script.name}\n\t\tLuaClassName = CheckpointGlobalScript\n")
        settings = Path(run.cwd) / "Userdata/Settings.ini"
        with settings.open("a") as stream:
            stream.write("\n\tEnableGlobalScript = UserScenes.rte/Checkpoint Global\n")
    try:
        record = run.start().finish()
    finally:
        run.close()
    log = (out / "stdout.log").read_text(encoding="utf-8-sig", errors="replace")
    for console in (Path(str(trace) + ".console.txt"), out / "runtime/Userdata/CheckpointErrors.txt"):
        if console.exists():
            log += "\n" + console.read_text(encoding="utf-8-sig", errors="replace")
    errors = re.findall(r"^.*(?:FIDELITY FAIL|RESTORE MISMATCH|\[rbprobe\] FAIL|ERROR:|RTE Assert|RTE Abort|stack traceback|checkpoint (?:table|vector|timer|sound|closure|shared|coroutine|wrapped|iterator)).*$", log, re.M)
    checks = {
        "process": record["exit_code"] == 0 and not record["timed_out"],
        "files": record["evidence_complete"],
        "no_errors": not errors,
        "trace_valid": strict_compare(trace, trace, ticks)[0],
    }
    detail = {"errors": errors, "exe_sha256": record["exe_sha256"]}
    if global_script:
        checks["global_started"] = "[global-callback] start=1 actor=" in log
        checks["global_deactivated"] = "[global-callback] deactivated calls=70" in log
    if capture:
        checks["probe"] = f"FIDELITY PASS: 30 ticks byte-identical after the restore, hash and full dump (capture {capture})" in log
    if script:
        rows = re.findall(r"^\d+ actor uid=\d+ .* script=(-?\d+)/(-?\d+)/(-?\d+)/(-?\d+)", dump.read_text() if dump.exists() else "", re.M)
        checks["script_continued"] = bool(rows) and all(int(c) == 1 and int(u) >= ticks - 65 and u == p and int(old) == 0 for c, u, p, old in rows)
        detail["script_rows"] = rows
    return {"checks": checks, "detail": detail, "trace": str(trace), "dump": str(dump)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--recording", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--script", type=Path)
    parser.add_argument("--global-script", type=Path, help="CheckpointGlobalScript fixture to enable in the private runtime")
    parser.add_argument("--ticks", type=int, default=521)
    parser.add_argument("--captures", type=int, nargs="+", default=[50, 150, 250, 400])
    parser.add_argument("--modes", nargs="+", choices=["memory", "file", "launch"], default=["memory", "file"])
    parser.add_argument("--lua-states", type=int)
    parser.add_argument("--hold-path-callbacks-until", type=int)
    parser.add_argument("--hold-path-results-until", type=int)
    options = parser.parse_args()
    if any(c < 1 or c + 31 >= options.ticks for c in options.captures):
        parser.error("every capture and window must finish before the continuation dump")
    changed = subprocess.check_output(["git", "ls-files", "--modified", "--others", "--exclude-standard", "-z"], cwd=options.repo).decode().split("\0")
    root = options.out / (datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S_") + uuid.uuid4().hex[:8])
    root.mkdir(parents=True, exist_ok=False)
    provenance = {"head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=options.repo, text=True).strip(), "recording": {"path": str(options.recording.resolve()), "sha256": sha256(options.recording)}, "script": {"path": str(options.script.resolve()), "sha256": sha256(options.script)} if options.script else None}
    if options.global_script:
        provenance["global_script"] = {"path": str(options.global_script.resolve()), "sha256": sha256(options.global_script)}
    patch = subprocess.check_output(["git", "diff", "--binary", "HEAD"], cwd=options.repo)
    (root / "source.patch").write_bytes(patch)
    provenance["source_patch_sha256"] = hashlib.sha256(patch).hexdigest()
    provenance["changed_files"] = {}
    for name in sorted(set(changed) - {""}):
        source = options.repo / name
        if source.is_file():
            target = root / "source_files" / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            provenance["changed_files"][name] = sha256(source)
    (root / "provenance.json").write_text(json.dumps(provenance, indent=2))
    results = {}
    cases = [(None, None)] + [(m, c) for m in options.modes for c in options.captures]
    for mode, capture in cases:
        label = f"{mode}_{capture}" if mode else "reference"
        result = run_case(options.repo, options.recording, options.script, root / label, options.ticks, capture, mode, options.lua_states, options.hold_path_callbacks_until, options.hold_path_results_until, options.global_script)
        if mode:
            reference = results["reference"]
            same, comparison = strict_compare(reference["trace"], result["trace"], options.ticks)
            result["checks"]["reference_trace"] = same
            result["detail"]["comparison"] = comparison
            result["checks"]["reference_dump"] = Path(reference["dump"]).exists() and Path(result["dump"]).exists() and sha256(reference["dump"]) == sha256(result["dump"])
            result["checks"]["same_binary"] = reference["detail"]["exe_sha256"] == result["detail"]["exe_sha256"]
        result["pass"] = all(result["checks"].values())
        results[label] = result
        unchanged = all((options.repo / name).is_file() and sha256(options.repo / name) == digest for name, digest in provenance["changed_files"].items())
        complete = len(results) == len(cases)
        passed = complete and unchanged and all(r["pass"] for r in results.values())
        (root / "result.json").write_text(json.dumps({"pass": passed, "complete": complete, "source_unchanged": unchanged, "planned_cases": len(cases), "results": results}, indent=2))
        print(json.dumps({"case": label, "pass": result["pass"], "checks": result["checks"], "out": str(root)}), flush=True)
        if mode is None and not result["pass"]:
            break
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
