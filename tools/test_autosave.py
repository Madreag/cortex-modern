"""Check rotated checkpoints and every tick against peers and autosave-off controls."""

import argparse
import hashlib
import importlib.util
import json
import os
import re
import subprocess
import sys
import threading
import zipfile
from pathlib import Path

from compare_sim_traces import strict_compare
from run_sim_test import make_run, engine_executable, file_sha256
import box_facts

CAPTURE = re.compile(r"^\[autosave\] tick=(\d+) capture_ms=(\d+(?:\.\d+)?) bytes=(\d+)$", re.MULTILINE)

def run_pair(repo: Path, root: Path, port: int, seconds: dict, ticks: int, *, prepare=None, case=None) -> dict:
    if box_facts.held("verification"):
        raise RuntimeError(f"engine launch prohibited while {box_facts.held('verification')[0]} exists")
    if case:
        case.stage_root(root)
    else:
        root.mkdir(parents=True, exist_ok=False)
    runs, records = {}, {}
    for who in ("host", "client"):
        args = ["-net-match-service-e2e", "-net-port", str(port), "-net-match-peers", "2",
                "-net-match-ticks", str(ticks), "-net-match-input-delay", "3"]
        if seconds[who] is not None:
            args += ["-net-autosave-seconds", str(seconds[who])]
        args += ["-tick-hashes", "-max-ticks", str(ticks), "-out", str(root / f"{who}_trace.json"),
                 "-net-match-report", str(root / f"{who}_report.json")]
        args += ["-net-host"] if who == "host" else ["-net-join", "127.0.0.1"]
        env = {"CCCP_HEADLESS": "1"}
        if minimum := os.environ.get("CC_TEST_AUTOSAVE_STARTUP_MIN_FRAMES"):
            env["CC_TEST_AUTOSAVE_STARTUP_MIN_FRAMES"] = minimum
        factory = case.make_run if case else make_run
        runs[who] = factory(repo, args, root / who, 360, env=env)
        if prepare:
            prepare(who, runs[who].cwd)

    def drive(who: str) -> None:
        try:
            records[who] = runs[who].start().finish()
        except Exception as error:
            records[who] = {"error": repr(error)}

    threads = [threading.Thread(target=drive, args=(who,)) for who in runs]
    try:
        threads[0].start()
        threading.Event().wait(1)
        threads[1].start()
        for thread in threads:
            thread.join()
    finally:
        for run in runs.values():
            run.close()
    return records


def prepare_setting(runtime: Path, seconds) -> None:
    path = runtime / "Userdata/Settings.ini"
    before = path.read_text(encoding="utf-8-sig")
    text = re.sub(r"(?m)^[ \t]*AutosaveSeconds[ \t]*=[^\r\n]*(?:\r?\n|$)", "", before)
    if seconds is not None:
        text += f"\n\tAutosaveSeconds = {seconds}\n"
    path.write_text(text, encoding="utf-8")
    (runtime.parent / "prepared-settings.json").write_text(json.dumps({
        "path": str(path), "autosave_seconds": seconds,
        "source_sha256": hashlib.sha256(before.encode()).hexdigest(),
        "prepared_sha256": hashlib.sha256(text.encode()).hexdigest(),
        "lines": [line for line in text.splitlines() if re.match(r"[ \t]*AutosaveSeconds[ \t]*=", line)]}, indent=2) + "\n", encoding="utf-8")


def inspect_setting(root: Path, who: str, persisted) -> dict:
    """A staged Settings.ini is already fully populated, so the engine never rewrites it: a None row
    proves the cadence came from the shipped default and not from a file the harness wrote."""
    path = root / who / "runtime/Userdata/Settings.ini"
    lines = [{"source": str(path), "line": index, "raw": line, "seconds": int(match[1])}
             for index, line in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1)
             if (match := re.fullmatch(r"[ \t]*AutosaveSeconds[ \t]*=[ \t]*(\d+)[ \t]*", line))]
    if persisted is None:
        assert not lines, f"the row carries a persisted autosave setting: {lines}"
    else:
        assert len(lines) == 1 and lines[0]["seconds"] == persisted, f"persisted autosave setting differs: {lines}"
    return {"persisted_seconds": persisted, "lines": lines,
            "prepared": json.loads((root / who / "prepared-settings.json").read_text())}


