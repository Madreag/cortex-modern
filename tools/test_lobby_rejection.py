"""Show a rejected join on both menus, then admit a compatible replacement and play."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import re

from compare_sim_traces import strict_compare
from run_sim_test import make_run
from test_lobby_lifecycle import wait_for_log


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=44210)
    options = parser.parse_args()
    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    runs, records = {}, {}
    result = {"pass": False}
    reason = "deterministic config"

    def start(name, host, suffix, extra=(), trace=False):
        script = f"wait 40\nactivate ButtonMainToMultiplayer\nwait 12\nsettext TextMultiplayerName {name}\n"
        if host:
            script += f"activate ButtonMultiplayerHostGame\nwait 10\nsettext TextHostPort {options.port}\nsettext TextHostPlayers 2\nsettext TextHostInputDelay 3\nactivate ButtonMultiplayerCreate\n"
        else:
            script += f"activate ButtonMultiplayerJoinGame\nwait 10\nsettext TextJoinAddress 127.0.0.1\nsettext TextJoinPort {options.port}\nactivate ButtonMultiplayerConnect\n"
        path = root / f"{name}.txt"
        path.write_text(script + suffix)
        args = ["-menu-script", path, "-num-lua-states", 4, *extra]
        if trace:
            args += ["-tick-hashes", "-max-ticks", 180, "-out", root / name / "trace.json"]
        run = make_run(options.repo, args, root / name, 150)
        runs[name] = run.start()
        return run

    try:
        host = start("Host", True,
            f"wait_error {reason}\nassert_substate Lobby\nassert_enabled ButtonMultiplayerStart 0\nassert_error {reason}\ndump_lobby\nscreenshot rejected-join\n"
            "wait_connected 2\nwait_remote_ready\nwait_all_ready\ndump_lobby\nactivate ButtonMultiplayerStart\nwait 99999\n", trace=True)
        wait_for_log(host, "activate ButtonMultiplayerCreate ok=1")
        # selected_module is hashed; -module Dummy.rte leaves the official set unchanged
        wrong = start("Rejected", False,
            f"wait_state Failed\nwait 5\nassert_substate Landing\nassert_error {reason}\ndump_lobby\nscreenshot rejected-client\nexit\n", extra=("-module", "Dummy.rte"))
        records["Rejected"] = wrong.finish()
        wait_for_log(host, '[menu-script] dump_lobby state=Starting')
        start("Replacement", False,
            "wait_connected 2\nactivate ButtonMultiplayerReady\nwait_all_ready\ndump_lobby\nwait 99999\n", trace=True)
        with ThreadPoolExecutor(max_workers=2) as pool:
            for name, record in pool.map(lambda name: (name, runs[name].finish()), ("Host", "Replacement")):
                records[name] = record
        checks, details = {}, {}
        for name, record in records.items():
            log = (root / name / "stdout.log").read_text(errors="replace")
            console = root / name / "runtime/LogConsole.txt"
            full_log = log + ("\n" + console.read_text(errors="replace") if console.exists() else "")
            errors = re.findall(r"^.*(?:FAILED:|FAIL:|ERROR:|EXCEPTION_|RTE Assert|RTE Abort|stack traceback).*$", full_log, re.M)
            checks[name + "_process"] = record["exit_code"] == 0 and not record["timed_out"]
            checks[name + "_no_errors"] = not errors
            checks[name + "_desktop"] = record["input_desktop_before"] == record["input_desktop_after"]
            checks[name + "_binary"] = record["exe_sha256"] == records["Host"]["exe_sha256"]
            details[name] = {"errors": errors, "binary": record["exe_sha256"], "exit": record["exit_code"]}
        host_log = (root / "Host/stdout.log").read_text(errors="replace")
        wrong_log = (root / "Rejected/stdout.log").read_text(errors="replace")
        checks["host_stayed_in_lobby"] = "assert_substate expected=Lobby actual=Lobby PASS" in host_log and "assert_enabled ButtonMultiplayerStart expected=0 actual=0 PASS" in host_log
        checks["host_reason_visible"] = f'assert_error "{reason}"' in host_log and 'A player could not join:' in host_log
        checks["client_reason_visible"] = f'assert_error "{reason}"' in wrong_log and 'assert_substate expected=Landing actual=Landing PASS' in wrong_log
        checks["rejected_peer_never_launched"] = 'dump_lobby state=Failed' in wrong_log and '[menu-mp]' not in wrong_log
        checks["replacement_launched"] = "Replacement(team" in host_log and "activate ButtonMultiplayerStart ok=1" in host_log
        checks["simulation"], details["simulation"] = strict_compare(root / "Host/trace.json", root / "Replacement/trace.json", 180)
        for name, image in (("Host", "rejected-join.png"), ("Rejected", "rejected-client.png")):
            images = list((root / name / "runtime/ScreenShots").glob(Path(image).stem + "_*.png"))
            checks[name + "_screenshot"] = len(images) == 1 and images[0].stat().st_size > 0
            details[name]["screenshot"] = str(images[0]) if len(images) == 1 else None
        result.update({"pass": all(checks.values()), "checks": checks, "details": details})
    except Exception as error:
        result["error"] = str(error)
    finally:
        for run in runs.values(): run.close()
        (root / "result.json").write_text(json.dumps(result, indent=2))
    print(json.dumps({"pass": result["pass"], "error": result.get("error"), "failed": [key for key, ok in result.get("checks", {}).items() if not ok], "out": str(root)}), flush=True)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
