"""Exercise leave, dropped clients, and replacement joins through the real menu handlers."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import re
import time

from compare_sim_traces import load_trace, strict_compare
from run_sim_test import make_run


def menu_script(name, host, players, port):
    script = f"wait 40\nactivate ButtonMainToMultiplayer\nwait 12\nsettext TextMultiplayerName {name}\n"
    if host:
        return script + f"activate ButtonMultiplayerHostGame\nwait 10\nsettext TextHostPort {port}\nsettext TextHostPlayers {players}\nsettext TextHostInputDelay 3\nactivate ButtonMultiplayerCreate\n"
    return script + f"activate ButtonMultiplayerJoinGame\nwait 10\nsettext TextJoinAddress 127.0.0.1\nsettext TextJoinPort {port}\nactivate ButtonMultiplayerConnect\nwait_connected {players}\nactivate ButtonMultiplayerReady\n"


def wait_for_log(run, marker, seconds=45):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        log = run.out / "stdout.log"
        if log.exists() and marker in log.read_text(errors="replace"):
            return
        if run.poll() is not None:
            raise RuntimeError(f"{run.out.name} ended before {marker}")
        time.sleep(0.05)
    raise RuntimeError(f"{run.out.name} did not reach {marker}")


def read_lockstep(out):
    """This peer's own view of the round, from -net-match-report; absent when it never started one."""
    path = out / "lockstep-report.json"
    if not path.exists():
        return None
    try:
        report = json.loads(path.read_text(errors="replace"))
    except ValueError as error:
        return {"report_error": str(error)}
    lockstep = report.get("runner", {}).get("lockstep")
    if not isinstance(lockstep, dict):
        return {"report_error": "no runner.lockstep block", "service_state": report.get("state")}
    return lockstep


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--players", type=int, choices=[2, 3, 4], default=3)
    parser.add_argument("--action", choices=["leave", "rejoin", "drop"], default="rejoin")
    parser.add_argument("--port", type=int, default=44160)
    parser.add_argument("--short-trace-control", action="store_true", help="require the replacement to report an incomplete longer trace")
    parser.add_argument("--fake-lag-ms", type=int, default=0)
    options = parser.parse_args()
    if options.short_trace_control and options.action == "leave":
        parser.error("--short-trace-control requires a replacement")
    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    runs = {}
    records = {}
    match = options.action != "leave"
    result = {"pass": False, "action": options.action, "players": options.players}

    def start(name, script, trace=False):
        path = root / f"{name}.txt"
        path.write_text(script, encoding="utf-8")
        out = root / name
        args = ["-menu-script", path, "-net-fake-lag", options.fake_lag_ms,
                "-net-match-report", out / "lockstep-report.json"]
        if trace:
            ticks = 360 if options.short_trace_control and name == "replacement" else 180
            args += ["-tick-hashes", "-max-ticks", ticks, "-out", out / "trace.json"]
        runs[name] = make_run(options.repo, args, out, 120).start()
        return runs[name]

    try:
        players = options.players
        host = menu_script("Host", True, players, options.port)
        host += f"wait_connected {players}\nwait_remote_ready\nwait_all_ready\ndump_lobby\nwait_connected {players - 1}\nassert_substate Lobby\nassert_enabled ButtonMultiplayerStart 0\ndump_lobby\n"
        if match:
            host += f"wait_connected {players}\nwait_remote_ready\nwait_all_ready\ndump_lobby\nactivate ButtonMultiplayerStart\nwait 99999\n"
        else:
            host += "wait 50\nactivate ButtonMultiplayerLeave\nwait 15\nassert_substate Landing\nexit\n"
        host_run = start("host", host, match)
        wait_for_log(host_run, "activate ButtonMultiplayerCreate ok=1")
        for index in range(1, players):
            name = "departing" if index == 1 else f"stayer{index}"
            display = "Departing" if index == 1 else f"Stayer{index}"
            script = menu_script(display, False, players, options.port) + "wait_all_ready\ndump_lobby\n"
            if index == 1:
                script += "wait 99999\n" if options.action == "drop" else "wait 15\nactivate ButtonMultiplayerLeave\nwait 15\nassert_substate Landing\nexit\n"
            elif match:
                script += f"wait_connected {players - 1}\nassert_substate Lobby\ndump_lobby\nwait_connected {players}\nwait_all_ready\ndump_lobby\nwait 99999\n"
            else:
                script += "wait_state Failed\nwait 5\nassert_substate Landing\nexit\n"
            start(name, script, match and index != 1)
        if options.action == "drop":
            wait_for_log(runs["departing"], "[menu-script] dump_lobby")
            wait_for_log(host_run, "allready -> OK")
            runs["departing"].terminate()
        wait_for_log(host_run, f"connected:{players - 1} -> OK")
        records["departing"] = runs["departing"].finish()
        if match:
            script = menu_script("Replacement", False, players, options.port) + "wait_all_ready\ndump_lobby\nwait 99999\n"
            start("replacement", script, True)
        pending = [(name, run) for name, run in runs.items() if name not in records]
        with ThreadPoolExecutor(max_workers=len(pending)) as pool:
            for name, record in pool.map(lambda pair: (pair[0], pair[1].finish()), pending):
                records[name] = record
        checks, details, lockstep = {}, {}, {}
        for name, record in records.items():
            log = (root / name / "stdout.log").read_text(errors="replace")
            errors = re.findall(r"^.*(?:FAILED|FAIL|EXCEPTION_|RTE Assert|RTE Abort|Runtime Error).*$", log, re.M)
            short_trace = options.short_trace_control and name == "replacement"
            if short_trace:
                errors = [line for line in errors if not re.fullmatch(r"\[menu-mp\] FAIL: collected \d+ of 360 requested ticks", line)]
            expected_exit = 137 if name == "departing" and options.action == "drop" else (1 if short_trace else 0)
            checks[f"{name}_process"] = record["exit_code"] == expected_exit and not record["timed_out"]
            checks[f"{name}_steps"] = not errors and "allready -> OK" in log
            checks[f"{name}_desktop"] = record["input_desktop_before"] == record["input_desktop_after"]
            checks[f"{name}_binary"] = record["exe_sha256"] == records["host"]["exe_sha256"]
            details[name] = {"exit": record["exit_code"], "binary": record["exe_sha256"], "errors": errors,
                             "rosters": [line for line in log.splitlines() if "dump_lobby" in line],
                             "net_match": [line for line in log.splitlines() if "[net-match] " in line]}
            stats = read_lockstep(root / name)
            if stats is not None:
                lockstep[name] = stats
            if match and name != "departing":
                if short_trace:
                    ticks, _ = load_trace(root / name / "trace.json")
                    checks["short_trace_refused"] = len(ticks) < 360 and "[menu-mp] FAIL: collected" in log and "[menu-mp] trace stopped:" in log
                    checks[f"{name}_simulation"], details[name]["comparison"] = strict_compare(root / "host/trace.json", root / name / "trace.json", min(180, len(ticks)), prefix=True)
                else:
                    checks[f"{name}_simulation"], details[name]["comparison"] = strict_compare(root / "host/trace.json", root / name / "trace.json", 180)
                checks[f"{name}_replacement_name"] = "Replacement(team" in log
            elif expected_exit == 0:
                checks[f"{name}_landing"] = "assert_substate expected=Landing actual=Landing PASS" in log
        host_log = (root / "host/stdout.log").read_text(errors="replace")
        checks["host_survived_departure"] = "assert_substate expected=Lobby actual=Lobby PASS" in host_log and "assert_enabled ButtonMultiplayerStart expected=0 actual=0 PASS" in host_log
        result.update(pass_=all(checks.values()), checks=checks, details=details)
        result["pass"] = result.pop("pass_")
        result["lockstep"] = lockstep
        (root / "lockstep-stats.json").write_text(json.dumps(lockstep, indent=2), encoding="utf-8")
    except Exception as error:
        result["error"] = str(error)
    finally:
        for run in runs.values():
            run.close()
        (root / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({"pass": result["pass"], "error": result.get("error"),
                      "failed": [key for key, value in result.get("checks", {}).items() if not value], "out": str(root)}), flush=True)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
