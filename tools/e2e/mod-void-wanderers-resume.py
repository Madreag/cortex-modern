"""Detect the reviewed script, purge, file-store and camera regressions."""

from __future__ import annotations

import argparse
import importlib.util
import json
import re
import sys
import time
from pathlib import Path


def load_sibling(name):
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"), Path(__file__).with_name(name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


PURGE_VIEW = """    if self.steps == 39 then
        for player = 0, 3 do
            if self:ScreenOfPlayer(player) >= 0 then
                self:SetViewState(Activity.AIGOTOPOINT, player);
                assert(self:GetViewState(player) == Activity.AIGOTOPOINT, "the local actor view was not entered");
                print("[resume-detector] player=" .. player .. " entered_ai_go_to=" .. self:GetViewState(player));
            end
        end
    end
    if self.steps == 42 then
        for player = 0, 3 do
            if self:ScreenOfPlayer(player) >= 0 then
                local view = self:GetViewState(player);
                assert(view ~= Activity.AIGOTOPOINT and view ~= Activity.UNITSELECTCIRCLE, "purge leaves actor-dependent view=" .. view .. " on player=" .. player);
                assert(self:GetPlayerBrain(player) ~= nil, "replacement brain is absent");
            end
        end
        print("[resume-detector] PASS purge_view");
    end
"""

TEAM_CHANGE = """    if self.steps == 20 then
        local brain = self:GetPlayerBrain(0);
        self:SetTeamOfPlayer(0, 1);
        self:SetPlayerBrain(brain, 0);
        print("[resume-detector] player_team=" .. self:GetTeamOfPlayer(0) .. " brain_team=" .. brain.Team);
        self.setterPass = brain.Team == self:GetTeamOfPlayer(0);
        assert(brain.Team == self:GetTeamOfPlayer(0), "Lua brain setter leaves the brain on the old team");
        print("[resume-detector] PASS team_change");
    end
"""

HELD_SWITCH = """    if self.steps > 45 then
        local brain = self:GetPlayerBrain(1);
        if brain and brain:GetController().InputMode == Controller.CIM_AI then
            local before = brain:GetNumberValue("mode_changes");
            assert(self:SwitchToActor(brain, 1, 0), "held seat switch was refused");
            local after = brain:GetNumberValue("mode_changes");
            print("[resume-detector] held_switch callbacks=" .. before .. "/" .. after);
            assert(before == after, "repeated held-seat switch fires another input-mode callback");
            print("[resume-detector] PASS held_switch");
        end
    end
"""

MODE_HOOK = """function Create(self)
    self:SetNumberValue("mode_changes", 0);
end
function Update(self) end
function OnControllerInputModeChange(self, previousMode, previousPlayer)
    self:SetNumberValue("mode_changes", self:GetNumberValue("mode_changes") + 1);
end
"""

FAIL_CLOSED = """function VWFileProbe:StartScript()
    MetricsCollector:BeginRun("unroutable mutations", 42);
    local created = LuaMan:DirectoryCreate("UnroutedCreate", true);
    local renamed = LuaMan:DirectoryRename("UnroutedRename", "UnroutedMoved");
    local removed = LuaMan:DirectoryRemove("Mods/UnroutedRemove/", true);
    print("[resume-detector] unroutable_create=" .. tostring(created) .. " rename=" .. tostring(renamed) .. " remove=" .. tostring(removed));
    assert(not created and not renamed and not removed, "opted-in store mutates a non-module directory");
    print("[resume-detector] PASS file_closed");
end
function VWFileProbe:EndScript()
    MetricsCollector:SetResult(true);
    MetricsCollector:EndRun();
end
"""


