"""Fight 15 detecting checks on one hidden engine and four SP picture guards.

The baseline contains the same native detecting fixture, with its production code
unchanged at 367fb9cf97. This driver never starts another machine or a spread run.
"""
from __future__ import annotations

import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import re

from PIL import Image

from run_sim_test import file_sha256, make_run, seed_settings
from test_connection_indicator import SIZES, compare_guard, guard_steps, run_single

BASE = "367fb9cf97e06a7d701653ac90c2f6f268290956"
ROWS = tuple(f"R{number}" for number in range(1, 9))


def settled_guard_steps():
    steps = guard_steps()
    # The camera scrolls in real time and controller icons expire after thirty seconds.
    # Capture the whole frozen simulation after that interval on cold and warm starts alike.
    steps[2]["renders"] = 30
    steps[2]["elapsed_ms"] = 31000
    return steps


def native_case(repo, root, row, size, exe=None):
    run = make_run(repo, ["-fight15-selftest", row], root, timeout=240,
                   env={"CCCP_HEADLESS": "1", "CC_RUNNER_IGNORE_FULLSCREEN": "1"})
    if exe:
        run.argv[0] = str(exe.resolve())
    width, height = map(int, size.split("x"))
    seed_settings(run, {"ResolutionX": width, "ResolutionY": height,
                       "ResolutionMultiplier": 1, "NetworkShowDiagnostics": 0})
    try:
        record = run.start().finish()
    finally:
        run.close()
    log = (root / "stdout.log").read_text(encoding="utf-8", errors="replace")
    lines = [line for line in log.splitlines() if line.startswith("[fight15-selftest]")]
    fails = [line for line in lines if f"FAIL {row} " in line]
    passes = [line for line in lines if f"PASS {row} " in line]
    close = re.search(r"R7 close_started_unix_ms=(\d+)", log)
    close_ms = datetime.fromisoformat(record["ended_utc"]).timestamp() * 1000 - int(close[1]) if close else None
    pictures = []
    for path in sorted((Path(run.cwd) / "ScreenShots").glob("fight15_" + row + "_*.png")):
        with Image.open(path) as shot:
            pictures.append({"path": str(path), "size": list(shot.size),
                             "ink_pixels": sum(pixel != (0, 0, 0) for pixel in shot.convert("RGB").get_flattened_data())})
    return {"exit_code": record.get("exit_code"), "timed_out": record.get("timed_out"),
            "evidence_complete": record.get("evidence_complete"), "passes": passes,
            "fails": fails, "lines": lines, "size": size, "close_to_exit_ms": close_ms,
            "pictures": pictures, "runtime": str(run.cwd), "exe_sha256": file_sha256(run.argv[0])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--baseline-repo", type=Path, required=True)
    parser.add_argument("--baseline-exe", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--case", choices=ROWS)
    parser.add_argument("--guards-only", action="store_true")
    parser.add_argument("--skip-guards", action="store_true")
    options = parser.parse_args()
    os.environ["CC_RUNNER_IGNORE_FULLSCREEN"] = "1"
    options.out.mkdir(parents=True, exist_ok=False)
    result = {"baseline": BASE, "topology": "single-engine", "multi_machine_proof": False,
              "cases": {}, "guards": {}, "pass": True}
    if not options.guards_only:
        for row in (options.case,) if options.case else ROWS:
            sizes = ("960x540", "1280x720") if row == "R4" else ("960x540",)
            for size in sizes:
                name = row + "-" + size
                before = native_case(options.baseline_repo, options.out / ("base-" + name), row, size, options.baseline_exe)
                after = native_case(options.repo, options.out / ("tip-" + name), row, size)
                red = before["exit_code"] == 1 and bool(before["fails"]) and not before["timed_out"] and before["evidence_complete"]
                green = after["exit_code"] == 0 and bool(after["passes"]) and not after["fails"] and not after["timed_out"] and after["evidence_complete"]
                if row == "R7":
                    green &= after["close_to_exit_ms"] is not None and 0 <= after["close_to_exit_ms"] < 1500
                if row in ("R4", "R5"):
                    expected_size = list(map(int, size.split("x")))
                    green &= bool(after["pictures"]) and all(shot["size"] == expected_size and shot["ink_pixels"] > 1000 for shot in after["pictures"])
                case = {"base": before, "tip": after, "red": red, "green": green, "pass": bool(red and green)}
                result["cases"][name] = case
                result["pass"] &= case["pass"]
                print(("RED " if red else "FAIL baseline ") + name + " " + (before["fails"][0] if before["fails"] else str(before["exit_code"])), flush=True)
                print(("GREEN " if green else "FAIL tip ") + name + " " + (after["fails"][0] if after["fails"] else after["passes"][-1] if after["passes"] else str(after["exit_code"])), flush=True)
    if not options.skip_guards and not options.case:
        for size in SIZES:
            before_root, after_root = options.out / ("guard-base-" + size), options.out / ("guard-tip-" + size)
            before = run_single(options.baseline_repo, before_root, settled_guard_steps(), size, exe=options.baseline_exe, guard=True)
            after = run_single(options.repo, after_root, settled_guard_steps(), size, guard=True)
            guard = {**compare_guard(before_root, after_root, size), "base_probe_pass": before["pass"], "tip_probe_pass": after["pass"]}
            guard["pass"] &= before["pass"] and after["pass"]
            result["guards"][size] = guard
            result["pass"] &= guard["pass"]
            print(("GREEN " if guard["pass"] else "FAIL ") + "single-player " + size + " " + json.dumps(guard), flush=True)
    (options.out / "fight15-checks.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
