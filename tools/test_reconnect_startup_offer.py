"""Assert the stored recovery record is offered on the way into the main menu, not on request.

A host and a client play a real match; the client is killed with its record on disk while the host
still hosts. A second client boots with that record and a script that navigates nowhere: the first
screen it shows must be the multiplayer landing panel carrying the rejoin offer. Dismissing the offer
must clear the line it wrote (G5), and a boot with no record must say so rather than show a blank
line (G4).
"""

import argparse
import json
from pathlib import Path
import time

from run_sim_test import make_run
from test_reconnect_menu_recovery import menu_script, wait_for_log


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=44290)
    parser.add_argument("--play-seconds", type=float, default=8.0)
    options = parser.parse_args()
    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    ticket = root / "Client.ticket"
    runs = {}
    result = {"pass": False, "checks": {}, "details": {}}

    def start(name, script, ticket_path):
        path = root / f"{name}.txt"
        path.write_text(script, encoding="utf-8")
        argv = ["-menu-script", path, "-num-lua-states", 4, "-net-reconnect-ticket", ticket_path]
        runs[name] = make_run(options.repo, argv, root / name, 300).start()
        return runs[name]

    try:
        host = start("Host", menu_script("Host", True, options.port,
                                         "wait_connected 2\nwait_remote_ready\nwait_all_ready\n"
                                         "activate ButtonMultiplayerStart\nwait 99999\n"),
                     root / "Host.ticket")
        wait_for_log(host, "activate ButtonMultiplayerCreate ok=1")
        client = start("Client", menu_script("Client", False, options.port, "wait_state Failed\nexit\n"), ticket)
        wait_for_log(host, "activate ButtonMultiplayerStart ok=1")
        wait_for_log(client, "[menu-mp] launching the match")
        time.sleep(options.play_seconds)
        result["checks"]["client_stored_a_ticket"] = ticket.exists()
        runs["Client"].terminate()

        # The relaunch: this script navigates nowhere, so whatever it finds is what the engine put up
        # by itself on the way into the main menu.
        start("Relaunch",
              "wait 60\nassert_screen MultiplayerScreen\nassert_substate Landing\n"
              "assert_error Rejoin your match at\nassert_enabled ButtonMultiplayerReconnect 1\n"
              "screenshot startup-offer\n"
              "activate ButtonMultiplayerCancelReconnect\nwait 6\nassert_landing_empty\nexit\n",
              ticket)
        record = runs["Relaunch"].finish()
        log = (root / "Relaunch/stdout.log").read_text(errors="replace")
        result["details"]["relaunch_exit"] = record["exit_code"]
        result["checks"]["relaunch_process"] = record["exit_code"] == 0 and not record["timed_out"]
        result["checks"]["offer_screen"] = "assert_screen expected=MultiplayerScreen actual=MultiplayerScreen PASS" in log
        result["checks"]["offer_panel"] = "assert_substate expected=Landing actual=Landing PASS" in log
        result["checks"]["offer_text"] = 'assert_error "Rejoin your match at"' in log and "FAIL" not in log
        result["checks"]["rejoin_enabled"] = "assert_enabled ButtonMultiplayerReconnect expected=1 actual=1 PASS" in log
        result["checks"]["dismiss_clears_the_line"] = 'assert_landing_empty status="" PASS' in log
        result["checks"]["no_script_failure"] = "[menu-script] FAILED:" not in log

        # A boot with no record at all: §11 says so instead of leaving the line blank.
        start("NoRecord",
              "wait 40\nactivate ButtonMainToMultiplayer\nwait 12\nassert_substate Landing\n"
              "assert_error No reconnect record for that match.\nexit\n",
              root / "absent.ticket")
        empty_record = runs["NoRecord"].finish()
        empty_log = (root / "NoRecord/stdout.log").read_text(errors="replace")
        result["checks"]["no_record_process"] = empty_record["exit_code"] == 0 and not empty_record["timed_out"]
        result["checks"]["no_record_text"] = ('assert_error "No reconnect record for that match."' in empty_log
                                              and "[menu-script] FAILED:" not in empty_log)
        result["pass"] = all(result["checks"].values())
    except Exception as error:
        result["error"] = str(error)
    finally:
        for run in runs.values():
            run.close()
        (root / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({"pass": result["pass"], "error": result.get("error"),
                      "failed": [k for k, v in result["checks"].items() if not v], "out": str(root)}), flush=True)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
