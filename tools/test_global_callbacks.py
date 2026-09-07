"""Check native global callback dispatch and continuation after memory/file restores."""

import argparse
import json
from pathlib import Path
import re
import shutil

from run_sim_test import make_run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--recording", type=Path, required=True)
    parser.add_argument("--late", action="store_true")
    parser.add_argument("--out", type=Path, required=True)
    options = parser.parse_args()
    out = options.out.resolve()
    run = make_run(options.repo, ["-net-replay", options.recording, "-num-lua-states", 4,
                                 "-max-ticks", 3, "-global-callback-selftest"], out, 90)
    module = Path(run.cwd) / "Userdata/UserScenes.rte"
    module.mkdir(exist_ok=True)
    shutil.copy2(options.repo / "tools/fixtures/mod_global_events.lua", module / "mod_global_events.lua")
    (module / "Index.ini").write_text("DataModule\n\tModuleName = User Scenes\n"
        "\tAddGlobalScript = GlobalScript\n\t\tPresetName = Checkpoint Global\n"
        "\t\tScriptPath = UserScenes.rte/mod_global_events.lua\n\t\tLuaClassName = CheckpointGlobalScript\n"
        f"\t\tLateUpdate = {int(options.late)}\n")
    with (Path(run.cwd) / "Userdata/Settings.ini").open("a") as stream:
        stream.write("\n\tEnableGlobalScript = UserScenes.rte/Checkpoint Global\n")
    saves = Path(run.cwd) / "Userdata/UserSavedGames.rte"
    saves.mkdir()
    (saves / "Index.ini").write_text("DataModule\n\tModuleName = Scripted Activity Saves\n")
    try:
        record = run.start().finish()
    finally:
        run.close()
    log = (out / "stdout.log").read_text(errors="replace")
    console_file = Path(run.cwd) / "LogConsole.txt"
    console = console_file.read_text(errors="replace") if console_file.exists() else ""
    errors = re.findall(r"^.*(?:ERROR:|RTE Assert|RTE Abort|stack traceback).*$", log + "\n" + console, re.M)
    checks = {"exit": record["exit_code"] == 0 and not record["timed_out"],
              "callbacks": "[global-callback-selftest] PASS guards=1 initial=1 memory=1 file=1" in log,
              "single_start": len(re.findall(r"\[global-events\] started=1", console)) == 1,
              "cleanup": console_file.is_file(), "no_errors": not errors,
              "desktop": record["input_desktop_before"] == record["input_desktop_after"]}
    result = {"pass": all(checks.values()), "checks": checks, "errors": errors, "binary": record["exe_sha256"]}
    (out / "result.json").write_text(json.dumps(result, indent=2))
    print(json.dumps(result), flush=True)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
