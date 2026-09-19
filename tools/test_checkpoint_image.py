"""Detecting checks for the copy-on-write checkpoint image.

WRITE-ONLY until the completion pass: this file is not executed by the lane.
GREEN is freeze_us < 16700 at a 240-actor scene, hash identity with capture on/off,
an image that ignores writes during worker traversal, and a capture that matches the
synchronous save of the same tick.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import re
import zipfile
from pathlib import Path

from run_selftests import score_selftest
from run_sim_test import make_run

FAMILY_LOCK = Path("D:/mx/LEAD_FAMILY.lock")
LIMIT_US = 16700
RED_STALL_MS = 870
# The all-dirty arm bounds the worst case: it may miss one tick, never the base stall.
DIRTY_BOUND_US = RED_STALL_MS * 1000
ACTORS = 240
TICKS = 180
RESTORE_NAME = "CheckpointRestoreProbe"
FREEZE = re.compile(
    r"^\[autosave\] tick=(\d+) freeze_us=(\d+) worker_us=(\d+) image_bytes=(\d+) "
    r"dirty_ratio=([0-9.]+) p99_freeze_us=(\d+)\s*$",
    re.MULTILINE,
)
SELFTEST_ROW = re.compile(r"^\[cow-checkpoint-selftest\] (PASS|FAIL) (.+)$", re.MULTILINE)
FREEZE_ROW = re.compile(
    r"^\[cow-checkpoint-selftest\] (PASS|FAIL) freeze_240_actors_under_one_tick freeze_us=(\d+) saved=(\d+)",
    re.MULTILINE,
)
# Rows the standalone flag can run without a scene.
REQUIRED_ROWS = (
    "generational_shadow_keeps_the_freeze_value",
    "peek_reuses_the_shadow_when_the_stamp_matches",
)
# Rows that need the live 240-actor scene, printed by the autosave run.
REQUIRED_SCENE_ROWS = (
    "image_ignores_writes_during_worker_traversal",
    "restore_round_trip_matches_synchronous_capture",
)


def fixture_ini(preset: str, script: str, lua_class: str) -> str:
    return f"""DataModule
	ModuleName = User Scenes
	AddActivity = GAScripted
		PresetName = {preset}
		SceneName = Grasslands
		ScriptPath = UserScenes.rte/{script}
		LuaClassName = {lua_class}
		MinTeamsRequired = 1
		IsTestActivity = 1
		DefaultRequireClearPathToOrbit = 0
		DefaultFogOfWar = 0
		DefaultDeployUnits = 0
"""


def fixture_lua(lua_class: str, dirty_all: bool) -> str:
    dirty = """
    for _, unit in ipairs(self.Units) do
        unit:SetNumberValue("AutosaveCaptureTick", self.CaptureTick);
    end
    self.Shared.revision = self.CaptureTick;
""" if dirty_all else "\n"
    return f"""package.loaded.Constants = nil; require("Constants");

function {lua_class}:StartActivity(startNewGame)
    self.ActivityState = Activity.RUNNING;
    if startNewGame ~= false then
        self.Units = {{}};
        self.Brains = {{}};
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
        self.Shared = {{revision = 0}};
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

function {lua_class}:UpdateActivity()
    self.CaptureTick = self.CaptureTick + 1;
    assert(self.Alias == self.Shared);
    for _, unit in ipairs(self.Units) do
        assert(MovableMan:IsActor(unit) and unit.Health > 0);
    end
    {dirty}
