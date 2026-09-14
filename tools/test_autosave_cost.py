"""Measure first and steady autosave boundaries and require exact same-peer reference archives."""

import argparse
import base64
import ctypes
from ctypes import wintypes
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import threading
import time
import zipfile

from test_autosave import CAPTURE, FAMILY_LOCK, compare_checkpoint_bytes, inspect_autosaves, run_pair


ACTORS = 240
LIMIT_MS = 3.0
PRESET = "Autosave Capture 240"
WORKER = re.compile(r"^\[autosave-worker\] tick=(\d+) thread=(\S+) boundary_thread=(\S+) "
                    r"serialize_ms=(\d+(?:\.\d+)?) stable=([01])$", re.MULTILINE)
FIXTURE_INI = """DataModule
	ModuleName = User Scenes
	AddActivity = GAScripted
		PresetName = Autosave Capture 240
		SceneName = Grasslands
		ScriptPath = UserScenes.rte/AutosaveCapture240.lua
		LuaClassName = AutosaveCapture240
		MinTeamsRequired = 2
		IsTestActivity = 1
		DefaultRequireClearPathToOrbit = 0
		DefaultFogOfWar = 0
		DefaultDeployUnits = 0
"""
FIXTURE_LUA = """package.loaded.Constants = nil; require("Constants");

function AutosaveCapture240:StartActivity(startNewGame)
    self.ActivityState = Activity.RUNNING;
    if startNewGame ~= false then
        self.Units = {};
        self.Brains = {};
        for i = 1, 240 do
            local team = (i - 1) % 2;
            local unit = CreateAHuman(i <= 2 and "Brain Robot" or "Green Dummy", "Base.rte");
            unit.Team = team;
            unit.AIMode = Actor.AIMODE_SENTRY;
            unit.Pos = Vector(100 + ((i - 1) % 24) * 80, 80 + math.floor((i - 1) / 24) * 55);
            unit.PinStrength = 100000;
            unit.HitsMOs = false;
            unit.GetsHitByMOs = false;
            MovableMan:AddActor(unit);
            self.Units[i] = unit;
            if i <= 2 then self.Brains[team] = unit; end
        end
        self.CaptureTick = 0;
        self.Shared = {revision = 0};
        self.Alias = self.Shared;
    end
    for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
        if self:PlayerActive(player) and self:PlayerHuman(player) then
            local team = self:GetTeamOfPlayer(player);
            local brain = self.Brains[team];
            self:SetPlayerBrain(brain, player);
            self:SwitchToActor(brain, player, team);
            self:SetObservationTarget(brain.Pos, player);
        end
    end
end

function AutosaveCapture240:UpdateActivity()
    self.CaptureTick = self.CaptureTick + 1;
    self.Shared.revision = self.CaptureTick;
    assert(self.Alias == self.Shared);
    for _, unit in ipairs(self.Units) do
        assert(MovableMan:IsActor(unit) and unit.Health > 0);
        unit:SetNumberValue("AutosaveCaptureTick", self.CaptureTick);
    end
end
"""


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def install_fixture(who, runtime):
    module = runtime / "Userdata/UserScenes.rte"
    module.mkdir(parents=True, exist_ok=True)
    (module / "Index.ini").write_text(FIXTURE_INI, encoding="utf-8")
    (module / "AutosaveCapture240.lua").write_text(FIXTURE_LUA, encoding="utf-8")


def checkpoint_actor_count(path):
    with zipfile.ZipFile(path) as archive:
        text = archive.read("Save.ini")
    matches = re.findall(rb"^WorldStructure = ([^\r\n]+)$", text, re.MULTILINE)
    if len(matches) != 1:
        raise ValueError(f"missing or duplicate WorldStructure: {path}")
    values = base64.b64decode(matches[0].replace(b".", b"="), altchars=b"-_", validate=True).split()
    if values[:2] not in ([b"15", b"WorldStructure1"], [b"15", b"WorldStructure2"]):
        raise ValueError(f"unexpected world structure version: {path}")
    cursor, cohorts = 2, []
    for _ in range(6):
        count = int(values[cursor])
        cursor += 1
        if count < 0 or cursor + count > len(values):
            raise ValueError(f"invalid world cohort: {path}")
        cohorts.append([int(uid) for uid in values[cursor:cursor + count]])
        cursor += count
    actors = cohorts[0] + cohorts[3]
    if len(set(actors)) != len(actors) or any(uid <= 0 for uid in actors):
        raise ValueError(f"invalid actor identities: {path}")
    return len(actors)