def stage_activity(run, case):
    boundary = load_sibling("mod-void-wanderers-engine")
    boundary.stage(run)
    module = Path(run.cwd) / "Userdata/UserSavedGames.rte"
    source = boundary.ACTIVITY
    if case == "purge-view":
        source = source.replace("    if self.steps > 60 then", PURGE_VIEW + "    if self.steps > 60 then")
        source = source.replace("                self:SwitchToActor(actor, player, 0);", "                if self.steps == 10 then self:SwitchToActor(actor, player, 0); end")
        source = source.replace("            self.purgeClear = true;", """            self.purgeClear = true;
            for player = 0, 3 do
                if self:ScreenOfPlayer(player) >= 0 then
                    print("[resume-detector] player=" .. player .. " immediate_purge_view=" .. self:GetViewState(player));
                    assert(self:GetViewState(player) == Activity.NORMAL, "purge does not reset the cleared control view");
                end
            end""")
    if case == "team-change":
        source = source.replace("self.steps == 10 or self.steps == 40", "self.steps == 10")
        source = source.replace("                self:SwitchToActor(actor, player, 0);", "")
        source = source.replace("    if self.steps > 60 then", TEAM_CHANGE + "    if self.steps > 60 then")
        source = source.replace("MetricsCollector:SetResult(self.purgeClear and self.responses[1] > 0)", "MetricsCollector:SetResult(self.setterPass == true)")
    if case == "held-switch":
        source = source.replace("self.steps == 10 or self.steps == 40", "self.steps == 10")
        source = source.replace("    if self.steps > 60 then", HELD_SWITCH + "    if self.steps > 60 then")
    if case == "feel":
        source = source.replace("MetricsCollector:SetResult(self.purgeClear and self.responses[1] > 0)", "MetricsCollector:SetResult(self.responses[1] > 0)")
        source = source.replace("            for player = 0, 3 do\n                if self:GetPlayerBrain(player) or self:GetControlledActor(player) then self.purgeClear = false; end\n            end\n", "")
    (module / "Activity.lua").write_text(source, encoding="utf-8")
    (module / "Cold.lua").write_text(MODE_HOOK if case == "held-switch" else "function Update(self) end\n", encoding="utf-8")


def stage_file_closed(run):
    from run_sim_test import seed_settings

    runtime = Path(run.cwd).resolve()
    mods = runtime / "Mods"
    assert mods.is_dir() and not mods.is_symlink() and not mods.is_junction(), "generated Mods must be a private plain directory"
    assert not any(mods.iterdir()), "generated Mods must contain no installed package"
    fixture = mods / "UnroutedRemove/Generated.rte"
    fixture.mkdir(parents=True)
    (fixture / "sentinel.txt").write_bytes(b"generated package\n")
    (mods / "UnroutedRename").mkdir()
    (mods / "UnroutedRename/sentinel.txt").write_bytes(b"generated directory\n")
    module = runtime / "Userdata/UserSavedGames.rte"
    module.mkdir()
    (module / "Index.ini").write_text(
        "DataModule\n\tModuleName = Scripted Activity Saves\n\tAddGlobalScript = GlobalScript\n"
        "\t\tPresetName = Module File Probe\n\t\tScriptPath = UserSavedGames.rte/ModuleFileProbe.lua\n"
        "\t\tLuaClassName = VWFileProbe\n", encoding="utf-8")
    (module / "ModuleFileProbe.lua").write_text(FAIL_CLOSED, encoding="utf-8")
    seed_settings(run, {"EnableGlobalScript": "UserSavedGames.rte/Module File Probe"})


