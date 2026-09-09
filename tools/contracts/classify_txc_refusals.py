"""Establish, from each refusal case's own runtime log, the stage at which the engine turned the candidate down.

The two stages the early/late grouping in prepare_transaction_control_commands.py names are distinct code paths:

  read_validate  ActivityMan::ReadSavedGame runs LuaStateWrapper::ValidateScriptGraph over every graph and throws on
                 the first problem, so LoadGameToRestart fails and LoadAndLaunchGame returns before a candidate is
                 ever staged. Logged as: ERROR: Could not load game "<name>": script graph validation failed: ...
  restore        RestartActivity has already set the live world aside and built the candidate; MovableMan::
                 RestoreScriptGraphs then fails in Graph.prepare/deserialize. Logged as:
                 ERROR: the saved script state did not restore: ... and [scriptgraph] restore failed: ...

Anything else the engine refuses on is reported under its own name rather than folded into one of those two.
"""

from pathlib import Path
import argparse
import json
import re

EARLY = [
    "bad_header_first",
    "bad_header_last",
    "truncated_graph",
    "bad_vm_rng",
    "unknown_native_class",
    "bad_closure",
    "unknown_closure_factory",
]
LATE = [
    "missing_native_preset",
    "missing_native_reference",
    "invalid_coroutine_pc",
    "invalid_iterator",
]

PATTERNS = [
    (
        "read_validate",
        re.compile(
            r'^.*ERROR: Could not load game "[^"]*": script graph validation failed:.*$',
            re.M,
        ),
    ),
    (
        "read_other",
        re.compile(
            r'^.*ERROR: Could not load game "[^"]*": (?!script graph validation failed).*$',
            re.M,
        ),
    ),
    (
        "restart_validate",
        re.compile(r"^.*ERROR: the saved script state is invalid:.*$", re.M),
    ),
    (
        "restore",
        re.compile(r"^.*ERROR: the saved script state did not restore:.*$", re.M),
    ),
]
RESTORE_FAILED = re.compile(r"^.*\[scriptgraph\] restore failed:.*$", re.M)
OTHER_ERRORS = re.compile(r"^.*ERROR: the saved (?!script state).*$", re.M)
CRASH = re.compile(r"^.*(?:RTE Assert|RTE Abort|stack traceback).*$", re.M)
COMPLETE = re.compile(
    r"\[contract-audit\] complete operation=\S+ prepared=\d+ applied=\d+ differences=\d+"
)
STAGE_OF = {
    "read_validate": "early",
    "read_other": "early",
    "restart_validate": "early",
    "restore": "late",
}


def text_of(case: Path) -> str:
    parts = []
    for name in ("stdout.log", "runtime/LogConsole.txt", "trace.json.console.txt"):
        path = case / name
        if path.exists():
            parts.append(path.read_text(encoding="utf-8-sig", errors="replace"))
    return "\n".join(parts)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--out", type=Path, required=True, help="the refusals job output directory"
    )
    parser.add_argument("--report", type=Path)
    options = parser.parse_args()

    result = json.loads((options.out / "result.json").read_text(encoding="utf-8"))
    by_case = {item["case"]: item for item in result["results"]}
    rows = []
    for name in EARLY + LATE:
        expected = "early" if name in EARLY else "late"
        log = text_of(options.out / f"ordinary-load_{name}_100")
        hits = {
            label: [line.strip() for line in pattern.findall(log)]
            for label, pattern in PATTERNS
        }
        fired = [label for label, lines in hits.items() if lines]
        stages = sorted({STAGE_OF[label] for label in fired})
        item = by_case.get(f"ordinary-load_{name}_100", {})
        continuation = item.get("continuation", {})
        complete = COMPLETE.search(log)
        rows.append(
            {
                "case": name,
                "expected_stage": expected,
                "mechanisms": fired,
                "observed_stage": stages[0]
                if len(stages) == 1
                else ("none" if not stages else "+".join(stages)),
                "stage_matches": stages == [expected],
                "applied": item.get("applied"),
                "prepared": item.get("prepared"),
                "raw_field_differences": item.get("raw_field_differences"),
                "continuation_passed": continuation.get("passed"),
                "continuation_status": continuation.get("status"),
                "failed_checks": [
                    key for key, ok in continuation.get("checks", {}).items() if not ok
                ],
                "gate_failures": item.get("gate_failures"),
                "refusal_message": next(
                    (lines[0][:240] for _, lines in hits.items() if lines), None
                ),
                "restore_failed": [
                    line.strip()[:240] for line in RESTORE_FAILED.findall(log)
                ][:2],
                "other_saved_state_errors": [
                    line.strip()[:160] for line in OTHER_ERRORS.findall(log)
                ][:4],
                "crash_lines": [line.strip()[:160] for line in CRASH.findall(log)][:3],
                "complete_line": complete.group(0) if complete else None,
            }
        )

    report = {
        "out": str(options.out),
        "job_complete": result.get("complete"),
        "source_unchanged": result.get("source_unchanged"),
        "continuation_checks_passed": result.get("continuation_checks_passed"),
        "all_refused": all(row["applied"] is False for row in rows),
        "all_stages_as_grouped": all(row["stage_matches"] for row in rows),
        "all_continuations_passed": all(row["continuation_passed"] for row in rows),
        "no_crash_lines": all(not row["crash_lines"] for row in rows),
        "cases": rows,
    }
    if options.report:
        options.report.write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))
    return (
        0
        if all(
            report[key]
            for key in (
                "all_refused",
                "all_stages_as_grouped",
                "all_continuations_passed",
                "no_crash_lines",
            )
        )
        else 1
    )


if __name__ == "__main__":
    raise SystemExit(main())
