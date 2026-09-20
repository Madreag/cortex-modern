"""Compare complete synchronous and worker checkpoint archives from the same ticks."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import zipfile

from run_sim_test import make_run


def stage_scenario(run) -> None:
    module = Path(run.cwd) / "Userdata/UserScenes.rte"
    module.mkdir(exist_ok=False)
    (module / "Index.ini").write_text(
        "DataModule\n\tModuleName = User Scenes\n\tAddActivity = GAScripted\n"
        "\t\tCopyOf = P4 Alpha Duel\n\t\tPresetName = Determinism P4 Alpha Duel\n",
        encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    options = parser.parse_args()
    os.environ["CCCP_HEADLESS"] = "1"
    run = make_run(options.repo, ["-scenario", "P4 Alpha Duel", "-seed", "42", "-num-lua-states", "4", "-max-ticks", "122",
                                 "-checkpoint-capture-selftest"], options.out, 180,
                   env={"CCCP_HEADLESS": "1"})
    stage_scenario(run)
    try:
        record = run.start().finish()
    finally:
        run.close()
    log = (options.out / "stdout.log").read_text(encoding="utf-8", errors="replace")
    rows = re.findall(r"^\[checkpoint-capture-selftest\] (PASS|FAIL) tick=(\d+) .*", log, re.M)
    comparisons = {}
    for tick in (60, 120):
        root = options.out / f"runtime/Autosaves/capture-{tick}"
        paths = [root / name for name in ("sync.ccsave", "image.ccsave")]
        if not all(path.exists() for path in paths):
            comparisons[tick] = {"same_bytes": False, "error": "missing archive"}
            continue
        payloads = [path.read_bytes() for path in paths]
        with zipfile.ZipFile(paths[0]) as sync, zipfile.ZipFile(paths[1]) as image:
            names = sorted(set(sync.namelist()) | set(image.namelist()))
            different = [name for name in names if name not in sync.namelist() or name not in image.namelist()
                         or sync.read(name) != image.read(name)]
            valid = sync.testzip() is None and image.testzip() is None
        comparisons[tick] = {"same_bytes": payloads[0] == payloads[1] and valid, "different_members": different,
                             "sync_sha256": hashlib.sha256(payloads[0]).hexdigest(),
                             "image_sha256": hashlib.sha256(payloads[1]).hexdigest(),
                             "sync_bytes": len(payloads[0]), "image_bytes": len(payloads[1])}
    passed = (record.get("exit_code") == 0 and not record.get("timed_out")
              and rows == [("PASS", "60"), ("PASS", "120")]
              and all(row["same_bytes"] for row in comparisons.values()))
    line = f"{'PASS' if passed else 'FAIL'} checkpoint capture: complete archives match at ticks 60 and 120"
    result = {"passed": passed, "rows": rows, "comparisons": comparisons, "final_line": line}
    (options.out / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(line)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
