"""Assert §11's retry schedule runs from the menu loop, on whatever screen the drop left up.

Two menu-driven peers play a real match. Once the client has launched it the driver damages the
client's recovery record and kills the host, so the client's link dies mid-match and every rejoin
attempt fails on the spot instead of parking on a dead address for the lobby wait. The client's
script never opens the multiplayer screen again: it watches the attempt counter climb from the main
screen it was left on. Before the fix the counter never moves there, because NetMatchService::Update()
had one caller, MainMenuGUI::UpdateMultiplayerScreen().
"""

import argparse
import json
from pathlib import Path
import re
import time

from run_sim_test import make_run


def menu_script(name, host, port, tail):
    script = f"wait 40\nactivate ButtonMainToMultiplayer\nwait 12\nsettext TextMultiplayerName {name}\n"
    if host:
        script += (f"activate ButtonMultiplayerHostGame\nwait 10\nsettext TextHostPort {port}\n"
                   "settext TextHostPlayers 2\nsettext TextHostInputDelay 3\nactivate ButtonMultiplayerCreate\n")
    else:
        script += (f"activate ButtonMultiplayerJoinGame\nwait 10\nsettext TextJoinAddress 127.0.0.1\n"
                   f"settext TextJoinPort {port}\nactivate ButtonMultiplayerConnect\n"
                   "wait_connected 2\nactivate ButtonMultiplayerReady\nwait_remote_ready\ndump_lobby\n")
    return script + tail


def wait_for_log(run, marker, seconds=120):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        log = run.out / "stdout.log"
        if log.exists():
            text = log.read_text(errors="replace")
            if marker in text:
                return text
        if run.poll() is not None:
            raise RuntimeError(f"{run.out.name} ended before {marker}")
        time.sleep(0.1)
    raise RuntimeError(f"{run.out.name} did not reach {marker}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=44288)
    parser.add_argument("--play-seconds", type=float, default=8.0)
    parser.add_argument("--console-check", action="store_true",
                        help="also assert the drop left the console closed (G2)")
    options = parser.parse_args()
    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    ticket = root / "Client.ticket"
    runs = {}
    result = {"pass": False, "checks": {}, "details": {}}

    def start(name, host, tail):
        path = root / f"{name}.txt"
        path.write_text(menu_script(name, host, options.port, tail), encoding="utf-8")
        argv = ["-menu-script", path, "-num-lua-states", 4, "-net-reconnect-ticket", root / f"{name}.ticket"]
        runs[name] = make_run(options.repo, argv, root / name, 420).start()
        return runs[name]

    try:
        # The client's steps after "wait_state Failed" run only once the match has ended and the menu
        # loop is back, so the whole recovery half of the script happens on the main screen.
        tail = "wait_state Failed\ndump_reconnect\ngoto_main\nassert_screen MainScreen\n" \
               "wait_attempts 1 40\ndump_reconnect\nassert_screen MainScreen\n" \
               "wait_attempts 3 60\ndump_reconnect\nassert_screen MainScreen\n"
        if options.console_check:
            tail += "assert_console 0\n"
        tail += "exit\n"
        host = start("Host", True, "wait_connected 2\nwait_remote_ready\nwait_all_ready\n"
                                   "activate ButtonMultiplayerStart\nwait 99999\n")
        wait_for_log(host, "activate ButtonMultiplayerCreate ok=1")
        client = start("Client", False, tail)
        wait_for_log(host, "activate ButtonMultiplayerStart ok=1")
        wait_for_log(client, "[menu-mp] launching the match")
        time.sleep(options.play_seconds)
        result["checks"]["client_stored_a_ticket"] = ticket.exists()
        if ticket.exists():
            # Damage the record: every automatic attempt then fails at once, so the schedule is
            # observable inside the host's own 20s hold instead of parking on a dead address.
            data = bytearray(ticket.read_bytes())
            for index in range(min(16, len(data))):
                data[index] ^= 0xFF
            ticket.write_bytes(bytes(data))
        result["checks"]["client_still_in_the_match"] = client.poll() is None
        runs["Host"].terminate()
        record = runs["Client"].finish()
        result["details"]["client_exit"] = record["exit_code"]
        log = (root / "Client/stdout.log").read_text(errors="replace")
        dumps = re.findall(r"\[menu-script\] dump_reconnect screen=(\S+) state=(\S+) attempts=(\d+)", log)
        result["details"]["dumps"] = dumps
        result["checks"]["client_process"] = record["exit_code"] == 0 and not record["timed_out"]
        # The first dump is taken wherever the drop left the player; everything after it is the
        # main screen, which is the point: the schedule advances with that screen closed.
        result["checks"]["never_left_the_main_screen"] = len(dumps) >= 2 and all(screen == "MainScreen" for screen, _, _ in dumps[1:])
        result["checks"]["no_multiplayer_update"] = "assert_screen expected=MainScreen actual=MultiplayerScreen" not in log
        result["checks"]["attempts_advanced"] = bool(dumps) and int(dumps[-1][2]) >= 3
        result["checks"]["schedule_was_running"] = any(state in ("Waiting", "Retrying") for _, state, _ in dumps)
        result["checks"]["no_script_failure"] = "[menu-script] FAILED:" not in log
        if options.console_check:
            result["checks"]["console_stayed_closed"] = "assert_console expected=0 actual=0 PASS" in log
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
