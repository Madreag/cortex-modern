"""Assert a player can become an applicant from the product, not only from a command-line switch.

Three service peers play a match and one of them is killed, so the host holds its seat. A fourth
process then joins through the real menu: the host refuses it because the match is running, the
landing panel offers to apply for a held seat, and the script presses that button. The host must end
with the applicant on its §9b moderation view, and the applicant with an application sent.
"""

import argparse
import json
from pathlib import Path
import time

from run_sim_test import make_run


def peer_args(port, ticket, report, extra, players, ticks):
    return ["-net-match-service-e2e", "-net-port", port, "-net-match-peers", players,
            "-net-match-ticks", ticks, "-net-reconnect-ticket", ticket,
            "-net-match-report", report, *extra]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=44294)
    options = parser.parse_args()
    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    players, ticks = 3, 2400
    trigger = root / "apply.trigger"
    runs = {}
    result = {"pass": False, "checks": {}, "details": {}}

    def start(name, argv, timeout=420, env=None):
        runs[name] = make_run(options.repo, argv, root / name, timeout, env=env).start()
        return runs[name]

    def wait_for(run, marker, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            log = run.out / "stdout.log"
            if log.exists() and marker in log.read_text(errors="replace"):
                return True
            if run.poll() is not None:
                raise RuntimeError(f"{run.out.name} ended before {marker}")
            time.sleep(0.1)
        raise RuntimeError(f"{run.out.name} did not reach {marker}")

    try:
        host = start("host", peer_args(options.port, root / "host.ticket", root / "host_report.json",
                                       ["-net-host", "-net-match-e2e-resync"], players, ticks))
        start("departing", peer_args(options.port, root / "departing.ticket", root / "departing_report.json",
                                     ["-net-join", "127.0.0.1", "-net-match-e2e-resync"], players, ticks))
        start("stayer", peer_args(options.port, root / "stayer.ticket", root / "stayer_report.json",
                                  ["-net-join", "127.0.0.1", "-net-match-e2e-resync"], players, ticks))
        wait_for(host, "lobby_snapshot: state=Running", 120)

        # The applicant boots now but its join is held until the host has adjudicated the drop, so the
        # refusal it answers is the refusal a live match with a held seat gives.
        script = root / "applicant.txt"
        script.write_text(
            "wait 40\nactivate ButtonMainToMultiplayer\nwait 12\nsettext TextMultiplayerName Applicant\n"
            "activate ButtonMultiplayerJoinGame\nwait 10\nsettext TextJoinAddress 127.0.0.1\n"
            f"settext TextJoinPort {options.port}\nactivate ButtonMultiplayerConnect\n"
            "wait_state Failed\nwait 5\nassert_substate Landing\n"
            "assert_error The match is already in progress\n"
            "assert_enabled ButtonMultiplayerReconnect 1\n"
            "activate ButtonMultiplayerReconnect\nwait_ms 12000\ndump_lobby\nexit\n", encoding="utf-8")
        applicant = start("applicant", ["-menu-script", script, "-num-lua-states", 4,
                                        "-net-reconnect-ticket", root / "applicant.ticket",
                                        "-net-match-report", root / "applicant_report.json",
                                        "-net-join-wait-for", trigger])
        time.sleep(4)
        runs["departing"].terminate()
        wait_for(host, "left the match at frame", 180)
        trigger.write_text("{}", encoding="utf-8")
        result["checks"]["host_registered_an_applicant"] = wait_for(host, "[net-reconnect] applicant", 90)
        for name in ("host", "stayer", "applicant"):
            result["details"][name] = {"exit": runs[name].finish()["exit_code"]}
        applicant_log = (root / "applicant/stdout.log").read_text(errors="replace")
        host_report = json.loads((root / "host_report.json").read_text(errors="replace"))
        applicant_report = json.loads((root / "applicant_report.json").read_text(errors="replace"))
        service = host_report.get("service", host_report)
        reconnect = service.get("reconnect", {})
        moderation = reconnect.get("moderation", [])
        names = [entry.get("display_name") for seat in moderation for entry in seat.get("applicants", [])]
        result["details"]["moderation_applicants"] = names
        applicant_reconnect = applicant_report.get("reconnect", {})
        result["details"]["applications_sent"] = applicant_reconnect.get("client_applications_sent")
        result["checks"]["offer_shown"] = 'assert_error "The match is already in progress"' in applicant_log
        result["checks"]["apply_button_enabled"] = "assert_enabled ButtonMultiplayerReconnect expected=1 actual=1 PASS" in applicant_log
        result["checks"]["apply_pressed"] = "activate ButtonMultiplayerReconnect ok=1" in applicant_log
        result["checks"]["no_script_failure"] = "[menu-script] FAILED:" not in applicant_log
        result["checks"]["applications_sent"] = (applicant_reconnect.get("client_applications_sent") or 0) >= 1
        result["checks"]["applicant_on_the_moderation_view"] = "Applicant" in names
        result["checks"]["host_exit"] = result["details"]["host"]["exit"] == 0
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
