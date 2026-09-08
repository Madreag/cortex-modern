"""Restore each full peer checkpoint; optionally compare shared state before and after both loads."""

import argparse
import base64
from concurrent.futures import ThreadPoolExecutor
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import zipfile

from run_sim_test import make_run


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def unchanged(files):
    return all(Path(name).is_file() and digest(name) == value for name, value in files.items())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("snapshots", nargs="+", type=Path)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--recording", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--lua-states", type=int, default=4)
    parser.add_argument("--shared-pair", action="store_true", help="require exactly two peers and compare their shared state before and after restoration")
    parser.add_argument("--audio-mode", choices=("uncontrolled", "locked", "negative"), default="uncontrolled",
        help="hold the mixer during ordinary load/save, or deliberately advance one held voice by one PCM sample")
    parser.add_argument("--check-playback", action="store_true", help="require restored unpaused audio to advance after releasing the mixer")
    parser.add_argument("--build-manifest", type=Path, help="immutable compiled-source manifest; permits later source development while the exact executable stays fixed")
    options = parser.parse_args()
    if options.shared_pair and len(options.snapshots) != 2:
        parser.error("--shared-pair requires exactly two snapshots")
    if options.shared_pair and options.audio_mode == "negative":
        parser.error("the one-PCM negative control compares each peer with its own original; omit --shared-pair")
    options.repo = options.repo.resolve()
    options.out = options.out.resolve()
    options.recording = options.recording.resolve()
    options.snapshots = [path.resolve() for path in options.snapshots]
    for path in [options.recording, *options.snapshots]:
        if not path.is_file():
            parser.error(f"input does not exist: {path}")
    options.out.mkdir(parents=True, exist_ok=False)
    shutil.copy2(__file__, options.out / "harness_source.py")
    comparator = options.out / "compare_snapshots.py"
    shutil.copy2(options.repo / "tools/compare_snapshots.py", comparator)
    shutil.copy2(options.repo / "tools/snapshot_runtime.py", options.out / "snapshot_runtime.py")
    runtime_spec = importlib.util.spec_from_file_location("roundtrip_frozen_runtime", options.out / "snapshot_runtime.py")
    runtime = importlib.util.module_from_spec(runtime_spec)
    runtime_spec.loader.exec_module(runtime)
    harness_files = {str(options.out / name): digest(options.out / name)
        for name in ("harness_source.py", "compare_snapshots.py", "snapshot_runtime.py")}
    binary = digest(options.repo / "Cortex Command.exe")
    build = json.loads(options.build_manifest.read_text()) if options.build_manifest else None
    build_files = dict(build["artifacts"]) if build else {}
    if build:
        build_files[str(options.build_manifest.resolve())] = digest(options.build_manifest)
        assert unchanged(build_files), "compiled source artifacts changed"
        assert binary == build["exe_sha256"], "executable differs from compiled-source manifest"
        shutil.copy2(options.build_manifest, options.out / "compiled-build.json")
    patch = Path(build["source_patch"]).read_bytes() if build else subprocess.check_output(["git", "diff", "--binary", "HEAD"], cwd=options.repo)
    (options.out / "source.patch").write_bytes(patch)
    changed = [] if build else subprocess.check_output(["git", "ls-files", "--modified", "--others", "--exclude-standard", "-z"], cwd=options.repo).decode().split("\0")
    sources = {}
    for name in sorted(set(changed) - {""}):
        source = options.repo / name
        if source.is_file():
            target = options.out / "source_files" / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            sources[str(source)] = digest(source)
    inputs, captured, captured_hashes = {}, [], {}
    for index, source in enumerate([options.recording, *options.snapshots]):
        target = options.out / "inputs" / str(index) / source.name
        target.parent.mkdir(parents=True)
        shutil.copy2(source, target)
        inputs[str(source)] = digest(source)
        assert digest(target) == inputs[str(source)], "input changed during capture"
        captured_hashes[str(target)] = digest(target)
        captured.append(target)
    recording, snapshots = captured[0], captured[1:]
    (options.out / "provenance.json").write_text(json.dumps({
        "head": build["head"] if build else subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=options.repo, text=True).strip(),
        "source_basis": str(options.build_manifest.resolve()) if build else "live source held unchanged for the run",
        "source_patch_sha256": hashlib.sha256(patch).hexdigest(), "sources": sources, "inputs": inputs,
        "binary": binary, "harness_files": harness_files, "captured_inputs": captured_hashes,
        "shared_pair": options.shared_pair, "audio_mode": options.audio_mode,
        "check_playback": options.check_playback}, indent=2))

    def compare(left, right, out, full=False):
        comparison = subprocess.run([sys.executable, str(comparator), str(left), str(right),
            "--report", str(out.with_suffix(".json")), *(["--full"] if full else [])], capture_output=True, text=True)
        out.with_suffix(".txt").write_text(comparison.stdout + comparison.stderr)
        report = json.loads(out.with_suffix(".json").read_text()) if out.with_suffix(".json").is_file() else {}
        return {"pass": comparison.returncode == 0, "exit_code": comparison.returncode,
            "output": comparison.stdout + comparison.stderr, "failures": report.get("failures")}

    def one_pcm_fault(left, right, log, comparison):
        changes = re.findall(r"^\[audio-cursor-negative\] id=(\d+) before=(\d+) after=(\d+)$", log, re.M)
        evidence = {"mutation_records": changes, "only_runtime_globals_differ": comparison["failures"] == ["Save.ini 'RuntimeGlobals' block differs (full state)"]}
        if len(changes) != 1 or not evidence["only_runtime_globals_differ"]:
            return {"pass": False, **evidence}
        identity, before, after = map(int, changes[0])
        states = []
        for path in (left, right):
            with zipfile.ZipFile(path) as archive:
                values = re.findall(rb"^RuntimeGlobals\s*=\s*([^\r\n]+)$", archive.read("Save.ini"), re.M)
            if len(values) != 1:
                return {"pass": False, **evidence, "error": "missing or duplicate RuntimeGlobals"}
            states.append(runtime.project(runtime.decode(base64.b64decode(values[0], validate=True)), snapshot_name=path.stem.encode()))
        voices = [voice for voice in states[0]["audio"]["voices"] if voice["identity"] == identity]
        evidence["matches_original_voice"] = len(voices) == 1 and voices[0]["position"] == before
        evidence["one_pcm"] = after == before + 1
        if evidence["matches_original_voice"]:
            voices[0]["position"] = after
        evidence["all_other_runtime_fields_equal"] = states[0] == states[1]
        return {"pass": all(evidence[key] for key in ("matches_original_voice", "one_pcm", "all_other_runtime_fields_equal")), **evidence}

    shared = {}
    if options.shared_pair:
        shared["before"] = compare(*snapshots, options.out / "shared-before")

    def check(item):
        index, snapshot = item
        out = options.out / f"{index}_{snapshot.stem}"
        run = make_run(options.repo, ["-net-replay", recording, "-num-lua-states", options.lua_states,
            "-max-ticks", 3, "-snapshot-roundtrip-selftest", snapshot.stem,
            *(["-snapshot-roundtrip-lock-audio"] if options.audio_mode != "uncontrolled" else []),
            *(["-snapshot-roundtrip-perturb-audio"] if options.audio_mode == "negative" else []),
            *(["-snapshot-roundtrip-check-playback"] if options.check_playback else [])], out, 90)
        saves = Path(run.cwd) / "Userdata/UserSavedGames.rte"
        saves.mkdir(exist_ok=True)
        (saves / "Index.ini").write_text("DataModule\n\tModuleName = Scripted Activity Saves\n")
        shutil.copy2(snapshot, saves / snapshot.name)
        try:
            record = run.start().finish()
        finally:
            run.close()
        log = (out / "stdout.log").read_text(errors="replace")
        console = out / "runtime/LogConsole.txt"
        if console.exists(): log += "\n" + console.read_text(errors="replace")
        errors = re.findall(r"^.*(?:ERROR:|RTE Assert|RTE Abort|stack traceback|\[snapshot-roundtrip\] FAIL).*$", log, re.M)
        restored = saves / (snapshot.stem + "_roundtrip.ccsave")
        comparison = compare(snapshot, restored, out / "comparison", full=True)
        fault = one_pcm_fault(snapshot, restored, log, comparison) if options.audio_mode == "negative" else None
        checks = {"process": record["exit_code"] == 0 and not record["timed_out"],
            "restored_and_saved": "[snapshot-roundtrip] PASS" in log,
            **({"one_pcm_fault_detected": fault["pass"]} if fault else {"full_snapshot": comparison["pass"]}), "no_errors": not errors,
            "desktop": record["input_desktop_before"] == record["input_desktop_after"],
            "expected_binary": record["exe_sha256"] == binary}
        if options.audio_mode != "uncontrolled":
            checks["mixer_boundary_unchanged"] = f"[snapshot-audio-boundary] locked=1 checked=1 unchanged=1 perturbed={int(options.audio_mode == 'negative')}" in log
        if options.check_playback:
            checks["playback_continued"] = "[audio-playback-continuation] PASS" in log
        result = {"pass": all(checks.values()), "checks": checks, "errors": errors,
            "audio_mode": options.audio_mode, "one_pcm_fault": fault,
            "binary": record["exe_sha256"], "snapshot": str(snapshot.resolve()),
            "snapshot_sha256": digest(snapshot), "restored": str(restored), "comparison": comparison}
        (out / "result.json").write_text(json.dumps(result, indent=2))
        return result

    with ThreadPoolExecutor(max_workers=2) as pool:
        results = list(pool.map(check, enumerate(snapshots)))
    if options.shared_pair:
        shared["after"] = compare(*(result["restored"] for result in results), options.out / "shared-after")
    guards = {"source_unchanged": unchanged(build_files if build else sources), "inputs_unchanged": unchanged(inputs),
        "captured_inputs_unchanged": unchanged(captured_hashes), "harness_unchanged": unchanged(harness_files),
        "binary_unchanged": digest(options.repo / "Cortex Command.exe") == binary}
    passed = all(guards.values()) and all(result["pass"] for result in results) and all(result["pass"] for result in shared.values())
    (options.out / "result.json").write_text(json.dumps({"pass": passed, **guards, "shared": shared, "results": results}, indent=2))
    print(json.dumps({"pass": passed, **guards, "checks": [result["checks"] for result in results],
        "shared": {key: value["pass"] for key, value in shared.items()}, "out": str(options.out)}), flush=True)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
