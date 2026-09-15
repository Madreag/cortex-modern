"""Detect a second Tests.rte load that aborts userdata creation.

The harness path already loads Tests.rte. A launch that also names it with
-module Tests.rte must not treat that second LoadDataModule as fatal, and
Userdata/UserSavedGames.rte/Index.ini must exist after the run.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_sim_test import make_run  # noqa: E402

ERROR_LINE = 'ERROR: Failed to load DataModule "Tests.rte"! Only official modules were loaded!'
MISSING_ERROR_PREFIX = "ERROR: Failed to load DataModule"
USERDATA_INDEX = Path("Userdata") / "UserSavedGames.rte" / "Index.ini"
LOAD_MARK = "Tests.rte"


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace") if path.is_file() else ""


def tests_load_count(log_loading: Path) -> int:
    text = read_text(log_loading)
    return sum(1 for line in text.splitlines() if LOAD_MARK in line and "loading" in line.lower())


def run_case(repo: Path, out: Path, timeout: float, extra_args: list[str]) -> dict:
    os.environ["CCCP_HEADLESS"] = "1"
    args = ["-scenario", "SimBaseline", "-seed", "42", "-max-ticks", "60", *extra_args]
    run = make_run(repo, args, out, timeout, env={"CCCP_HEADLESS": "1"})
    try:
        record = run.start().finish()
    finally:
        run.close()
    runtime = out / "runtime"
    console = read_text(runtime / "LogConsole.txt")
    stdout = read_text(out / "stdout.log")
    combined = console + "\n" + stdout
    index_path = runtime / USERDATA_INDEX
    error_lines = [line for line in combined.splitlines() if ERROR_LINE in line or line.startswith(MISSING_ERROR_PREFIX)]
    return {
        "args": args,
        "exit_code": record.get("exit_code"),
        "timed_out": record.get("timed_out"),
        "exe_sha256": record.get("exe_sha256"),
        "console": str(runtime / "LogConsole.txt"),
        "log_loading": str(runtime / "LogLoading.txt"),
        "error_line_present": ERROR_LINE in combined,
        "error_lines": error_lines[:8],
        "userdata_index": str(index_path),
        "userdata_index_exists": index_path.is_file(),
        "tests_rte_load_lines": tests_load_count(runtime / "LogLoading.txt"),
        "log_text": combined,
    }


def score_detect(case: dict) -> dict:
    from run_selftests import score_selftest
    process = score_selftest(case.get("log_text") or "", case.get("exit_code"), case.get("timed_out") is True)
    ok = (
        (not case["error_line_present"])
        and case["userdata_index_exists"]
        and process["exit_code"] == 0
        and process["timed_out"] is False
        and not process["fatal"]
    )
    if case["error_line_present"] and not case["userdata_index_exists"]:
        reason = "ERROR line present and UserSavedGames Index.ini absent"
    elif case["error_line_present"]:
        reason = "ERROR line present"
    elif not case["userdata_index_exists"]:
        reason = "UserSavedGames Index.ini absent"
    elif process["exit_code"] != 0 or process["timed_out"] or process["fatal"]:
        reason = process["reason"]
    else:
        reason = ""
    return {
        "pass": bool(ok),
        "exit_code": case.get("exit_code"),
        "timed_out": case.get("timed_out"),
        "pass_lines": 1 if ok else 0,
        "fail_lines": [] if ok else [reason],
        "fatal": process["fatal"],
        "reason": reason,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument(
        "--arm",
        choices=("detect", "no_module", "missing"),
        default="detect",
        help="detect is the suite row; no_module and missing are lane checks",
    )
    parser.add_argument("--missing-module", default="NoSuchModule.rte")
    options = parser.parse_args()
    os.environ["CCCP_HEADLESS"] = "1"
    repo, root = options.repo.resolve(), options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)

    extra = []
    if options.arm == "detect":
        extra = ["-module", "Tests.rte"]
    elif options.arm == "missing":
        extra = ["-module", options.missing_module]

    case = run_case(repo, root / options.arm, options.timeout, extra)
    if options.arm == "detect":
        scored = score_detect(case)
        token = "[single-module-harness] PASS" if scored["pass"] else "[single-module-harness] FAIL"
    elif options.arm == "no_module":
        ok = (not case["error_line_present"]) and case["tests_rte_load_lines"] == 1 and case["userdata_index_exists"]
        scored = {"pass": ok, "reason": "" if ok else f"error={case['error_line_present']} loads={case['tests_rte_load_lines']} index={case['userdata_index_exists']}"}
        token = f"[single-module-harness] {'PASS' if ok else 'FAIL'} no_module loads={case['tests_rte_load_lines']}"
    else:
        missing_line = f'ERROR: Failed to load DataModule "{options.missing_module}"! Only official modules were loaded!'
        combined_errors = case["error_lines"]
        refused = any(missing_line in line or options.missing_module in line for line in combined_errors) or any(
            options.missing_module in line for line in combined_errors
        )
        if not refused:
            console = read_text(Path(case["console"]))
            stdout = read_text(root / options.arm / "stdout.log")
            refused = missing_line in console or missing_line in stdout or options.missing_module in console or options.missing_module in stdout
            if missing_line in console or missing_line in stdout:
                case["error_lines"] = [missing_line]
        scored = {"pass": bool(refused), "reason": "" if refused else "missing-module refusal line absent"}
        token = f"[single-module-harness] {'PASS' if refused else 'FAIL'} missing {options.missing_module}"

    result = {"arm": options.arm, "case": case, "scored": scored}
    (root / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(token, flush=True)
    print(json.dumps({"arm": options.arm, "pass": scored.get("pass"), "reason": scored.get("reason"),
                      "error_line_present": case["error_line_present"],
                      "userdata_index_exists": case["userdata_index_exists"],
                      "tests_rte_load_lines": case["tests_rte_load_lines"],
                      "error_lines": case["error_lines"],
                      "out": str(root)}, indent=2), flush=True)
    return 0 if scored.get("pass") else 1


if __name__ == "__main__":
    raise SystemExit(main())