def inspect_autosaves(root: Path, who: str, enabled: bool) -> dict:
    runtime = root / who / "runtime"
    files = sorted((runtime / "Autosaves").glob("*.ccsave"))
    log = "\n".join((root / who / name).read_text(encoding="utf-8", errors="replace")
                    for name in ("stdout.log", "stderr.log") if (root / who / name).exists())
    captures = [{"tick": int(t), "capture_ms": float(ms), "bytes": int(n)} for t, ms, n in CAPTURE.findall(log)]
    if not enabled:
        assert not files and not captures, f"autosave off produced files or captures: {runtime}"
        return {"files": [], "captures": []}
    assert len(files) == 3, f"expected 3 rotated autosaves per peer, found {len(files)} in {runtime / 'Autosaves'}"
    assert len(captures) >= 3, f"missing measured [autosave] capture lines in {root / who}"
    assert len({row["tick"] for row in captures}) == len(captures), "duplicate capture ticks"
    assert all(row["bytes"] > 0 for row in captures), "empty checkpoint capture"
    by_tick = {row["tick"]: row for row in captures}
    file_ticks = []
    for path in files:
        match = re.fullmatch(r"(.+)-(\d+)\.ccsave", path.name)
        assert match, f"invalid autosave name {path.name}"
        tick = int(match[2])
        file_ticks.append(tick)
        assert tick in by_tick, f"no capture measurement for {path}"
        with path.open("rb") as source:
            assert source.read(4) == b"PK\x03\x04", f"invalid archive header: {path}"
        with zipfile.ZipFile(path) as archive:
            assert archive.testzip() is None, f"corrupt checkpoint: {path}"
            assert {"Save.ini", "Index.ini", "Save Mat.png", "Save FG.png", "Save BG.png"} <= set(archive.namelist()), path
            state = archive.read("Save.ini").decode("utf-8")
            saved_tick = re.search(r"(?m)^\s*SimUpdateCount = (\d+)\s*$", state)
            assert saved_tick and int(saved_tick[1]) == tick, f"checkpoint tick differs from file name: {path}"
            assert "RuntimeGlobals = " in state and "LuaStateGraph = " in state, f"incomplete checkpoint: {path}"
    assert sorted(file_ticks) == sorted(by_tick)[-3:], "rotation did not retain the newest captures"
    assert "[autosave] failed" not in log and "[autosave] skipped" not in log, "autosave write/capture refused"
    return {"files": [str(path) for path in files], "captures": captures,
            "capture_ms_max": max(row["capture_ms"] for row in captures)}


def compare_peer_checkpoints(repo: Path, left: Path, right: Path, report_a: Path, report_b: Path, log: Path) -> None:
    comparer = repo / "tools" / "compare_snapshots.py"
    env = {**os.environ, "CCCP_TOOLS_DIR": str(repo / "tools")}
    proc = subprocess.run(
        [sys.executable, str(comparer), str(left), str(right),
         "--peer-report-a", str(report_a), "--peer-report-b", str(report_b),
         "--cross-process"],
        capture_output=True, text=True, env=env,
    )
    output = (proc.stdout or "") + (proc.stderr or "")
    log.write_text(output, encoding="utf-8")
    first_fail = next((line for line in output.splitlines() if "FAIL" in line),
                      (output.strip().splitlines() or ["no comparer output"])[0])
    assert proc.returncode == 0, f"peer checkpoint compare failed: {first_fail}"


def exact_role_compare(control: Path, saved: Path, ticks: int) -> None:
    left = json.loads(control.read_text(encoding="utf-8-sig"))["runs"][0]["tick_hashes"]
    right = json.loads(saved.read_text(encoding="utf-8-sig"))["runs"][0]["tick_hashes"]
    assert len(left) == len(right) == ticks, "same-peer trace length"
    for before, after in zip(left, right):
        assert before == after, f"same-peer full tick record differs at tick {before.get('tick')}: {control} vs {saved}"


