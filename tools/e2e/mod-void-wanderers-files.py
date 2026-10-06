"""Check module file isolation and the unchanged default Lua file API."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


FILE_PROBE = """function VWFileProbe:StartScript()
    MetricsCollector:BeginRun("module file isolation", 42);
    local function read(path)
        local f = LuaMan:FileOpen(path, "r");
        assert(f >= 0, "read failed: " .. path);
        local line = LuaMan:FileReadLine(f);
        LuaMan:FileClose(f);
        return line;
    end
    assert(read("Mods/LuaFileProbe.rte/CampaignData/seed.dat") == "installed\\n", "installed fallback differs");
    local f = LuaMan:FileOpen("LuaFileProbe.rte/CampaignData/seed.dat", "w");
    assert(f >= 0, "campaign write failed");
    LuaMan:FileWriteLine(f, "campaign\\n");
    LuaMan:FileClose(f);
    assert(LuaMan:FileExists("Mods/LuaFileProbe.rte/CampaignData/seed.dat"), "written campaign missing");
    assert(read("Data/LuaFileProbe.rte/CampaignData/seed.dat") == "campaign\\n", "campaign read differs");
    f = LuaMan:FileOpen("Mods/LuaFileProbe.rte/CampaignData/seed.dat", "a");
    assert(f >= 0, "append failed");
    LuaMan:FileWriteLine(f, "appended\\n");
    LuaMan:FileClose(f);
    f = LuaMan:FileOpen("Data/LuaFileProbe.rte/CampaignData/seed.dat", "r+");
    assert(f >= 0, "update failed");
    LuaMan:FileWriteLine(f, "updated!\\n");
    LuaMan:FileClose(f);
    assert(read("LuaFileProbe.rte/CampaignData/seed.dat") == "updated!\\n", "updated campaign read differs");
    f = LuaMan:FileOpen("LuaFileProbe.rte/CampaignData/new.dat", "w");
    assert(f >= 0, "new campaign write failed");
    LuaMan:FileWriteLine(f, "new campaign\\n");
    LuaMan:FileClose(f);
    assert(LuaMan:FileExists("Data/LuaFileProbe.rte/CampaignData/new.dat"), "new campaign missing");
    assert(read("Mods/LuaFileProbe.rte/CampaignData/new.dat") == "new campaign\\n", "new campaign read differs");
    assert(LuaMan:FileRename("LuaFileProbe.rte/CampaignData/new.dat", "LuaFileProbe.rte/CampaignData/moved.dat"), "file rename failed");
    assert(not LuaMan:FileExists("LuaFileProbe.rte/CampaignData/new.dat"), "file rename leaves old path");
    assert(read("Mods/LuaFileProbe.rte/CampaignData/moved.dat") == "new campaign\\n", "renamed campaign read differs");
    assert(LuaMan:FileRemove("LuaFileProbe.rte/CampaignData/moved.dat"), "file removal failed");
    assert(not LuaMan:FileExists("Mods/LuaFileProbe.rte/CampaignData/moved.dat"), "file removal leaves old path");
    assert(LuaMan:DirectoryCreate("LuaFileProbe.rte/CampaignData/nested", false), "directory create failed");
    assert(LuaMan:DirectoryExists("LuaFileProbe.rte/CampaignData/nested"), "new directory missing");
    local listed = false;
    for file in LuaMan:GetFileList("Mods/LuaFileProbe.rte/CampaignData") do
        if file == "seed.dat" then listed = true; end
    end
    assert(listed, "campaign file list omits seed");
    listed = false;
    for directory in LuaMan:GetDirectoryList("Mods/LuaFileProbe.rte/CampaignData") do
        if directory == "nested" then listed = true; end
    end
    assert(listed, "campaign directory list omits nested");
    assert(LuaMan:DirectoryRename("LuaFileProbe.rte/CampaignData/nested", "LuaFileProbe.rte/CampaignData/renamed"), "directory rename failed");
    assert(not LuaMan:DirectoryExists("LuaFileProbe.rte/CampaignData/nested"), "directory rename leaves old path");
    assert(LuaMan:DirectoryRemove("LuaFileProbe.rte/CampaignData/renamed", true), "directory removal failed");
    assert(not LuaMan:DirectoryExists("LuaFileProbe.rte/CampaignData/renamed"), "directory removal leaves old path");
    print("[module-file-probe] PASS read_write_append_path_aliases");
end

function VWFileProbe:EndScript()
    MetricsCollector:SetResult(true);
    MetricsCollector:EndRun();
end
"""

ROOT_PROBE = """    local rootFile = LuaMan:FileOpen("LuaFileProbe.rte/root.dat", "w");
    assert(rootFile >= 0, "module root write failed");
    LuaMan:FileWriteLine(rootFile, "root\\n");
    LuaMan:FileClose(rootFile);
    for _, prefix in ipairs({"LuaFileProbe.rte", "Mods/LuaFileProbe.rte", "Data/LuaFileProbe.rte"}) do
        local rootListed = false;
        for file in LuaMan:GetFileList(prefix) do
            if file == "root.dat" then rootListed = true; end
        end
        assert(rootListed, "private module root listing omits root.dat: " .. prefix);
    end
    assert(LuaMan:FileRemove("LuaFileProbe.rte/root.dat"), "module root cleanup failed");