end
"""


def freeze_failures(rows: list[dict], *, skip_first: bool = False, limit: int = LIMIT_US) -> list[str]:
    measured = rows[1:] if skip_first and len(rows) > 1 else rows
    failures = []
    for row in measured:
        if row["freeze_us"] == 0:
            failures.append(
                f"{row['source']}:{row['line']}: {row['raw']} "
                f"(actual freeze_us=0 required freeze_us>0 and < {limit})"
            )
        elif row["freeze_us"] >= limit:
            failures.append(
                f"{row['source']}:{row['line']}: {row['raw']} "
                f"(actual freeze_us={row['freeze_us']} required < {limit} us; "
                f"RED today is the ~{RED_STALL_MS} ms sim-thread stall of Scene::CaptureSavedScene plus Lua graph capture)"
            )
    return failures


def parse_freeze_log(text: str, source: str) -> list[dict]:
    rows = []
    for index, line in enumerate(text.splitlines(), 1):
        match = FREEZE.fullmatch(line)
        if not match:
            continue
        rows.append({
            "source": source,
            "line": index,
            "raw": line,
            "tick": int(match[1]),
            "freeze_us": int(match[2]),
            "worker_us": int(match[3]),
            "image_bytes": int(match[4]),
            "dirty_ratio": float(match[5]),
            "p99_freeze_us": int(match[6]),
        })
    return rows


def named_rows(stdout: str) -> dict:
    rows = {}
    for status, name in SELFTEST_ROW.findall(stdout):
        rows[name.split()[0]] = status
    return rows


def score_selftest_stdout(stdout: str, exit_code: int) -> dict:
    scored = score_selftest(stdout, exit_code, False, "cow-checkpoint-selftest")
    rows = named_rows(stdout)
    missing = [name for name in REQUIRED_ROWS if rows.get(name) != "PASS"]
    scored["rows"] = rows
    scored["missing"] = missing
    scored["pass"] = bool(scored["pass"] and not missing)
    if missing and not scored["reason"]:
        scored["reason"] = "; ".join(f"actual {name}={rows.get(name, 'absent')} required PASS" for name in missing)
    return scored


def score_scene_rows(stdout: str, source: str) -> dict:
    """The live-scene rows and the timed freeze row are printed by the autosave run."""
    rows = named_rows(stdout)
    failures = [
        f"{source}: actual {name}={rows.get(name, 'absent')} required PASS"
        for name in REQUIRED_SCENE_ROWS
        if rows.get(name) != "PASS"
    ]
    timed = FREEZE_ROW.findall(stdout)
    if not timed:
        failures.append(f"{source}: actual freeze_240_actors_under_one_tick absent required one timed row per capture")
    for status, freeze_us, saved in timed:
        if saved == "0" or int(freeze_us) == 0:
            failures.append(
                f"{source}: actual freeze_us={freeze_us} saved={saved} required a saved capture with freeze_us>0"
            )
        elif status == "FAIL" or int(freeze_us) >= LIMIT_US:
            failures.append(
                f"{source}: actual freeze_us={freeze_us} required < {LIMIT_US} us / one sim tick; "
                f"RED today is the ~{RED_STALL_MS} ms sim-thread stall of Scene::CaptureSavedScene plus Lua graph capture"
            )
    return {"pass": not failures, "failures": failures, "rows": rows, "timed": timed}


def score_metrics_json(path: Path) -> dict:
    if not path.is_file():
        return {"pass": False, "failures": [f"missing {path}"], "metrics": {}}
    data = json.loads(path.read_text(encoding="utf-8"))
    failures = []
    for key in ("freeze_us", "worker_us", "image_bytes", "dirty_ratio", "p99_freeze_us", "samples"):
        if key not in data:
            failures.append(f"missing {key}")
    freeze_us = data.get("freeze_us", 0)
    samples = data.get("samples", 0)
    if freeze_us == 0 or samples == 0:
        failures.append(f"actual freeze_us={freeze_us} samples={samples} required freeze_us>0 and samples>0")
    elif freeze_us >= LIMIT_US:
        failures.append(
            f"actual freeze_us={freeze_us} required < {LIMIT_US} "
            f"(RED today is the ~{RED_STALL_MS} ms sim-thread stall)"
        )
    return {"pass": not failures, "failures": failures, "metrics": data}


def score_hash_identity(off_trace: Path, on_trace: Path, ticks: int, on_stdout: str = "") -> dict:
    left = json.loads(off_trace.read_text(encoding="utf-8-sig"))["runs"][0].get("tick_hashes", [])
    right = json.loads(on_trace.read_text(encoding="utf-8-sig"))["runs"][0].get("tick_hashes", [])
    failures = []
    saved = parse_freeze_log(on_stdout, "hash-on")
    if not saved:
        failures.append("actual no [autosave] freeze row in the capture-on run required at least one published capture")
    if not (len(left) == len(right) == ticks):
        failures.append(
            f"actual off={len(left)} on={len(right)} required={ticks} tick hashes"
        )
    else:
        for before, after in zip(left, right):
            if before != after:
                failures.append(
                    f"actual hash differs at tick {before.get('tick')} required identical on/off hashes"
                )
                break
    return {"pass": not failures, "failures": failures}


def newest_autosave(runtime: Path, match: str = "c0de-a1") -> Path | None:
    saves = sorted((runtime / "Autosaves").glob(f"{match}-*.ccsave"), key=lambda item: int(item.stem.split("-")[-1]))
    return saves[-1] if saves else None


def score_restore_round_trip(first: Path, second: Path) -> dict:
    """The archive of a loaded save must carry the same members as the archive it was loaded from."""
    failures = []
    if not first or not first.is_file():
        return {"pass": False, "failures": ["actual no autosave from the 240-actor run required one .ccsave"]}
    if not second or not second.is_file():
        return {"pass": False, "failures": [f"actual no autosave from the run that loaded {first.name} required one .ccsave"]}
    with zipfile.ZipFile(first) as before, zipfile.ZipFile(second) as after:
        before_names, after_names = set(before.namelist()), set(after.namelist())
        if before_names != after_names:
            failures.append(f"actual members {sorted(after_names)} required {sorted(before_names)}")
        for name in sorted(before_names | after_names):
            if name not in before_names or name not in after_names:
                failures.append(f"{name} missing from one archive")
                continue
            if before.read(name) != after.read(name):
                detail = ""
                if name == "Save.ini":
                    detail = " " + first_graph_difference(before.read(name), after.read(name))
                failures.append(
                    f"{name} differs between {first.name} and the archive written after loading it "
                    "(first differing member; restore-and-recapture is not byte identical)" + detail
                )
                break
    return {"pass": not failures, "failures": failures}


def graph_nodes(text: bytes) -> dict:
    """The node ids of every LuaStateGraph block, by VM index, with no full parse."""
    graphs = {}
    for line in text.decode("utf-8", "replace").splitlines():
        stripped = line.strip()
        if not stripped.startswith("LuaStateGraph"):
            continue
        value = stripped.split("=", 1)[1].strip()
        index, _, payload = value.partition("|")
        data = base64.b64decode(payload.replace(".", "="), altchars=b"-_", validate=True)
        header = re.match(rb"SG5;S(\d+);", data)
        ids = re.findall(rb"[TUFCHBJ](\d+);", data[data.rfind(b";N"):]) if header else []
        graphs[index] = (int(header[1]) if header else None, [int(item) for item in ids])
    return graphs


def first_graph_difference(before: bytes, after: bytes) -> str:
    """Name the first node id that moved, so a birth-number divergence is not read as a byte mismatch."""
    try:
        left, right = graph_nodes(before), graph_nodes(after)
    except Exception as error:  # a malformed block is reported as itself
        return f"(the Lua graph could not be read: {error})"
    for index in sorted(left.keys() | right.keys()):
        if index not in left or index not in right:
            return f"(Lua VM {index} is in only one archive)"
        (serial_a, ids_a), (serial_b, ids_b) = left[index], right[index]
        if serial_a != serial_b:
            return f"(Lua VM {index} state counter {serial_a} became {serial_b})"
        for position, (id_a, id_b) in enumerate(zip(ids_a, ids_b)):
            if id_a != id_b:
                return f"(Lua VM {index} node {position}: id {id_a} became {id_b})"
        if len(ids_a) != len(ids_b):
            return f"(Lua VM {index} has {len(ids_a)} nodes and {len(ids_b)} after the restore)"
    return "(the Lua graph ids are identical; the difference is elsewhere in Save.ini)"


def score_archive_round_trip(sync_path: Path, image_path: Path) -> dict:
    failures = []
    if not sync_path.is_file() or not image_path.is_file():
        return {"pass": False, "failures": [f"actual missing archives required {sync_path} and {image_path}"]}
    with zipfile.ZipFile(sync_path) as sync, zipfile.ZipFile(image_path) as image:
        sync_names = set(sync.namelist())
        image_names = set(image.namelist())
        if sync_names != image_names:
            failures.append(f"actual members {sorted(image_names)} required {sorted(sync_names)}")
        for name in sorted(sync_names | image_names):
            if name not in sync_names or name not in image_names:
                failures.append(f"{name} missing from one archive")
                continue
            if sync.read(name) != image.read(name):
                failures.append(f"{name} bytes differ from the synchronous capture of the same tick")
    return {"pass": not failures, "failures": failures}


def install_fixture(runtime: Path, *, dirty_all: bool = False) -> None:
    module = runtime / "Userdata/UserScenes.rte"
    module.mkdir(parents=True, exist_ok=True)
    if dirty_all:
        (module / "Index.ini").write_text(
            fixture_ini("Determinism Autosave Capture 240 Dirty", "AutosaveCapture240Dirty.lua", "AutosaveCapture240Dirty"),
            encoding="utf-8",
        )
        (module / "AutosaveCapture240Dirty.lua").write_text(fixture_lua("AutosaveCapture240Dirty", True), encoding="utf-8")
    else:
        (module / "Index.ini").write_text(
            fixture_ini("Determinism Autosave Capture 240", "AutosaveCapture240.lua", "AutosaveCapture240"),
            encoding="utf-8",
        )
        (module / "AutosaveCapture240.lua").write_text(fixture_lua("AutosaveCapture240", False), encoding="utf-8")


def launch(repo: Path, out: Path, args: list, timeout: float, env: dict | None = None, prepare=None) -> dict:
    if FAMILY_LOCK.exists():
        raise RuntimeError(f"engine launch prohibited while {FAMILY_LOCK} exists")
    os.environ["CCCP_HEADLESS"] = "1"
    private = {"CCCP_HEADLESS": "1", **(env or {})}
    run = make_run(repo, args, out, timeout, env=private)
    if prepare:
        prepare(run.cwd)
    try:
        record = run.start().finish()
    finally:
        run.close()
    stdout = ""
    log = out / "stdout.log"
    if log.exists():
        stdout = log.read_text(encoding="utf-8", errors="replace")
    return {"record": record, "stdout": stdout, "cwd": run.cwd, "out": out}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--score-stdout", type=Path)
    parser.add_argument("--exit-code", type=int, default=0)
    args = parser.parse_args()
    if args.score_stdout:
        scored = score_selftest_stdout(
            args.score_stdout.read_text(encoding="utf-8", errors="replace"), args.exit_code
        )
        print(json.dumps(scored, indent=2))
        print(("PASS" if scored["pass"] else "FAIL") + " cow-checkpoint-selftest")
        return 0 if scored["pass"] else 1

    repo, root = args.repo.resolve(), args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    failures: list[str] = []
    result: dict = {}

    roundtrip = root / "roundtrip"
    selftest = launch(
        repo, root / "cow-checkpoint-selftest",
        ["-cow-checkpoint-selftest"],
        args.timeout,
    )
    scored = score_selftest_stdout(selftest["stdout"], selftest["record"].get("exit_code", 1))
    result["selftest"] = scored
    if not scored["pass"]:
        failures.append(f"selftest: {scored.get('reason')}")

    def run_scene(name: str, scenario: str, dirty: bool, extras: list[str], env: dict | None = None) -> dict:
        return launch(
            repo, root / name,
            ["-scenario", scenario, "-seed", "42", "-max-ticks", str(TICKS), "-scenario-run-past-end", *extras],
            args.timeout,
            env=env,
            prepare=lambda cwd: install_fixture(cwd, dirty_all=dirty),
        )

    skip = run_scene(
        "skip", "Autosave Capture 240", False,
        ["-cow-checkpoint-autosave", "-tick-hashes", "-out", str(root / "skip_trace.json")],
        env={"CCCP_CHECKPOINT_ROUNDTRIP": str(roundtrip)},
    )
    dirty = run_scene("dirty", "Autosave Capture 240 Dirty", True, ["-cow-checkpoint-autosave"])
    off = run_scene("hash-off", "Autosave Capture 240", False, ["-tick-hashes", "-out", str(root / "hash_off.json")])
    on = run_scene("hash-on", "Autosave Capture 240", False, ["-cow-checkpoint-autosave", "-tick-hashes", "-out", str(root / "hash_on.json")])

    scene_rows = score_scene_rows(skip["stdout"], "skip")
    result["scene_rows"] = scene_rows
    if not scene_rows["pass"]:
        failures.extend(scene_rows["failures"])

    archive = score_archive_round_trip(roundtrip / "sync.ccsave", roundtrip / "image.ccsave")
    result["archive"] = archive
    if not archive["pass"]:
        failures.extend(f"archive: {item}" for item in archive["failures"])

    skip_rows = parse_freeze_log(skip["stdout"], "skip")
    dirty_rows = parse_freeze_log(dirty["stdout"], "dirty")
    first_fill = freeze_failures(skip_rows[:1], skip_first=False) if skip_rows else [
        "first fill missing (required a SaveAutosaveSnapshot freeze on the 240-actor scene)"
    ]
    skip_fail = freeze_failures(skip_rows, skip_first=False)
    dirty_fail = freeze_failures(dirty_rows, skip_first=False, limit=DIRTY_BOUND_US)
    if not dirty_rows:
        dirty_fail.append("dirty arm published no freeze row (required one per capture)")
    result["freeze_skip"] = skip_rows
    result["freeze_dirty"] = dirty_rows
    result["dirty_bound_us"] = {
        "limit": DIRTY_BOUND_US,
        "max": max((row["freeze_us"] for row in dirty_rows), default=0),
    }
    result["first_fill"] = first_fill
    if first_fill:
        failures.extend(f"first_fill: {item}" for item in first_fill)
    if skip_fail:
        failures.extend(f"skip: {item}" for item in skip_fail)
    if dirty_fail:
        failures.extend(f"dirty: {item}" for item in dirty_fail)

    metrics = score_metrics_json(skip["cwd"] / "Autosaves" / "checkpoint-metrics.json")
    result["metrics"] = metrics
    if not metrics["pass"]:
        failures.extend(f"metrics: {item}" for item in metrics["failures"])

    # Restore and recapture: the save a second process loads must write the same archive back.
    saved = newest_autosave(skip["cwd"])
    restored = {"pass": False, "failures": ["actual the 240-actor run wrote no autosave required one to load"]}
    if saved:
        def stage_save(cwd: Path) -> None:
            install_fixture(cwd, dirty_all=False)
            module = cwd / "Userdata/UserSavedGames.rte"
            module.mkdir(parents=True, exist_ok=True)
            (module / f"{RESTORE_NAME}.ccsave").write_bytes(saved.read_bytes())

        reload_run = launch(
            repo, root / "restore",
            ["-load-game", RESTORE_NAME, "-cow-checkpoint-autosave", "-max-ticks", "2"],
            args.timeout,
            prepare=stage_save,
        )
        result["restore_stdout_tail"] = reload_run["stdout"][-2000:]
        restored = score_restore_round_trip(saved, newest_autosave(reload_run["cwd"]))
    result["restore"] = restored
    if not restored["pass"]:
        failures.extend(f"restore: {item}" for item in restored["failures"])

    hashes = score_hash_identity(root / "hash_off.json", root / "hash_on.json", TICKS, on["stdout"])
    result["hashes"] = hashes
    if not hashes["pass"]:
        failures.extend(f"hashes: {item}" for item in hashes["failures"])

    (root / "result.json").write_text(json.dumps({"result": result, "failures": failures}, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"failures": failures, "selftest": scored}, indent=2))
    if failures:
        print("FAIL checkpoint-image: " + "; ".join(failures[:8]))
        return 1
    print("PASS checkpoint-image")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
