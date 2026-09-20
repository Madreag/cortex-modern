"""Reload a whole world while its actor's spatial sound is playing."""

import argparse
import json
import os
from pathlib import Path
import re

from run_selftests import score_selftest
from run_sim_test import make_run
from test_checkpoint_capture import stage_scenario


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    options = parser.parse_args()
    os.environ["CCCP_HEADLESS"] = "1"
    run = make_run(options.repo, ["-scenario", "P4 Alpha Duel", "-seed", "42", "-num-lua-states", "4",
                                 "-max-ticks", "62", "-checkpoint-audio-world-selftest"], options.out, 180,
                   env={"CCCP_HEADLESS": "1"})
    stage_scenario(run)
    try:
        record = run.start().finish()
    finally:
        run.close()
    log = (options.out / "stdout.log").read_text(encoding="utf-8", errors="replace")
    console = options.out / "runtime/checkpoint-audio-world-console.log"
    errors = re.findall(r"^.*ERROR:.*$", console.read_text(encoding="utf-8", errors="replace"), re.M) if console.exists() else ["console missing"]
    score = score_selftest(log, record.get("exit_code"), record.get("timed_out"), "checkpoint-audio-world-selftest")
    passed = score["pass"] and not errors
    line = f"{'PASS' if passed else 'FAIL'} checkpoint world audio: controls and playback preserved; errors={len(errors)}"
    (options.out / "result.json").write_text(json.dumps({"passed": passed, "score": score, "errors": errors,
                                                        "final_line": line}, indent=2) + "\n", encoding="utf-8")
    print(line)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
