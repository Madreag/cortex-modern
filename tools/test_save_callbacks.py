"""Check that script save callbacks are reflected in every part of the snapshot."""

import argparse
import json
from pathlib import Path
import re
import shutil

from run_sim_test import make_run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    setup = parser.add_mutually_exclusive_group(required=True)
    setup.add_argument("--recording", type=Path)
    setup.add_argument("--scenario")
    parser.add_argument("--out", type=Path, required=True)
    options = parser.parse_args()
    out = options.out.resolve()
    scenario = ["-net-replay", options.recording] if options.recording else ["-scenario", options.scenario, "-seed", 42]
    run = make_run(options.repo, [*scenario, "-num-lua-states", 4, "-max-ticks", 3,
                                 "-tick-hashes", "-out", out / "trace.json", "-save-callback-selftest"], out, 90)
    directory = Path(run.cwd) / "Userdata/UserSavedGames.rte"
    directory.mkdir()
    (directory / "Index.ini").write_text("DataModule\n\tModuleName = Scripted Activity Saves\n")
    scripts = Path(run.cwd) / "Userdata/UserScenes.rte"
    scripts.mkdir(exist_ok=True)
    (scripts / "Index.ini").write_text("DataModule\n\tModuleName = User Scenes\n\tScanFolderContents = 1\n")
    shutil.copy2(options.repo / "tools/fixtures/mod_save_callbacks.lua", scripts / "mod_save_callbacks.lua")
    try:
        record = run.start().finish()
    finally:
        run.close()
    log = (out / "stdout.log").read_text(errors="replace")
    console_file = Path(run.cwd) / "LogConsole.txt"
    console = console_file.read_text(errors="replace") if console_file.exists() else ""
    errors = re.findall(r"^.*(?:ERROR:|RTE Assert|RTE Abort|stack traceback).*$", log + "\n" + console, re.M)
    checks = {"exit": record["exit_code"] == 0 and not record["timed_out"],
              "callback": "[save-callback-selftest] PASS callback=1 loaded=1 images=1 scene=1 objects=1" in log,
              "cleanup": console_file.is_file(), "no_errors": not errors,
              "desktop": record["input_desktop_before"] == record["input_desktop_after"]}
    result = {"pass": all(checks.values()), "checks": checks, "errors": errors, "binary": record["exe_sha256"]}
    (out / "result.json").write_text(json.dumps(result, indent=2))
    print(json.dumps(result), flush=True)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
