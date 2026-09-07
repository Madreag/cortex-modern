"""Require diagnostic success and failure paths to complete normal shutdown."""

import argparse
import json
from pathlib import Path
import socket
import time

from run_sim_test import make_run


ARGUMENT_FAILURES = {
    "bad_replay_range": ["-net-replay-dump", "bad"],
    "missing_input_script": ["-input-script", "missing.txt"],
    "missing_ai_script": ["-ai-write-script", "missing.txt"],
    "bad_aim_speed": ["-digital-aim-speed", "bad"],
    "bad_prediction_depth": ["-local-prediction-depth", "bad"],
    "bad_invariance": ["-local-prediction-invariance", "bad"],
    "bad_expectation": ["-lpinv-expect", "bad"],
}

MENU_FAILURES = {
    "empty_menu": "# no steps\n",
    "bad_menu_command": "unknown_command\n",
    "bad_menu_assertion": "assert_screen DeliberatelyWrong\n",
    "bad_menu_control": "activate DeliberatelyMissing\n",
}


def session_case(repo, out, report_failure=False):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    runs = {}
    reports = {}
    for role in ("host", "client"):
        reports[role] = out / role / ("missing/report.json" if role == "host" and report_failure else "report.json")
        args = ["-net-host"] if role == "host" else ["-net-join", "127.0.0.1"]
        runs[role] = make_run(repo, [*args, "-net-port", port, "-net-exit-after-ready", "-net-allow-userdata",
                                    "-net-session-report", reports[role]], out / role, 30)
    records = {}
    try:
        runs["host"].start()
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if "[net-session] hosting" in (out / "host/stdout.log").read_text(errors="replace"):
                break
            if runs["host"].poll() is not None:
                raise RuntimeError("host exited before opening its listener")
            time.sleep(0.05)
        else:
            raise RuntimeError("host did not open its listener")
        runs["client"].start()
        records = {role: run.finish() for role, run in runs.items()}
    finally:
        for run in runs.values():
            run.close()
    checks = {}
    for role, record in records.items():
        expected_failure = role == "host" and report_failure
        log = (out / role / "stdout.log").read_text(errors="replace")
        checks[role + "_exit"] = record["exit_code"] == int(expected_failure) and not record["timed_out"]
        checks[role + "_ready"] = "[net-session] ready" in log
        checks[role + "_cleanup"] = (Path(runs[role].cwd) / "LogConsole.txt").is_file()
        checks[role + "_desktop"] = record["input_desktop_before"] == record["input_desktop_after"]
        checks[role + "_report"] = "[net-session] report failed:" in log if expected_failure else reports[role].is_file()
    return {"pass": all(checks.values()), "checks": checks, "binary": records["host"]["exe_sha256"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--recording", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--case", action="append")
    options = parser.parse_args()
    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    expected_arguments = {"bad_prediction_depth": "bad depth", "bad_invariance": "[lpinv] bad spec",
                          "bad_expectation": "[lpinv] bad expectation"}
    cases = {name: (args, 1, expected_arguments.get(name, args[0][1:])) for name, args in ARGUMENT_FAILURES.items()}
    for name, text in MENU_FAILURES.items():
        path = root / (name + ".txt")
        path.write_text(text)
        cases[name] = (["-menu-script", path], 1, "[menu-script] FAILED:")
    cases.update({
        "missing_menu": (["-menu-script", root / "missing.txt"], 1, "[menu-script] FAILED:"),
        "graph": (["-script-graph-selftest"], 0, "[script-graph-selftest] PASS"),
        "identity": (["-net-identity-dump", root / "identity.json"], 0, "[net-identity-dump] wrote"),
        "identity_report_failure": (["-net-identity-dump", root / "missing/identity.json"], 1, "[net-identity-dump] failed"),
        "replay": (["-net-replay-verify", options.recording, "-out", root / "replay.json"], 0, "[net-replay-verify]"),
        "missing_replay": (["-net-replay-verify", root / "missing.ccreplay"], 3, "could not open replay file"),
        "replay_report_failure": (["-net-replay-verify", options.recording, "-out", root / "missing/replay.json"], 1, "could not write"),
        "session": None,
        "session_report_failure": None,
    })
    results = {}
    for name in options.case or cases:
        if name in ("session", "session_report_failure"):
            results[name] = session_case(options.repo, root / name, name == "session_report_failure")
            print(json.dumps({"case": name, **results[name]}), flush=True)
            (root / "result.json").write_text(json.dumps({"pass": all(r["pass"] for r in results.values()), "results": results}, indent=2))
            continue
        args, code, expected = cases[name]
        run = make_run(options.repo, args, root / name, 90)
        try:
            record = run.start().finish()
        finally:
            run.close()
        log = (root / name / "stdout.log").read_text(errors="replace")
        checks = {"exit": record["exit_code"] == code and not record["timed_out"],
                  "diagnostic": expected in log,
                  "cleanup": (Path(run.cwd) / "LogConsole.txt").is_file(),
                  "desktop": record["input_desktop_before"] == record["input_desktop_after"]}
        if name in ("identity", "replay"):
            report = root / (name + ".json")
            try:
                checks["report"] = bool(json.loads(report.read_text()))
            except (OSError, ValueError):
                checks["report"] = False
        if name == "graph":
            checks["graph"] = "[script-graph-selftest] FAIL" not in log and "[script-graph-selftest] ERROR" not in log
        results[name] = {"pass": all(checks.values()), "checks": checks, "exit_code": record["exit_code"], "binary": record["exe_sha256"]}
        print(json.dumps({"case": name, **results[name]}), flush=True)
        (root / "result.json").write_text(json.dumps({"pass": all(r["pass"] for r in results.values()), "results": results}, indent=2))
    return 0 if all(r["pass"] for r in results.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
