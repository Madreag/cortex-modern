"""A running match stays in the session directory for its whole life and leaves it at its end.

Three service peers meet through a loopback session directory and play; one is killed, so the host
holds its seat. While the match runs the host's row must be listed with the state "running". A fourth
process then reaches the match through the directory alone (the Join page's 'session:<id>' address):
the host refuses it because the match is running, the landing panel offers to apply for the held
seat, and the script presses that button; the host must register the applicant. When the match ends
the row must be gone from the listing.

  python tools/test_directory_running_listing.py --out D:/mx/<lane>/running-listing
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
from run_sim_test import make_run  # noqa: E402
import test_directory_ice_join as directory  # noqa: E402
from edith_cross import make_cert  # noqa: E402
from test_menu_readback import spread, managed_case


@managed_case
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=REPO)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=46000, help="the directory's loopback port")
    parser.add_argument("--game-port", type=int, default=46001)
    if spread:
        spread.add_arguments(parser)
    options = parser.parse_args()
    if not spread:
        parser.error("running listing requires the shared spread executor")
    options.spread = True
    spread.configure(options)
    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    players, ticks = 3, 3600
    trigger = root / "apply.trigger"
    runs = {}
    execution = None
    result = {"pass": False, "checks": {}, "details": {}}
    cert, key, pin = make_cert(root)
    settings = {"SessionDirectoryUrl": f"127.0.0.1:{options.port}", "SessionDirectoryCertSha256": pin,
                "SessionDirectoryInstallKey": "running-listing-install", "NetworkIceEnable": "1", "NetworkStunServers": ""}
    service = directory.start_service(root, options.port, cert, key)

    def start(name, argv, timeout=420, env=None):
        run = execution.make_run(options.repo, argv, root / name, timeout, env=env)
        directory.patch_settings(Path(run.cwd), settings)
        runs[name] = run.start()
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

    def row_of(session_id):
        return next((row for row in directory.list_sessions(options.port) if row.get("session_id") == session_id), None)

    def row_when(session_id, ready, seconds=15):
        # The host's state reaches the listing on its next heartbeat (5 s on this directory).
        deadline = time.monotonic() + seconds
        row = row_of(session_id)
        while not (row and ready(row)) and time.monotonic() < deadline:
            time.sleep(0.5)
            row = row_of(session_id)
        return row

    def peer(name, role):
        return ["-net-match-service-e2e", "-net-port", options.game_port, "-net-match-peers", players,
                "-net-match-ticks", ticks, "-net-reconnect-ticket", root / f"{name}.ticket",
                "-net-match-report", root / f"{name}_report.json", "-net-ice", "on", "-net-match-e2e-resync", *role]

    try:
        execution = spread.prepare_case(options.repo, root,
            [spread.Peer(name, os="windows" if name == "applicant" else "any", reviewed=name == "applicant")
             for name in ("host", "departing", "stayer", "applicant")],
            spread.Match(options.game_port, options.port, parameters={"lane": "menus",
                "directory": {"DIRECTORY_URL": f"127.0.0.1:{options.port}", "DIRECTORY_PIN": pin, "DIRECTORY_ROOT": root},
                "join_by_session": False}))
        host = start("host", peer("host", ["-net-host"]))
        session_id = ""
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline and not session_id and host.poll() is None:
            rows = directory.list_sessions(options.port)
            session_id = rows[0]["session_id"] if rows else ""
            time.sleep(0.5)
        result["details"]["session_id"] = session_id
        if not session_id:
            raise RuntimeError("the host registered no row")
        start("departing", peer("departing", ["-net-join-session", session_id]))
        start("stayer", peer("stayer", ["-net-join-session", session_id]))
        wait_for(host, "lobby_snapshot: state=Running", 120)
        running = row_when(session_id, lambda row: row.get("state") == "running")
        result["details"]["row_running"] = running
        result["checks"]["listed_as_running"] = bool(running) and running.get("state") == "running"

        # The applicant knows only the session id; its join is held until the host has held the dropped seat.
        script = root / "applicant.txt"
        script.write_text(
            "wait 40\nactivate ButtonMainToMultiplayer\nwait 12\nsettext TextMultiplayerName Applicant\n"
            f"activate ButtonMultiplayerJoinGame\nwait 10\nactivate ButtonJoinByAddress\nwait 4\nsettext TextJoinAddress session:{session_id}\n"
            "activate ButtonJoinAddressGo\n"
            "wait_state Failed 240\nwait 5\nassert_substate Landing\n"
            "assert_error The match is already in progress\n"
            "assert_enabled ButtonMultiplayerReconnect 1\n"
            "activate ButtonMultiplayerReconnect\nwait_ms 12000\ndump_lobby\nexit\n", encoding="utf-8")
        start("applicant", ["-menu-script", script, "-num-lua-states", 4,
                            "-net-reconnect-ticket", root / "applicant.ticket",
                            "-net-match-report", root / "applicant_report.json",
                            "-net-join-wait-for", trigger])
        time.sleep(4)
        runs["departing"].terminate()
        wait_for(host, "left the match at frame", 180)
        held = row_when(session_id, lambda row: row.get("seats_held", 0) >= 1)
        result["details"]["row_while_held"] = held
        result["checks"]["listed_as_running_while_a_seat_is_held"] = bool(held) and held.get("state") == "running"
        result["checks"]["listed_with_the_held_seat"] = bool(held) and held.get("seats_held") == 1
        trigger.write_text("{}", encoding="utf-8")
        result["checks"]["host_registered_the_applicant"] = wait_for(host, "[net-reconnect] applicant", 90)
        for name in ("host", "stayer", "applicant"):
            result["details"][name] = {"exit": runs[name].finish()["exit_code"]}
        gone_deadline = time.monotonic() + 20
        while row_of(session_id) and time.monotonic() < gone_deadline:
            time.sleep(0.5)
        result["details"]["row_after_the_match"] = row_of(session_id)
        result["checks"]["gone_at_the_match_end"] = result["details"]["row_after_the_match"] is None
        host_log = (root / "host/stdout.log").read_text(errors="replace")
        result["checks"]["host_deleted_its_row"] = "[net-directory] state: registered -> deleting" in host_log
        applicant_log = (root / "applicant/stdout.log").read_text(errors="replace")
        result["checks"]["refused_as_running"] = 'assert_error "The match is already in progress"' in applicant_log
        result["checks"]["apply_pressed"] = "activate ButtonMultiplayerReconnect click ButtonMultiplayerReconnect PASS" in applicant_log
        result["checks"]["no_script_failure"] = "[menu-script] FAILED:" not in applicant_log
        applicant_report = json.loads((root / "applicant_report.json").read_text(errors="replace"))
        result["details"]["applications_sent"] = applicant_report.get("reconnect", {}).get("client_applications_sent")
        result["checks"]["application_sent"] = (result["details"]["applications_sent"] or 0) >= 1
        result["checks"]["host_exit"] = result["details"]["host"]["exit"] == 0
        result["pass"] = all(result["checks"].values())
    except Exception as error:
        result["error"] = f"{type(error).__name__}: {error}"
    finally:
        for run in runs.values():
            run.close()
        service.terminate()
        try:
            service.wait(timeout=10)
        except subprocess.TimeoutExpired:
            service.kill()
        receipt = execution.result() if execution else spread.read_json(root / "spread-result.json", {})
        result.update(topology="spread", peer_boxes=receipt.get("peer_boxes", {}), spread=receipt, proof=result["pass"])
        (root / "result.json").write_text(json.dumps(result, indent=2, default=str), encoding="utf-8")
    print(json.dumps({"pass": result["pass"], "error": result.get("error"),
                      "failed": [k for k, v in result["checks"].items() if not v], "out": str(root)}), flush=True)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