def process_inventory():
    if os.name != "nt":
        raise RuntimeError("quiet-cost process inventory requires Windows")

    class ProcessEntry(ctypes.Structure):
        _fields_ = [("size", wintypes.DWORD), ("usage", wintypes.DWORD), ("pid", wintypes.DWORD),
                    ("heap", ctypes.c_size_t), ("module", wintypes.DWORD), ("threads", wintypes.DWORD),
                    ("parent", wintypes.DWORD), ("priority", wintypes.LONG), ("flags", wintypes.DWORD),
                    ("name", wintypes.WCHAR * 260)]

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    for function in (kernel.Process32FirstW, kernel.Process32NextW):
        function.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
        function.restype = wintypes.BOOL
    handle = kernel.CreateToolhelp32Snapshot(2, 0)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    rows, entry = [], ProcessEntry()
    entry.size = ctypes.sizeof(entry)
    try:
        found = kernel.Process32FirstW(handle, ctypes.byref(entry))
        while found:
            name = entry.name.lower()
            if name in ("cl.exe", "link.exe") or (name.startswith("cortex command") and name.endswith(".exe")):
                rows.append({"ProcessId": entry.pid, "Name": entry.name})
            found = kernel.Process32NextW(handle, ctypes.byref(entry))
        if ctypes.get_last_error() != 18:
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        kernel.CloseHandle(handle)
    return rows


