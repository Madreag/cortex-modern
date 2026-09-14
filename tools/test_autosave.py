"""Check rotated checkpoints and every tick against peers and autosave-off controls."""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import threading
import zipfile
from pathlib import Path

from compare_sim_traces import strict_compare
from run_sim_test import make_run

CAPTURE = re.compile(r"^\[autosave\] tick=(\d+) capture_ms=(\d+(?:\.\d+)?) bytes=(\d+)$", re.MULTILINE)


def run_pair(repo: Path, root: Path, port: int, seconds: dict, ticks: int, *,
             extra_args=None, prepare=None, env=None, timeout=360) -> dict:
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
        args += list((extra_args or {}).get(who, ()))
        runs[who] = make_run(repo, args, root / who, timeout, env={**(env or {}), "CCCP_HEADLESS": "1"})
        if prepare:
            prepare(who, Path(runs[who].cwd))

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


def compare_checkpoint_bytes(left: Path, right: Path) -> dict:
    """Compare entire archives and report member differences without normalizing bytes."""
    payloads = [left.read_bytes(), right.read_bytes()]
    members, ticks = [], []
    for path in (left, right):
        with zipfile.ZipFile(path) as archive:
            names = archive.namelist()
            if len(names) != len(set(names)) or archive.testzip() is not None:
                raise ValueError(f"invalid checkpoint archive: {path}")
            if not {"Index.ini", "Save.ini", "Save Mat.png", "Save FG.png", "Save BG.png"} <= set(names):
                raise ValueError(f"incomplete checkpoint archive: {path}")
            entries = {name: archive.read(name) for name in names}
            matches = re.findall(rb"^SimUpdateCount = (\d+)\s*$", entries["Save.ini"], re.MULTILINE)
            if len(matches) != 1:
                raise ValueError(f"missing or duplicate checkpoint tick: {path}")
            ticks.append(int(matches[0]))
            members.append(entries)
    different = [name for name in sorted(members[0].keys() | members[1].keys())
                 if members[0].get(name) != members[1].get(name)]
    first = next((i for i, (a, b) in enumerate(zip(*payloads)) if a != b), None)
    if first is None and len(payloads[0]) != len(payloads[1]):
        first = min(map(len, payloads))
    return {"passed": ticks[0] == ticks[1] and payloads[0] == payloads[1],
            "paths": [str(left), str(right)], "ticks": ticks,
            "sizes": list(map(len, payloads)),
            "sha256": [hashlib.sha256(data).hexdigest() for data in payloads],
            "different_members": different, "first_different_archive_byte": first}


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