"""

READ_ONLY_PROBE = """function VWFileProbe:StartScript()
    MetricsCollector:BeginRun("module file read only", 42);
    local f = LuaMan:FileOpen("Mods/LuaFileProbe.rte/CampaignData/seed.dat", "rt");
    assert(f >= 0, "read-only text open failed");
    assert(LuaMan:FileReadLine(f) == "installed\\n", "read-only text differs");
    LuaMan:FileClose(f);
    print("[module-file-probe] PASS read_write_append_path_aliases");
end
function VWFileProbe:EndScript()
    MetricsCollector:SetResult(true);
    MetricsCollector:EndRun();
end
"""


def file_probe(repo: Path, out: Path, private_store: bool = True, roots: bool = False, read_only: bool = False) -> dict:
    """Run the file bindings against a generated package in the private runtime."""
    sys.path.insert(0, str(repo / "tools"))
    from run_sim_test import make_run, seed_settings

    out.mkdir(parents=True, exist_ok=False)
    trace = out / "trace.json"
    args = ["-module", "VoidWanderers.rte", "-scenario", "VoidWanderers.rte/Void Wanderers",
            "-seed", "42", "-max-ticks", "120", "-tick-hashes", "-out", str(trace)]
    run = make_run(repo, args, out / "run", 120,
                   env={"CC_LUA_FILE_ROOT": "Userdata/ScriptFiles" if private_store else ""})
    try:
        runtime = Path(run.cwd)
        package = runtime / "Mods/LuaFileProbe.rte/CampaignData"
        package.mkdir(parents=True)
        (package / "seed.dat").write_bytes(b"installed\n")
        module = runtime / "Userdata/UserSavedGames.rte"
        module.mkdir()
        (module / "Index.ini").write_text(
            "DataModule\n\tModuleName = Scripted Activity Saves\n\tAddGlobalScript = GlobalScript\n"
            "\t\tPresetName = Module File Probe\n\t\tScriptPath = UserSavedGames.rte/ModuleFileProbe.lua\n"
            "\t\tLuaClassName = VWFileProbe\n", encoding="utf-8")
        source = FILE_PROBE.replace('    print("[module-file-probe]', ROOT_PROBE + '    print("[module-file-probe]') if roots else FILE_PROBE
        (module / "ModuleFileProbe.lua").write_text(READ_ONLY_PROBE if read_only else source, encoding="utf-8")
        seed_settings(run, {"EnableGlobalScript": "UserSavedGames.rte/Module File Probe"})
        record = run.start().finish()
        console = "\n".join(path.read_text(encoding="utf-8", errors="replace") for path in
                            [out / "run/stdout.log", runtime / "LogConsole.txt"] if path.is_file())
        errors = []
        if record.get("exit_code") != 0 or record.get("timed_out"):
            errors.append(f"engine exit={record.get('exit_code')} timeout={record.get('timed_out')}")
        if "[module-file-probe] PASS read_write_append_path_aliases" not in console:
            errors.append("file binding read/write/append witness missing")
        written = b"updated!\r\nappended\r\n" if sys.platform == "win32" else b"updated!\nappended\n"
        expected = b"installed\n" if private_store or read_only else written
        if (package / "seed.dat").read_bytes() != expected or sorted(path.name for path in package.iterdir()) != ["seed.dat"]:
            errors.append("Lua file writes alter the installed package" if private_store else "default Lua file operations differ")
        shadow = runtime / "Userdata/ScriptFiles/LuaFileProbe.rte/CampaignData"
        if read_only:
            if shadow.parent.exists():
                errors.append("read-only text open creates a private file store")
        elif private_store:
            for name, content in [("seed.dat", written)]:
                if not (shadow / name).is_file() or (shadow / name).read_bytes() != content:
                    errors.append(f"private campaign {name} is absent or differs")
            if shadow.is_dir() and sorted(path.name for path in shadow.iterdir()) != ["seed.dat"]:
                errors.append("private campaign removals leave a file or directory")
        elif shadow.exists():
            errors.append("unset file-root lever creates a private file store")
        if re.search(r"^ERROR:|RTE Aborted|RTE Assert|Assertion failed|stack traceback:", console, re.M):
            errors.append("engine or Lua error in the file probe log")
        case = "module_file_read_only" if read_only else "module_file_root_aliases" if roots else "module_file_isolation" if private_store else "module_file_defaults"
        result = {"case": case,
                  "pass": not errors, "errors": errors, "record": record}
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        return result
    finally:
        run.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--defaults", action="store_true")
    parser.add_argument("--roots", action="store_true")
    parser.add_argument("--read-only", action="store_true")
    options = parser.parse_args()
    result = file_probe(options.repo.resolve(), options.out.resolve(), not options.defaults, options.roots, options.read_only)
    print(json.dumps({key: value for key, value in result.items() if key != "record"}, indent=2))
    return int(not result["pass"])


if __name__ == "__main__":
    sys.exit(main())