def launch(repo, root, port, arm, actors):
    if FAMILY_LOCK.exists():
        raise RuntimeError(f"engine launch prohibited while {FAMILY_LOCK} exists")
    before = process_inventory()
    if before:
        raise RuntimeError(f"quiet run requires no compiler, linker or other engine: {before}")
    exe = repo / "Cortex Command.exe"
    exe_before = hashlib.sha256(exe.read_bytes()).hexdigest()
    head_before = subprocess.check_output(["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip()
    samples, sample_errors = [], []
    stop = threading.Event()
    started = time.monotonic()

    def sample():
        while not stop.wait(1):
            try:
                samples.append({"elapsed_seconds": time.monotonic() - started,
                                "family_lock": FAMILY_LOCK.exists(), "processes": process_inventory()})
            except Exception as error:
                sample_errors.append(str(error))
                return

    sampler = threading.Thread(target=sample)
    sampler.start()
    records = {}
    try:
        preset = PRESET if actors == ACTORS else "P4 Alpha Duel"
        extras = {who: ["-net-match-service-preset", preset, "-num-lua-states", "4",
                        "-net-replay-out", str(root / f"{who}.ccreplay")]
                  for who in ("host", "client")}
        records = run_pair(repo, root, port, {"host": 2, "client": 2}, 400,
                           extra_args=extras, prepare=install_fixture if actors == ACTORS else None, timeout=600,
                           env={"CCCP_AUTOSAVE_FULL_REFERENCE": "1"} if arm == "incremental-full" else {})
    finally:
        stop.set()
        sampler.join()
        root.mkdir(parents=True, exist_ok=True)
        allowed = {record.get("pid") for record in records.values()}
        try:
            after = process_inventory()
        except Exception as error:
            after = []
            sample_errors.append(str(error))
        foreign = [row for sample in samples for row in sample["processes"] if row["ProcessId"] not in allowed]
        write_json(root / "quiet.json", {"passed": bool(samples) and not sample_errors and not foreign and not after
                   and not FAMILY_LOCK.exists() and not any(row["family_lock"] for row in samples),
                   "before": before, "after": after, "samples": samples, "errors": sample_errors, "foreign": foreign})
        write_json(root / "records.json", records)
        exe_after = hashlib.sha256(exe.read_bytes()).hexdigest()
        head_after = subprocess.check_output(["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip()
        write_json(root / "exe-ledger.json", {"head_before": head_before, "head_after": head_after,
                   "exe_before": exe_before, "exe_after": exe_after,
                   "passed": head_before == head_after and exe_before == exe_after and len(records) == 2
                   and all(record.get("exe_sha256") == exe_before for record in records.values())})


def capture_rows(root, who):
    rows = []
    for name in ("stdout.log", "stderr.log"):
        path = root / who / name
        if not path.is_file():
            continue
        for line, raw in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            match = CAPTURE.fullmatch(raw)
            if match:
                rows.append({"tick": int(match[1]), "capture_ms": float(match[2]), "bytes": int(match[3]),
                             "source": str(path), "line": line, "raw": raw})
    return sorted(rows, key=lambda row: row["tick"])


def worker_rows(root, who):
    rows = []
    for name in ("stdout.log", "stderr.log"):
        path = root / who / name
        if not path.is_file():
            continue
        for line, raw in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            match = WORKER.fullmatch(raw)
            if match:
                rows.append({"tick": int(match[1]), "thread": match[2], "boundary_thread": match[3],
                             "serialize_ms": float(match[4]), "stable": match[5] == "1",
                             "source": str(path), "line": line, "raw": raw})
    return sorted(rows, key=lambda row: row["tick"])


def worker_failures(captures, workers):
    failures = []
    if len({row["tick"] for row in captures}) != len(captures):
        failures.append("duplicate boundary capture ticks")
    if len({row["tick"] for row in workers}) != len(workers):
        failures.append("duplicate worker serialization ticks")
    if {row["tick"] for row in captures} != {row["tick"] for row in workers}:
        failures.append("worker serialization ticks do not match every boundary capture, including the first")
    for row in workers:
        if row["thread"] in (row["boundary_thread"], "0") or row["boundary_thread"] == "0" or not row["stable"]:
            failures.append(f"{row['source']}:{row['line']}: {row['raw']} (requires owned data on a separate worker)")
    return failures


def cost_failures(rows):
    return [f"{row['source']}:{row['line']}: {row['raw']} (limit < {LIMIT_MS:g} ms)"
            for row in rows if row["capture_ms"] >= LIMIT_MS]


def inspect(root, arm, actors=ACTORS, baseline=False):
    result = {"root": str(root), "arm": arm, "actors": actors, "baseline": baseline,
              "boundary_limit_ms_exclusive": LIMIT_MS, "peers": {}, "failures": []}
    records_path = root / "records.json"
    if records_path.exists():
        records = json.loads(records_path.read_text(encoding="utf-8"))
        result["records"] = records
        for who in ("host", "client"):
            record = records.get(who, {})
            if record.get("exit_code") != 0 or record.get("timed_out") or not record.get("evidence_complete"):
                result["failures"].append(f"{who}: runner did not complete successfully")
    else:
        result["failures"].append(f"missing runner records: {records_path}")
    ledger = root / "exe-ledger.json"
    result["exe_ledger"] = json.loads(ledger.read_text(encoding="utf-8")) if ledger.exists() else None
    if not result["exe_ledger"] or not result["exe_ledger"].get("passed"):
        result["failures"].append("executable ledger missing or failed")
    if arm == "cost":
        quiet = root / "quiet.json"
        result["quiet"] = json.loads(quiet.read_text(encoding="utf-8")) if quiet.exists() else None
        if not result["quiet"] or not result["quiet"].get("passed"):
            result["failures"].append("quiet-run evidence missing or failed")
    for who in ("host", "client"):
        rows = capture_rows(root, who)
        workers = worker_rows(root, who)
        peer = {"captures": rows, "workers": workers, "checkpoints": [],
                "first_capture": rows[0] if rows else None, "steady_captures": rows[1:],
                "first_capture_ms": rows[0]["capture_ms"] if rows else None,
                "steady_capture_ms_max": max((row["capture_ms"] for row in rows[1:]), default=None)}
        result["peers"][who] = peer
        if arm == "cost":
            result["failures"].extend(cost_failures(rows[1:]))
        if not baseline:
            result["failures"].extend(f"{who}: {failure}" for failure in worker_failures(rows, workers))
        try:
            details = inspect_autosaves(root, who, True)
            for name in details["files"]:
                path = Path(name)
                count = checkpoint_actor_count(path)
                peer["checkpoints"].append({"path": name, "actors": count})
                if count != actors:
                    result["failures"].append(f"{path}: actors={count}, required={actors}")
                if arm == "incremental-full":
                    reference = path.parent.parent / "AutosaveFull" / path.name
                    comparison = compare_checkpoint_bytes(path, reference)
                    peer["checkpoints"][-1]["comparison"] = comparison
                    if not comparison["passed"]:
                        result["failures"].append(f"{path}: incremental/full differ: {comparison}")
        except Exception as error:
            result["failures"].append(f"{who}: {error}")
    result["passed"] = not result["failures"]
    return result


def oracle_selftest(root):
    def archive(state, comment=b"", extra=None):
        stream = io.BytesIO()
        with zipfile.ZipFile(stream, "w", compression=zipfile.ZIP_STORED) as output:
            entries = {"Index.ini": b"index", "Save.ini": state, "Save Mat.png": b"mat",
                       "Save FG.png": b"fg", "Save BG.png": b"bg", **(extra or {})}
            for name, value in entries.items():
                output.writestr(zipfile.ZipInfo(name, (2000, 1, 1, 0, 0, 0)), value)
            output.comment = comment
        return stream.getvalue()

    full = root / "full.ccsave"
    state = b"SimUpdateCount = 121\nWorld = exact\n"
    full.write_bytes(archive(state))
    cases = {"identical": (archive(state), True),
             "incremental_stub": (archive(b"SimUpdateCount = 121\nWorld = stub\n"), False),
             "different_tick": (archive(state.replace(b"121", b"122")), False),
             "different_comment": (archive(state, b"other"), False),
             "different_terrain": (archive(state, extra={"Save Mat.png": b"changed"}), False)}
    results = {}
    for name, (payload, expected) in cases.items():
        candidate = root / f"{name}.ccsave"
        candidate.write_bytes(payload)
        comparison = compare_checkpoint_bytes(candidate, full)
        results[name] = {"expected_equal": expected, "detected": comparison["passed"] == expected, **comparison}
    log = root / "host/stdout.log"
    log.parent.mkdir()
    log.write_text("[autosave] tick=121 capture_ms=162.000 bytes=50\n"
                   "[autosave] tick=241 capture_ms=3.000 bytes=50\n"
                   "[autosave] tick=361 capture_ms=117.077 bytes=50\n"
                   "[autosave] tick=481 capture_ms=2.999 bytes=50\n"
                   "[autosave-worker] tick=121 thread=2 boundary_thread=1 serialize_ms=162.000 stable=1\n"
                   "[autosave-worker] tick=241 thread=2 boundary_thread=1 serialize_ms=117.000 stable=1\n"
                   "[autosave-worker] tick=361 thread=2 boundary_thread=1 serialize_ms=117.000 stable=1\n"
                   "[autosave-worker] tick=481 thread=2 boundary_thread=1 serialize_ms=117.000 stable=1\n", encoding="utf-8")
    captures, workers = capture_rows(root, "host"), worker_rows(root, "host")
    failures = cost_failures(captures[1:])
    results["strict_cost_boundary"] = {"detected": len(failures) == 2 and "tick=241" in failures[0]
                                       and "tick=361" in failures[1], "failures": failures}
    results["first_capture_retained"] = {"detected": len(captures) == 4 and captures[0]["capture_ms"] == 162.0
                                         and captures[0]["tick"] == 121, "first": captures[0]}
    for name, candidate, expected in (
            ("owned_worker", workers, True), ("missing_first_worker", workers[1:], False),
            ("duplicate_worker", [*workers, workers[0]], False),
            ("sim_thread_serialization", [{**row, "thread": row["boundary_thread"],
                                           "raw": row["raw"].replace(" thread=2 ", " thread=1 ")}
                                          for row in workers], False),
            ("live_object_serialization", [{**row, "stable": False, "raw": row["raw"].replace("stable=1", "stable=0")}
                                           for row in workers], False)):
        failures = worker_failures(captures, candidate)
        results[name] = {"detected": (not failures) == expected, "failures": failures}
    write_json(root / "oracle.json", results)
    return {"passed": all(row["detected"] for row in results.values()), "cases": results}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--arm", choices=("cost", "incremental-full", "compare", "oracle", "fixtures"), required=True)
    parser.add_argument("--run-root", type=Path, help="inspect retained output without launching an engine")
    parser.add_argument("--left", type=Path)
    parser.add_argument("--right", type=Path)
    parser.add_argument("--port", type=int, default=48240)
    parser.add_argument("--runs", type=int, choices=(1, 3), default=3)
    parser.add_argument("--scene", type=int, choices=(4, ACTORS), default=ACTORS,
                        help="use the accepted 240-actor fixture or the original four-actor P4 Alpha Duel")
    parser.add_argument("--baseline", action="store_true", help="measure the full-capture control without requiring a worker line")
    args = parser.parse_args()
    if not 48240 <= args.port <= 48247:
        parser.error("three ports must fit 48240..48249")
    if args.baseline and args.arm != "cost":
        parser.error("--baseline applies only to --arm cost")
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    os.environ["CCCP_HEADLESS"] = "1"
    try:
        if args.arm == "compare":
            if not args.left or not args.right:
                parser.error("--arm compare requires --left and --right")
            result = compare_checkpoint_bytes(args.left.resolve(), args.right.resolve())
        elif args.arm == "oracle":
            result = oracle_selftest(args.out)
        elif args.arm == "fixtures":
            install_fixture("host", args.out)
            result = {"passed": True, "engine_launched": False, "actors": ACTORS}
        elif args.run_root:
            result = inspect(args.run_root.resolve(), args.arm, args.scene, args.baseline)
        else:
            runs = []
            repo = args.repo.resolve()
            for repeat in range(args.runs):
                root = args.out / f"run-{repeat + 1}"
                try:
                    launch(repo, root, args.port + repeat, args.arm, args.scene)
                    measured = inspect(root, args.arm, args.scene, args.baseline)
                except Exception as error:
                    measured = inspect(root, args.arm, args.scene, args.baseline)
                    measured["failures"].append(str(error))
                    measured["passed"] = False
                runs.append(measured)
                write_json(args.out / "result.json", {"runs": runs, "passed": False})
            result = {"passed": all(row["passed"] for row in runs), "runs": runs, "quiet_repetitions": args.runs,
                      "actors": args.scene, "baseline": args.baseline,
                      "fixture_sha256": hashlib.sha256(FIXTURE_LUA.encode()).hexdigest() if args.scene == ACTORS
                      else hashlib.sha256((repo / "Data/Base.rte/Activities/P4AlphaDuel.lua").read_bytes()).hexdigest()}
    except Exception as error:
        result = {"passed": False, "error": str(error)}
    write_json(args.out / "result.json", result)
    print(f"{'PASS' if result['passed'] else 'FAIL'} {args.arm}: {args.out / 'result.json'}", flush=True)
    for row in result.get("runs", [result]):
        for who, peer in row.get("peers", {}).items():
            print(f"COST {row['root']}/{who}: first_ms={peer['first_capture_ms']} "
                  f"steady_max_ms={peer['steady_capture_ms_max']} "
                  f"captures={len(peer['captures'])} worker_lines={len(peer['workers'])}", flush=True)
        for failure in row.get("failures", []):
            print(f"FAIL {failure}", flush=True)
    if result.get("error"):
        print(f"FAIL {result['error']}", flush=True)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
