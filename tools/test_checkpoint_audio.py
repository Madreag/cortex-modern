"""Reload playing checkpoint voices and compare their full backend controls."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re

from run_sim_test import make_run
from run_selftests import score_selftest
from test_checkpoint_capture import stage_scenario


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    options = parser.parse_args()
    os.environ["CCCP_HEADLESS"] = "1"
    run = make_run(options.repo, ["-scenario", "P4 Alpha Duel", "-seed", "42", "-num-lua-states", "4", "-max-ticks", "3",
                                 "-checkpoint-audio-effects-selftest"], options.out, 120,
                   env={"CCCP_HEADLESS": "1"})
    stage_scenario(run)
    try:
        record = run.start().finish()
    finally:
        run.close()
    log = (options.out / "stdout.log").read_text(encoding="utf-8", errors="replace")
    console = options.out / "runtime/checkpoint-audio-console.log"
    if console.exists():
        log += "\n" + console.read_text(encoding="utf-8", errors="replace")
    score = score_selftest(log, record.get("exit_code"), record.get("timed_out"), "checkpoint-audio-effects-selftest")
    errors = re.findall(r"^.*ERROR:.*$", log, re.M)
    rows = re.findall(r"^\[checkpoint-audio-effects-selftest\] (PASS|FAIL) spatial=(false|true) .*", log, re.M)
    passed = score["pass"] and not errors and rows == [("PASS", "false"), ("PASS", "true")]
    result = {"passed": passed, "score": score, "errors": errors, "rows": rows}
    line = f"{'PASS' if passed else 'FAIL'} checkpoint audio: spatial and immobile controls; errors={len(errors)}"
    result["final_line"] = line
    (options.out / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(line)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