def main() -> int:
    # The explicit helper directory is the lead's shared main worktree. Do not
    # vendor or patch the mechanism in an autosave-specific transport wrapper.
    helper_parser = argparse.ArgumentParser(add_help=False)
    helper_parser.add_argument("--spread-tools", type=Path)
    helper_args, _ = helper_parser.parse_known_args()
    spread = None
    helper_files = []
    if helper_args.spread_tools:
        folder = helper_args.spread_tools.resolve()
        helper_files = [folder / "spread_peers.py", folder / "cross_peers.py"]
        if not all(path.is_file() for path in helper_files):
            helper_parser.error("spread-tools must contain the shared spread_peers.py and cross_peers.py")
        sys.path.insert(0, str(folder))
        spec = importlib.util.spec_from_file_location("spread_peers", helper_files[0])
        spread = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = spread
        spec.loader.exec_module(spread)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--spread-tools", type=Path, help="lead's shared main tools directory; use its unmodified named transport")
    if spread:
        spread.add_arguments(parser)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=48212)
    parser.add_argument("--ticks", type=int, default=400)
    parser.add_argument("--arm", choices=("all", "default", "host-option"), default="all")
    parser.add_argument("--startup-min-frames", type=int, default=0,
                        help="test control: common input boundary, still bounded by actual measured startup (default unchanged)")
    args = parser.parse_args()
    if spread:
        spread.configure(args)
    if not (48211 <= args.port <= 48216 or 48500 <= args.port <= 48516) or args.ticks < 400:
        parser.error("four ports must fit 48211..48219 or 48500..48519; at least 400 ticks are required")
    if not 0 <= args.startup_min_frames <= 4096:
        parser.error("startup-min-frames must be 0..4096")
    if args.startup_min_frames:
        os.environ["CC_TEST_AUTOSAVE_STARTUP_MIN_FRAMES"] = str(args.startup_min_frames)
    os.environ["CCCP_HEADLESS"] = "1"
    repo, root = args.repo.resolve(), args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    with engine_executable(repo).open("rb") as exe:
        exe_sha = file_sha256(exe)
    result = {"exe_sha256": exe_sha, "arms": {}}
    protected = [repo / "tools" / name for name in
                 ("test_autosave.py", "compare_sim_traces.py", "run_sim_test.py", "compare_snapshots.py", "snapshot_runtime.py", "cross_peers.py")]
    protected += [repo / "Data/Base.rte/Devices/Shared/Scripts/MuzzleSmoke.lua", *helper_files]
    before = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in protected}
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
    named = bool(spread and spread.enabled())
    result.update(source_head=head, topology="spread" if named else "single-box", proof=False, protected_hashes=before)
    if named:
        result.update(interface_version=spread.INTERFACE_VERSION, peer_boxes=spread.pairs(args.peer_boxes))
    native_refused = False
    arms = {"off": {"host": 0, "client": 0}, "on": {"host": 2, "client": 2},
            "asymmetric": {"host": 2, "client": 0}, "rotation": {"host": 1, "client": 1}}
    if args.arm == "default":
        # Only the run lever goes below a minute, so the persisted-setting row takes its 2 s from the flag.
        arms = {"default": {"host": None, "client": None}, "setting": {"host": 2, "client": 2},
                "setting-off": {"host": None, "client": None}, "flag-off": {"host": 0, "client": 0}}
    elif args.arm == "host-option":
        arms = {"host-option": {"host": 2, "client": None}}
    for index, (arm, cadence) in enumerate(arms.items()):
        arm_root = root / arm
        details = {"cadence_seconds": cadence}
        if args.startup_min_frames:
            details["startup_min_frames"] = args.startup_min_frames
        result["arms"][arm] = details
        try:
            prepare, setting = None, None
            if arm == "host-option":
                # Only the host is given a cadence; the client's own setting is staged off.
                def prepare(who, runtime):
                    prepare_setting(runtime, 0 if who == "client" else None)

            if args.arm == "default":
                setting = {"default": None, "setting": 2, "setting-off": 0, "flag-off": 2}[arm]
                details["initial_setting_seconds"] = setting

                def prepare(who, runtime):
                    prepare_setting(runtime, setting)

            autosaving = arm in ("host-option", "setting")
            if named:
                port = args.port + index
                peers = [spread.Peer("host", os="posix", memory=6, timeout=360,
                                     lane="sol-checkpoint-restore-20261006", share_ok=False),
                         spread.Peer("client", os="windows", memory=6, timeout=360,
                                     lane="sol-checkpoint-restore-20261006", share_ok=False)]
                match = spread.Match(port, directory_port=48216 + port - 48212,
                                     parameters={"network": "ice", "lane": "sol-checkpoint-restore-20261006",
                                                 "label": args.runner_label, "runner_wait": args.runner_wait})
                with spread.prepare_case(repo, arm_root, peers, match) as case:
                    records = run_pair(repo, arm_root, port, cadence, args.ticks, prepare=prepare, case=case)
                    details["placement"] = case.result()
                    if case.refusals:
                        raise spread.SpreadRefusal("; ".join(item["text"] for item in case.refusals))
                    identities = [item.get("machine_id") for item in details["placement"]["identities"].values()]
                    assert len(identities) == 2 and all(identities) and len(set(identities)) == 2, "named peers lack distinct native machine identities"
            else:
                records = run_pair(repo, arm_root, args.port + index, cadence, args.ticks, prepare=prepare)
            details["records"] = records
            for who in cadence:
                assert records[who].get("exit_code") == 0 and not records[who].get("timed_out"), records[who]
                # NetMatchService publishes the host's cadence; both peers write it even when the client's own option is off.
                enabled = autosaving or cadence["host"] is not None and cadence["host"] > 0
                details[who] = inspect_autosaves(arm_root, who, enabled)
                if args.arm == "default":
                    if arm in ("setting", "flag-off"):
                        flag = records[who]["argv"].index("-net-autosave-seconds")
                        assert records[who]["argv"][flag + 1] == str(cadence[who]), f"{arm} override is not {cadence[who]}"
                    else:
                        assert "-net-autosave-seconds" not in records[who]["argv"], f"{arm} row supplied the autosave flag"
                    details[who]["setting"] = inspect_setting(arm_root, who, setting)
                    directory = arm_root / who / "runtime/Autosaves"
                    listing = sorted(str(path) for path in directory.iterdir()) if directory.exists() else []
                    details[who]["listing"] = {"directory": str(directory), "exists": directory.exists(), "files": listing}
                    if enabled:
                        saved_ticks = [row["tick"] for row in details[who]["captures"]]
                        assert all(b - a == setting * 60 for a, b in zip(saved_ticks, saved_ticks[1:])), saved_ticks
                    else:
                        log = (arm_root / who / "stdout.log").read_text(encoding="utf-8", errors="replace")
                        assert "[autosave]" not in log, f"{arm} row emitted an autosave line"
                        assert not listing, f"{arm} row produced Autosaves entries: {listing}"
                    print(f"FILES {arm}/{who}: {listing} directory={directory} exists={directory.exists()}", flush=True)
                if arm == "host-option":
                    details[who]["setting"] = inspect_setting(arm_root, who, 0 if who == "client" else None)
                    if who == "client":
                        assert "-net-autosave-seconds" not in records[who]["argv"], "the client was handed the autosave flag"
                    saved_ticks = [row["tick"] for row in details[who]["captures"]]
                    assert all(b - a == 2 * 60 for a, b in zip(saved_ticks, saved_ticks[1:])), saved_ticks
                if arm == "rotation":
                    assert len(details[who]["captures"]) > 3, "rotation arm never exceeded the retention limit"
            passed, comparison = strict_compare(arm_root / "host_trace.json", arm_root / "client_trace.json", args.ticks)
            details["peer_comparison"] = comparison
            assert passed, comparison
            host_files = {Path(path).name: Path(path) for path in details.get("host", {}).get("files", [])}
            client_files = {Path(path).name: Path(path) for path in details.get("client", {}).get("files", [])}
            shared = sorted(set(host_files) & set(client_files))
            if host_files or client_files:
                assert set(host_files) == set(client_files), "peer autosave file sets differ at the host's cadence"
                assert shared, "peer autosaves share no file names for the comparer"
                details["snapshot_compares"] = []
                for name in shared:
                    log = arm_root / f"snapshot_compare_{name}.txt"
                    compare_peer_checkpoints(
                        repo, host_files[name], client_files[name],
                        arm_root / "host_report.json", arm_root / "client_report.json", log)
                    details["snapshot_compares"].append(str(log))
            if enabled:
                ticks_by_peer = {who: [row["tick"] for row in details[who]["captures"]] for who in cadence}
                details["capture_ticks"] = ticks_by_peer
                assert ticks_by_peer["host"] == ticks_by_peer["client"], ticks_by_peer
            if args.arm == "all" and arm != "off":
                for who in cadence:
                    exact_role_compare(root / "off" / f"{who}_trace.json", arm_root / f"{who}_trace.json", args.ticks)
            details["passed"] = True
            if args.arm == "default":
                counts = {who: {"captures": len(details[who]["captures"]), "files": len(details[who]["files"])} for who in cadence}
                print(f"PASS {arm}: {args.ticks} peer ticks match; persisted_setting={setting} autosaving={autosaving}; {counts}", flush=True)
            elif arm == "host-option":
                print(f"PASS host-option: the client autosaved at the host's cadence at ticks "
                      f"{details['capture_ticks']['client']} with its own setting off", flush=True)
            else:
                print(f"PASS {arm}: {args.ticks} peer ticks match; checkpoint rotation and same-peer full hashes match", flush=True)
        except Exception as error:
            details.update(passed=False, error=str(error))
            print(f"FAIL {arm}: {error}", flush=True)
            if named and isinstance(error, spread.SpreadRefusal):
                native_refused = True
                break
    after = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in protected}
    result["harness_unchanged"] = before == after
    result["source_unchanged"] = head == subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
    with engine_executable(repo).open("rb") as exe:
        result["binary_unchanged"] = exe_sha == file_sha256(exe)
    result["proof"] = named and len(result["arms"]) == len(arms) and all(
        details.get("passed", False) and details.get("placement", {}).get("topology") == "spread"
        for details in result["arms"].values()) and all(result[key] for key in
        ("harness_unchanged", "source_unchanged", "binary_unchanged"))
    result["native_refused"] = native_refused
    (root / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if native_refused:
        return 3
    return 0 if all(arm["passed"] for arm in result["arms"].values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