def exact_role_compare(control: Path, saved: Path, ticks: int) -> None:
    left = json.loads(control.read_text(encoding="utf-8-sig"))["runs"][0]["tick_hashes"]
    right = json.loads(saved.read_text(encoding="utf-8-sig"))["runs"][0]["tick_hashes"]
    assert len(left) == len(right) == ticks, "same-peer trace length"
    for before, after in zip(left, right):
        assert before == after, f"same-peer full tick record differs at tick {before.get('tick')}: {control} vs {saved}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=48212)
    parser.add_argument("--ticks", type=int, default=400)
    parser.add_argument("--arm", choices=("all", "default", "peer-bytes", "restore"), default="all")
    args = parser.parse_args()
    if not (48211 <= args.port <= 48216 or 48240 <= args.port <= 48246) or args.ticks < 400:
        parser.error("four ports must fit 48211..48219 or 48240..48249; at least 400 ticks are required")
    os.environ["CCCP_HEADLESS"] = "1"
    repo, root = args.repo.resolve(), args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    with (repo / "Cortex Command.exe").open("rb") as exe:
        exe_sha = hashlib.file_digest(exe, "sha256").hexdigest()
    result = {"exe_sha256": exe_sha, "arms": {}}
    arms = {"off": {"host": 0, "client": 0}, "on": {"host": 2, "client": 2},
            "asymmetric": {"host": 2, "client": 0}, "rotation": {"host": 1, "client": 1}}
    if args.arm == "default":
        arms = {"default": {"host": None, "client": None}}
    elif args.arm in ("peer-bytes", "restore"):
        arms = {args.arm: {"host": 2, "client": 2}}
    for index, (arm, cadence) in enumerate(arms.items()):
        arm_root = root / arm
        details = {"cadence_seconds": cadence}
        result["arms"][arm] = details
        try:
            extras = {who: ["-net-replay-out", str(arm_root / f"{who}.ccreplay")]
                      for who in cadence} if arm == "restore" else None
            records = run_pair(repo, arm_root, args.port + index, cadence, args.ticks, extra_args=extras)
            details["records"] = records
            for who in cadence:
                assert records[who].get("exit_code") == 0 and not records[who].get("timed_out"), records[who]
                details[who] = inspect_autosaves(arm_root, who, cadence[who] is not None and cadence[who] > 0)
                if arm == "default":
                    assert "-net-autosave-seconds" not in records[who]["argv"], "default row supplied the autosave flag"
                    log = (arm_root / who / "stdout.log").read_text(encoding="utf-8", errors="replace")
                    assert "[autosave]" not in log, "default row emitted an autosave line"
                    directory = arm_root / who / "runtime/Autosaves"
                    listing = sorted(str(path) for path in directory.iterdir()) if directory.exists() else []
                    assert not listing, f"default row produced Autosaves entries: {listing}"
                    details[who]["listing"] = {"directory": str(directory), "exists": directory.exists(), "files": listing}
                    print(f"FILES default/{who}: {listing} directory={directory} exists={directory.exists()}", flush=True)
                if arm == "rotation":
                    assert len(details[who]["captures"]) > 3, "rotation arm never exceeded the retention limit"
            passed, comparison = strict_compare(arm_root / "host_trace.json", arm_root / "client_trace.json", args.ticks)
            details["peer_comparison"] = comparison
            assert passed, comparison
            if arm == "peer-bytes":
                by_tick = {who: {int(Path(path).stem.rsplit("-", 1)[1]): Path(path)
                                 for path in details[who]["files"]} for who in cadence}
                assert by_tick["host"].keys() == by_tick["client"].keys(), "peer saved ticks differ"
                comparisons = [compare_checkpoint_bytes(by_tick["host"][tick], by_tick["client"][tick])
                               for tick in sorted(by_tick["host"])]
                details["checkpoint_byte_comparisons"] = comparisons
                assert all(row["passed"] for row in comparisons), comparisons
            elif arm == "restore":
                snapshots = [max(map(Path, details[who]["files"]), key=lambda path: int(path.stem.rsplit("-", 1)[1]))
                             for who in cadence]
                command = [sys.executable, str(repo / "tools/test_snapshot_roundtrip.py"), *map(str, snapshots),
                           "--repo", str(repo), "--recording", str(arm_root / "host.ccreplay"),
                           "--out", str(arm_root / "roundtrip"), "--lua-states", "4", "--shared-pair",
                           "--audio-mode", "locked"]
                with (arm_root / "roundtrip-driver.log").open("w", encoding="utf-8") as log:
                    restored = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=300)
                details["roundtrip"] = {"argv": command, "exit_code": restored.returncode,
                                        "result": str(arm_root / "roundtrip/result.json")}
                assert restored.returncode == 0, details["roundtrip"]
                assert json.loads((arm_root / "roundtrip/result.json").read_text())["pass"], details["roundtrip"]
            elif arm not in ("off", "default"):
                for who in cadence:
                    exact_role_compare(root / "off" / f"{who}_trace.json", arm_root / f"{who}_trace.json", args.ticks)
            details["passed"] = True
            if arm == "default":
                print(f"PASS default: {args.ticks} peer ticks match; autosave flag absent; capture_lines=0 files_host=0 files_client=0", flush=True)
            elif arm == "peer-bytes":
                print(f"PASS peer-bytes: {args.ticks} peer ticks match; every retained checkpoint is byte-equal", flush=True)
            elif arm == "restore":
                print(f"PASS restore: both newest autosaves pass the full snapshot roundtrip driver", flush=True)
            else:
                print(f"PASS {arm}: {args.ticks} peer ticks match; checkpoint rotation and same-peer full hashes match", flush=True)
        except Exception as error:
            details.update(passed=False, error=str(error))
            print(f"FAIL {arm}: {error}", flush=True)
    (root / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return 0 if all(arm["passed"] for arm in result["arms"].values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