def stage_camera(run):
    from run_sim_test import seed_settings

    module = Path(run.cwd) / "Userdata/UserSavedGames.rte"
    module.mkdir()
    (module / "Index.ini").write_text(
        "DataModule\n\tModuleName = Scripted Activity Saves\n\tAddGlobalScript = GlobalScript\n"
        "\t\tPresetName = Camera Detector\n\t\tLateUpdate = 1\n"
        "\t\tScriptPath = UserSavedGames.rte/CameraDetector.lua\n\t\tLuaClassName = CameraDetector\n", encoding="utf-8")
    (module / "CameraDetector.lua").write_text("""function CameraDetector:StartScript() self.ticks = 0; end
function CameraDetector:UpdateScript()
    self.ticks = self.ticks + 1;
    if self.ticks == 60 then
        local activity = ActivityMan:GetActivity();
        for player = 0, 3 do
            local screen = activity:ScreenOfPlayer(player);
            if screen >= 0 then
                local target = CameraMan:GetScrollTarget(screen);
                local offset = CameraMan:GetOffset(screen);
                print("[resume-camera] player=" .. player .. " target=" .. target.X .. "," .. target.Y .. " offset=" .. offset.X .. "," .. offset.Y .. " menu=" .. VoidWanderers.Mid.X .. "," .. VoidWanderers.Mid.Y);
            end
        end
    end
end
""", encoding="utf-8")
    seed_settings(run, {"EnableGlobalScript": "UserSavedGames.rte/Camera Detector"})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--case", choices=["purge-view", "team-change", "held-switch", "file-closed", "external-root", "camera", "feel"], required=True)
    parser.add_argument("--single", action="store_true", help="also check the legacy single-player setter")
    parser.add_argument("--paired", action="store_true", help="check purge cancellation on both peers")
    parser.add_argument("--port", type=int, default=47650)
    args = parser.parse_args()
    if args.paired and (args.single or args.case != "purge-view"):
        parser.error("--paired is only for the purge-view detector")
    repo, out = args.repo.resolve(), args.out.resolve()
    sys.path.insert(0, str(repo / "tools"))
    from run_sim_test import make_run

    if args.case == "external-root":
        files = load_sibling("mod-void-wanderers-files")
        result = files.file_probe(repo, out, external_root=True)
        print(json.dumps({key: value for key, value in result.items() if key != "record"}, indent=2))
        return int(not result["pass"])

    out.mkdir(parents=True, exist_ok=False)
    paired = args.paired or (args.case in ["team-change", "held-switch", "camera"] and not args.single)
    script = out / "presses.txt"
    script.write_text("" if args.case == "camera" else "player=0 80 240 L_LEFT\n", encoding="utf-8")
    common = ["-module", "VoidWanderers.rte", "-seed", "42", "-input-script", str(script)]
    runs, records, rows = [], {}, []
    try:
        for peer in (["host", "client"] if paired else ["single"]):
            environment = {"CC_LUA_FILE_ROOT": "Userdata/ScriptFiles"} if args.case == "file-closed" else {}
            if paired:
                flags = ["-net-match-service-e2e", "-net-port", str(args.port), "-net-match-peers", "2",
                         "-net-match-ticks", "300", "-net-match-input-delay", "3", "-net-autosave-seconds", "0",
                         "-net-match-service-module", "UserSavedGames.rte", "-net-match-service-preset", "Seat Boundary",
                         "-net-match-service-scene", "VoidWanderers Strategy Screen", "-net-match-service-scene-module", "VoidWanderers.rte",
                         "-net-match-report", str(out / f"{peer}-match.json"), "-net-live-tick-hashes", str(out / f"{peer}-live.jsonl")]
                flags += ["-net-host"] if peer == "host" else ["-net-join", "127.0.0.1"]
                if args.case == "held-switch" and peer == "client":
                    flags += ["-net-match-e2e-leave", "-net-match-e2e-leave-tick", "80"]
                if args.case == "camera":
                    flags[flags.index("-net-match-service-module") + 1] = "VoidWanderers.rte"
                    flags[flags.index("-net-match-service-preset") + 1] = "Void Wanderers"
                    flags[flags.index("-net-match-ticks") + 1] = "120"
                    probe = out / f"{peer}-probe/probe.json"
                    probe.parent.mkdir()
                    probe.write_text(json.dumps({"schema": 1, "timeout_ms": 60000, "steps": [
                        {"op": "wait", "lockstep_frame_at_least": 70},
                        {"op": "screenshot", "name": "unassisted_menu", "composited": True}, {"op": "finish"}]}), encoding="utf-8")
                    environment["CC_TEST_NET_UI_SCRIPT"] = str(probe)
            else:
                scenario = "VoidWanderers.rte/Void Wanderers" if args.case == "file-closed" else "UserSavedGames.rte/Seat Boundary"
                flags = ["-scenario", scenario, "-max-ticks", "300", "-tick-hashes", "-out", str(out / "trace.json")]
            run = make_run(repo, common + flags, out / peer, 120, env=environment)
            if args.case == "file-closed":
                stage_file_closed(run)
            elif args.case == "camera":
                stage_camera(run)
            else:
                stage_activity(run, args.case)
            runs.append((peer, run))
            run.start()
            if peer == "host":
                time.sleep(3)
        for peer, run in runs:
            records[peer] = run.finish()
            logs = "\n".join(path.read_text(encoding="utf-8", errors="replace") for path in
                             [out / peer / "stdout.log", Path(run.cwd) / "LogConsole.txt", Path(run.cwd) / "AbortLog.txt"] if path.is_file())
            errors = []
            record = records[peer]
            expected_leave = (args.case == "held-switch" and peer == "client" and record.get("exit_code") == 1
                              and "[net-match] leave: quitting to menu at tick 80" in logs)
            if (record.get("exit_code") != 0 and not expected_leave) or record.get("timed_out"):
                errors.append(f"engine exit={record.get('exit_code')} timeout={record.get('timed_out')}")
            witness = args.case.replace("-", "_")
            if args.case == "feel":
                if "[seat-boundary] responses=161,0" not in logs:
                    errors.append("ordinary purge/replacement and released movement witness missing")
            elif args.case == "camera":
                from PIL import Image
                checks = load_sibling("mod-void-wanderers-scenes")
                pictures = sorted((Path(run.cwd) / "ScreenShots").glob("unassisted_menu_*.png"))
                menu = [[checks.find_words(Image.open(picture), repo / "Data/VoidWanderers.rte", word) for word in ["New game", "Load game"]] for picture in pictures]
                if not menu or not any(all(word["pass"] for word in words) for words in menu):
                    errors.append("unassisted joining camera omits visible New game / Load game")
            elif not (args.case == "held-switch" and peer == "client") and f"[resume-detector] PASS {witness}" not in logs:
                errors.append(f"{witness} witness missing")
            bad = [line[:350] for line in logs.splitlines() if re.search(r"RTE Abort|RTE Assert|stack traceback|stopped a preview hook|\bdesync\b", line, re.I)]
            rejected = {"ERROR: Failed to create directory UnroutedCreate",
                        "ERROR: Failed to rename oldPath UnroutedRename to newPath UnroutedMoved",
                        "ERROR: Failed to remove directory Mods/UnroutedRemove/"} if args.case == "file-closed" else set()
            bad += [line[:350] for line in logs.splitlines() if line.startswith("ERROR:") and line not in rejected]
            if bad:
                errors.append(bad[0])
            if args.case == "file-closed":
                runtime = Path(run.cwd)
                if not (runtime / "Mods/UnroutedRemove/Generated.rte/sentinel.txt").is_file() or not (runtime / "Mods/UnroutedRename/sentinel.txt").is_file() or (runtime / "Mods/UnroutedCreate").exists() or (runtime / "Mods/UnroutedMoved").exists():
                    errors.append("generated non-module directories were altered")
            rows.append({"peer": peer, "pass": not errors, "errors": errors,
                         "detector_lines": [line for line in logs.splitlines() if "[resume-detector]" in line or "[resume-camera]" in line],
                         "binary": record.get("exe_sha256"), "record": record})
        result = {"case": args.case, "pass": all(row["pass"] for row in rows),
                  "proof": not paired, "topology": "single-box: not proof" if paired else "single-peer", "peers": rows}
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({**result, "peers": [{key: value for key, value in row.items() if key != "record"} for row in rows]}, indent=2))
        return int(not result["pass"])
    finally:
        for peer, run in runs:
            run.close()


if __name__ == "__main__":
    raise SystemExit(main())
