"""Exercise menu transitions and shutdown in private Windows game runtimes."""

import argparse
import json
from pathlib import Path
import re

from run_sim_test import make_run


SCRIPTS = {
    "main_exit": "wait 40\nassert_screen MainScreen\nwait 150\nexit\n",
    "quit_button": "wait 40\nassert_screen MainScreen\nactivate ButtonQuit\nwait 5\nassert_screen ShouldHaveQuit\n",
    "join_exit": "wait 40\nactivate ButtonMainToMultiplayer\nwait 12\nactivate ButtonMultiplayerJoinGame\nwait 12\nassert_substate JoinSetup\nwait 30\nactivate ButtonJoinBack\nwait 10\nassert_substate Landing\nwait 150\nexit\n",
    "credits_exit": "wait 40\nactivate ButtonMainToCreds\nwait 60\nassert_screen CreditsScreen\nwait 150\nexit\n",
    # §9b's moderation panel: every control the host's seat list needs is in the skin, with no match
    # and no socket - a name that never resolved would only show up in a live gate otherwise.
    "moderation_panel": "wait 40\nactivate ButtonMainToMultiplayer\nwait 12\nassert_substate Landing\nassert_control ButtonMultiplayerModerate\nassert_control MultiplayerModerationPanel\nassert_control LabelModerationTitle\nassert_control LabelModerationSummary\nassert_control LabelModerationStatus\nassert_control ButtonModerationBack\nassert_control LabelModerationSeat0\nassert_control ButtonModerationApplicant0\nassert_control ButtonModerationWait0\nassert_control ButtonModerationSubstitute0\nassert_control ButtonModerationCancel0\nassert_control LabelModerationSeat1\nassert_control ButtonModerationApplicant1\nassert_control ButtonModerationWait1\nassert_control ButtonModerationSubstitute1\nassert_control ButtonModerationCancel1\nassert_control LabelModerationSeat2\nassert_control ButtonModerationApplicant2\nassert_control ButtonModerationWait2\nassert_control ButtonModerationSubstitute2\nassert_control ButtonModerationCancel2\nwait 20\nexit\n",
}

REQUIRED = {
    "main_exit": "assert_screen expected=MainScreen actual=MainScreen PASS",
    "quit_button": "activate ButtonQuit ok=1",
    "join_exit": "assert_substate expected=Landing actual=Landing PASS",
    "credits_exit": "assert_screen expected=CreditsScreen actual=CreditsScreen PASS",
    "moderation_panel": "assert_control ButtonModerationCancel2 PASS",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--case", action="append", choices=SCRIPTS)
    options = parser.parse_args()
    if options.repeat < 1:
        parser.error("--repeat must be positive")
    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    results = []
    for name in options.case or SCRIPTS:
        script = root / (name + ".txt")
        script.write_text(SCRIPTS[name], encoding="utf-8")
        for repeat in range(options.repeat):
            out = root / f"{name}_{repeat + 1}"
            run = make_run(options.repo, ["-menu-script", script], out, 120)
            try:
                record = run.start().finish()
            finally:
                run.close()
            log = (out / "stdout.log").read_text(encoding="utf-8-sig", errors="replace")
            errors = re.findall(r"^.*(?:FAILED|FAIL|Runtime Error|EXCEPTION_|RTE Assert|RTE Abort).*$", log, re.M)
            checks = {
                "process": record["exit_code"] == 0 and not record["timed_out"],
                "steps": REQUIRED[name] in log and not errors,
                "desktop": record["input_desktop_before"] == record["input_desktop_after"],
            }
            result = {"name": name, "repeat": repeat + 1, "pass": all(checks.values()),
                      "checks": checks, "errors": errors, "exit_code": record["exit_code"],
                      "binary": record["exe_sha256"]}
            results.append(result)
            (root / "result.json").write_text(json.dumps({"results": results, "pass": all(r["pass"] for r in results)}, indent=2), encoding="utf-8")
            print(json.dumps(result), flush=True)
    return 0 if all(r["pass"] for r in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
