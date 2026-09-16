"""Detecting checks for the F76 copy-on-write checkpoint image.

WRITE-ONLY until the completion pass: this file is not executed by the lane.
RED today is the ~870 ms sim-thread stall of Scene::CaptureSavedScene plus Lua graph capture.
GREEN is freeze_us < 16700 at a 240-actor scene, hash identity with capture on/off,
an image that ignores writes during worker traversal, and a restore that matches a
synchronous capture of the same tick.
"""

from __future__ import annotations

import argparse
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
ACTORS = 240
FREEZE = re.compile(
    r"^\[autosave\] tick=(\d+) freeze_us=(\d+) worker_us=(\d+) image_bytes=(\d+) "
    r"dirty_ratio=([0-9.]+) p99_freeze_us=(\d+)\s*$",
    re.MULTILINE,
)
SELFTEST_ROW = re.compile(r"^\[cow-checkpoint-selftest\] (PASS|FAIL) (.+)$", re.MULTILINE)
REQUIRED_ROWS = (
    "generational_shadow_keeps_the_freeze_value",
    "peek_reuses_the_shadow_when_the_stamp_matches",
    "freeze_240_actors_under_one_tick",
    "image_ignores_writes_during_worker_traversal",
    "hash_identity_with_capture_on_or_off",
    "restore_round_trip_matches_synchronous_capture",
)

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


def freeze_failures(rows: list[dict], *, skip_first: bool = True) -> list[str]:
    measured = rows[1:] if skip_first and len(rows) > 1 else rows
    failures = []
    for row in measured:
        if row["freeze_us"] >= LIMIT_US:
            failures.append(
                f"{row['source']}:{row['line']}: {row['raw']} "
                f"(limit < {LIMIT_US} us / one sim tick; RED today is the ~{RED_STALL_MS} ms "
                "sim-thread stall of Scene::CaptureSavedScene plus Lua graph capture)"
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


def score_selftest_stdout(stdout: str, exit_code: int) -> dict:
    scored = score_selftest(stdout, exit_code, False, "cow-checkpoint-selftest")
    rows = {name: status for status, name in SELFTEST_ROW.findall(stdout)}
    missing = [name for name in REQUIRED_ROWS if rows.get(name) != "PASS"]
    freeze = re.search(
        r"\[cow-checkpoint-selftest\] (PASS|FAIL) freeze_240_actors_under_one_tick freeze_us=(\d+)",
        stdout,
    )
    if freeze and freeze[1] == "FAIL":
        missing.append(
            f"freeze_240_actors_under_one_tick freeze_us={freeze[2]} "
            f"(RED today is the ~{RED_STALL_MS} ms sim-thread stall of Scene::CaptureSavedScene plus Lua graph capture)"
        )
    scored["rows"] = rows
    scored["missing"] = missing
    scored["pass"] = bool(scored["pass"] and not missing)
    if missing and not scored["reason"]:
        scored["reason"] = "; ".join(missing)
    return scored


def score_metrics_json(path: Path) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    failures = []
    for key in ("freeze_us", "worker_us", "image_bytes", "dirty_ratio", "p99_freeze_us"):
        if key not in data:
            failures.append(f"missing {key}")
    if data.get("freeze_us", LIMIT_US) >= LIMIT_US:
        failures.append(
            f"freeze_us={data.get('freeze_us')} >= {LIMIT_US} "
            f"(RED today is the ~{RED_STALL_MS} ms sim-thread stall)"
        )
    return {"pass": not failures, "failures": failures, "metrics": data}


def score_hash_identity(off_trace: Path, on_trace: Path, ticks: int) -> dict:
    left = json.loads(off_trace.read_text(encoding="utf-8-sig"))["runs"][0]["tick_hashes"]
    right = json.loads(on_trace.read_text(encoding="utf-8-sig"))["runs"][0]["tick_hashes"]
    failures = []
    if not (len(left) == len(right) == ticks):
        failures.append(f"tick hash length off={len(left)} on={len(right)} expected={ticks}")
    else:
        for before, after in zip(left, right):
            if before != after:
                failures.append(f"hash identity differs at tick {before.get('tick')}")
                break
    return {"pass": not failures, "failures": failures}


def score_archive_round_trip(sync_path: Path, image_path: Path) -> dict:
    with zipfile.ZipFile(sync_path) as sync, zipfile.ZipFile(image_path) as image:
        names = {"Save.ini", "Index.ini"}
        failures = []
        for name in names:
            if sync.read(name) != image.read(name):
                failures.append(f"{name} bytes differ from the synchronous capture of the same tick")
    return {"pass": not failures, "failures": failures}


def install_fixture(runtime: Path) -> None:
    module = runtime / "Userdata/UserScenes.rte"
    module.mkdir(parents=True, exist_ok=True)
    (module / "Index.ini").write_text(FIXTURE_INI, encoding="utf-8")
    (module / "AutosaveCapture240.lua").write_text(FIXTURE_LUA, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=120)
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
    if FAMILY_LOCK.exists():
        raise RuntimeError(f"engine launch prohibited while {FAMILY_LOCK} exists")
    os.environ["CCCP_HEADLESS"] = "1"
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    case = out / "cow-checkpoint-selftest"
    run = make_run(args.repo, ["-cow-checkpoint-selftest"], case, args.timeout)
    try:
        record = run.start().finish()
    finally:
        run.close()
    stdout = (case / "stdout.log").read_text(encoding="utf-8", errors="replace") if (case / "stdout.log").exists() else ""
    scored = score_selftest_stdout(stdout, record.get("exit_code", 1))
    (out / "result.json").write_text(json.dumps({"selftest": scored, "record": {
        "exit_code": record.get("exit_code"), "timed_out": record.get("timed_out"),
        "exe_sha256": record.get("exe_sha256"),
    }}, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(scored, indent=2))
    if not scored["pass"]:
        print(f"FAIL cow-checkpoint-selftest: {scored.get('reason')}")
        return 1
    print("PASS cow-checkpoint-selftest")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
