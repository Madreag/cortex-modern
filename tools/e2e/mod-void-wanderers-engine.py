"""Detect actor purge, scripted seat control and preview creation boundaries."""

from __future__ import annotations

import argparse
import importlib.util
import json
import re
import sys
import time
from pathlib import Path


ACTIVITY = """function SeatBoundary:StartActivity()
    self.steps = 0;
    self.purgeClear = false;
    self.responses = {0, 0};
    MetricsCollector:BeginRun("scripted seat boundaries", 42);
end

function SeatBoundary:UpdateActivity()
    self.steps = self.steps + 1;
    if self.steps == 10 or self.steps == 40 then
        if self.steps == 40 then
            MovableMan:PurgeAllMOs();
            self.purgeClear = true;
            for player = 0, 3 do
                if self:GetPlayerBrain(player) or self:GetControlledActor(player) then self.purgeClear = false; end
            end
            print("[seat-boundary] purge_clear=" .. tostring(self.purgeClear));
        end
        for player = 0, 3 do
            if self:PlayerActive(player) and self:PlayerHuman(player) then
                local actor = CreateAHuman("Brain Robot", "Base.rte");
                actor.Pos = Vector(700 + player * 100, 540);
                actor.Team = 0;
                actor:AddScript("UserSavedGames.rte/Cold.lua");
                MovableMan:AddActor(actor);
                self:SetPlayerBrain(actor, player);
                self:SwitchToActor(actor, player, 0);
            end
        end
    end
    if self.steps > 60 then
        for player = 0, 1 do
            local actor = self:GetControlledActor(player);
            if actor and MovableMan:IsActor(actor) and actor:GetController():IsState(Controller.MOVE_LEFT) then
                self.responses[player + 1] = self.responses[player + 1] + 1;
            end
        end
    end
    if self.steps == 260 then
        print("[seat-boundary] responses=" .. self.responses[1] .. "," .. self.responses[2]);
        for player = 0, 1 do
            local actor = self:GetControlledActor(player);
            print("[seat-boundary] player=" .. player .. " live=" .. tostring(actor ~= nil and MovableMan:IsActor(actor)));
        end
    end
end

function SeatBoundary:EndActivity()
    MetricsCollector:SetResult(self.purgeClear and self.responses[1] > 0);
    MetricsCollector:EndRun();
end
"""

COLD = """function Create(self)
    self.requiredAfterCreate = 12;
end
function Update(self)
    self.requiredAfterCreate = self.requiredAfterCreate + 1;
end
"""


def stage(run) -> None:
    package = Path(run.cwd) / "Userdata/UserSavedGames.rte"
    package.mkdir()
    (package / "Index.ini").write_text(
        "DataModule\n\tModuleName = Scripted Activity Saves\n\tAddActivity = GAScripted\n"
        "\t\tPresetName = Seat Boundary\n\t\tSceneName = VoidWanderers Strategy Screen\n"
        "\t\tMaxPlayerSupport = 4\n\t\tTeamOfPlayer1 = 0\n\t\tCPUTeam = 1\n"
        "\t\tScriptPath = UserSavedGames.rte/Activity.lua\n\t\tLuaClassName = SeatBoundary\n"
        "\t\tDefaultDeployUnits = 0\n", encoding="utf-8")
    (package / "Activity.lua").write_text(ACTIVITY, encoding="utf-8")
    (package / "Cold.lua").write_text(COLD, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--single", action="store_true")
    parser.add_argument("--port", type=int, default=47630)
    args = parser.parse_args()
    repo, out = args.repo.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(repo / "tools"))
    from run_sim_test import make_run
    spec = importlib.util.spec_from_file_location("scene_checks", Path(__file__).with_name("mod-void-wanderers-scenes.py"))
    checks = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(checks)
    script = out / "presses.txt"
    script.write_text("player=0 80 240 L_LEFT\n", encoding="utf-8")
    common = ["-module", "VoidWanderers.rte", "-seed", "42", "-input-script", str(script)]
    runs = []
    try:
        for peer in (["single"] if args.single else ["host", "client"]):
            if args.single:
                flags = ["-scenario", "UserSavedGames.rte/Seat Boundary", "-max-ticks", "300", "-out", str(out / "single-trace.json")]
            else:
                flags = ["-net-match-service-e2e", "-net-port", str(args.port), "-net-match-peers", "2",
                         "-net-match-ticks", "300", "-net-match-input-delay", "3", "-net-autosave-seconds", "0",
                         "-net-match-service-module", "UserSavedGames.rte", "-net-match-service-preset", "Seat Boundary",
                         "-net-match-service-scene", "VoidWanderers Strategy Screen", "-net-match-service-scene-module", "VoidWanderers.rte",
                         "-net-match-report", str(out / f"{peer}-match.json"), "-net-live-tick-hashes", str(out / f"{peer}-live.jsonl")]
                flags += ["-net-host"] if peer == "host" else ["-net-join", "127.0.0.1"]
            run = make_run(repo, common + flags, out / peer, 120)
            stage(run)
            runs.append((peer, run))
            run.start()
            if peer == "host":
                time.sleep(3)
        records = {peer: run.finish() for peer, run in runs}
        results = []
        for peer, run in runs:
            logs = "\n".join(path.read_text(encoding="utf-8", errors="replace") for path in
                             [out / peer / "stdout.log", Path(run.cwd) / "LogConsole.txt", Path(run.cwd) / "AbortLog.txt"] if path.is_file())
            errors = []
            record = records[peer]
            if record.get("exit_code") != 0 or record.get("timed_out"):
                errors.append(f"engine exit={record.get('exit_code')} timeout={record.get('timed_out')}")
            if "[seat-boundary] purge_clear=true" not in logs:
                errors.append("purge retains an activity brain or controlled actor slot")
            response = re.findall(r"\[seat-boundary\] responses=(\d+),(\d+)", logs)
            if not response or int(response[-1][0]) == 0:
                errors.append("first player's presses never reach the scripted actor")
            if not args.single and (not response or int(response[-1][1]) == 0):
                errors.append("second player's presses never reach the scripted actor")
            bad = [line[:350] for line in logs.splitlines() if re.search(r"ERROR:|RTE Abort|RTE Assert|stack traceback|stopped a preview hook|\bdesync\b", line, re.I)]
            if bad:
                errors.append("engine or Lua error: " + bad[0])
            results.append({"peer": peer, "pass": not errors, "errors": errors, "responses": response,
                            "binary": record.get("exe_sha256"), "record": record})
        hashes = None if args.single else checks.compare_every_tick(out / "host-live.jsonl", out / "client-live.jsonl")
        result = {"pass": all(row["pass"] for row in results) and (hashes is None or hashes["pass"]),
                  "peers": results, "hashes": hashes}
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({**result, "peers": [{k: v for k, v in row.items() if k != "record"} for row in results]}, indent=2), flush=True)
        return int(not result["pass"])
    finally:
        for peer, run in runs:
            run.close()


if __name__ == "__main__":
    sys.exit(main())
