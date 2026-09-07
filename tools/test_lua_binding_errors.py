"""Require recoverable native binding errors, including coroutine and GC paths."""

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
    parser.add_argument("--out", type=Path, required=True)
    options = parser.parse_args()
    out = options.out.resolve()
    run = make_run(options.repo, ["-net-replay", options.recording, "-num-lua-states", 4,
                                 "-max-ticks", 3, "-test-script", "UserScenes.rte/mod_binding_errors.lua"], out, 90)
    scripts = Path(run.cwd) / "Userdata/UserScenes.rte"
    scripts.mkdir(exist_ok=True)
    (scripts / "Index.ini").write_text("DataModule\n\tModuleName = User Scenes\n\tScanFolderContents = 1\n")
    shutil.copy2(options.repo / "tools/fixtures/mod_binding_errors.lua", scripts / "mod_binding_errors.lua")
    try:
        record = run.start().finish()
    finally:
        run.close()
    log = (out / "stdout.log").read_text(errors="replace")
    console_path = Path(run.cwd) / "LogConsole.txt"
    console = console_path.read_text(errors="replace") if console_path.exists() else ""
    errors = re.findall(r"^.*(?:ERROR:|RTE Assert|RTE Abort|stack traceback).*$", log + "\n" + console, re.M)
    actors = re.findall(r"\[lua-binding-errors\] PASS uid=(\d+) cases=401$", log + "\n" + console, re.M)
    checks = {"exit": record["exit_code"] == 0 and not record["timed_out"],
              "all_actors": len(set(actors)) == 4, "cleanup": console_path.is_file(), "no_errors": not errors,
              "desktop": record["input_desktop_before"] == record["input_desktop_after"]}
    result = {"pass": all(checks.values()), "checks": checks, "errors": errors, "binary": record["exe_sha256"]}
    (out / "result.json").write_text(json.dumps(result, indent=2))
    print(json.dumps(result), flush=True)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
