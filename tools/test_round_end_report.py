"""Detect a completed round being replaced by its next lobby in the report.

Two local engines use the real pause menus and a private directory. This row
checks report retention; it does not establish a relay result.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import subprocess

from e2e.directory import serve
from run_sim_test import engine_executable, make_run, seed_settings
from test_lobby_lifecycle import wait_for_log


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def menus(root, port):
    common = "in_play\nwait_ms 1500\nactivate ButtonMainToMultiplayer\nwait_ms 400\n"
    host = common + (
        "settext TextMultiplayerName Host\nactivate ButtonMultiplayerHostGame\nwait_ms 400\n"
        "combo_select ComboHostActivity P4 Alpha Duel\nwait_ms 400\n"
        f"setup_host_port {port}\ncombo_select ComboHostPlayers 2\n"
        "activate ButtonMultiplayerCreate\nwait_connected 2 60\nwait_remote_ready 60\n"
        "activate ButtonMultiplayerStart\nwait_lockstep 180 60\nwait_connected 1 10\n"
        "open_local_pause\nactivate ButtonEndMatch\nactivate ButtonLeaveConfirm\n"
        "wait_round_end 10\nwait_ms 2000\nassert_substate Landing\n"
        "assert_label LabelMultiplayerLandingStatus Match ended by host\ndump_lobby\nexit\n")
    guest = common + (
        "settext TextMultiplayerName Guest\nactivate ButtonMultiplayerJoinGame\nwait_ms 400\n"
        "activate ButtonJoinByAddress\nwait 4\nsettext TextJoinAddress 127.0.0.1\n"
        f"settext TextJoinPort {port}\nactivate ButtonJoinAddressGo\nwait_connected 2 60\n"
        "activate ButtonMultiplayerReady\nwait_lockstep 180 60\nopen_local_pause\n"
        "activate ButtonLeaveMatch\nactivate ButtonLeaveConfirm\nwait_ms 1500\nexit\n")
    paths = {}
    for role, text in (("host", host), ("guest", guest)):
        paths[role] = root / (role + ".menu.txt")
        paths[role].write_text(text, encoding="utf-8")
    return paths


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=41831)
    args = parser.parse_args()
    repo, root = args.repo.resolve(), args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    tip = subprocess.check_output(["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip()
    if subprocess.check_output(["git", "-C", str(repo), "status", "--porcelain"], text=True).strip():
        raise RuntimeError("Commit the source before measuring the report")
    paths = menus(root, args.port)
    runs, records, checks, details = {}, {}, {}, {}
    result = {"source_tip": tip, "binary_sha256": digest(engine_executable(repo)),
              "driver_sha256": digest(__file__), "topology": "single-box", "relay_proof": False}
    try:
        with serve(root / "directory", args.port + 1, block=(args.port, args.port + 1)) as directory:
            for role in ("host", "guest"):
                out = root / role
                run = make_run(repo, ["-menu-script", paths[role], "-num-lua-states", "4",
                                     "-net-match-ticks", "0", "-net-match-report", out / "match-report.json",
                                     "-net-live-tick-hashes", out / "live.jsonl"], out, timeout=120)
                seed_settings(run, {"SessionDirectoryUrl": "https://" + directory["DIRECTORY_URL"],
                                    "SessionDirectoryCertSha256": directory["DIRECTORY_PIN"],
                                    "NetworkHostVisibility": 1, "NetworkIceEnable": 0,
                                    "NetworkConnectionMode": 0, "NetworkHostRelayMode": 0,
                                    "NetworkInputDelayFrames": 3})
                runs[role] = run.start()
                if role == "host":
                    wait_for_log(run, "activate ButtonMultiplayerCreate click ButtonMultiplayerCreate PASS", 45)
            with ThreadPoolExecutor(max_workers=2) as pool:
                finished = {role: pool.submit(run.finish) for role, run in runs.items()}
                records = {role: future.result() for role, future in finished.items()}
        for role, record in records.items():
            checks[role + "_exit"] = record["exit_code"] == 0 and not record.get("timed_out")
        report = json.loads((root / "host/match-report.json").read_text())
        text = (root / "host/stdout.log").read_text(errors="replace")
        locked = report.get("runner", {}).get("lockstep", {})
        directory = report.get("runner", {}).get("directory_at_end", {})
        ended = report.get("last_match", {})
        notes = re.findall(r"\[net-peer-departure\] peer=(\d+) frame=(\d+).*source=round-notice", text)
        checks.update(
            real_host_end=all(any(line.startswith("[menu-script] " + step) and line.endswith("PASS")
                                 for line in text.splitlines()) for step in
                              ("open_local_pause", "activate ButtonEndMatch", "activate ButtonLeaveConfirm")),
            completed_round=ended.get("result") == "Match ended by host" and ended.get("running_ticks", 0) >= 180,
            completed_tick=locked.get("completed_simulation_tick") == ended.get("running_ticks") and
                           isinstance(locked.get("completed_simulation_tick"), int) and locked["completed_simulation_tick"] >= 180,
            next_frame=locked.get("next_frame", 0) > 180,
            departed_peer=len(notes) == 1 and int(notes[0][0]) == 2 and
                          locked.get("peer_round_ends", {}).get("2", {}).get("frame") == int(notes[0][1]),
            directory_at_round_end=directory.get("confirmed_listed") is True and bool(directory.get("session_id")),
            truthful_host_state=report.get("state") == "Completed" and report.get("status") == "Match ended by host" and
                                not report.get("error") and "Rematch setup failed" not in text,
            truthful_host_label=bool(re.search(r'\[menu-script\] assert_label LabelMultiplayerLandingStatus .*text="Match ended by host" PASS', text)),
            round_delay=1 <= locked.get("input_delay_frames", 0) <= 36)
        details.update(lockstep=locked, directory_at_end=directory, last_match=ended,
                       state=report.get("state"), status=report.get("status"), departures=notes)
    except Exception as error:
        checks["completed"] = False
        details["error"] = str(error)
    finally:
        for run in runs.values():
            run.close()
    result.update(pass_=bool(checks) and all(checks.values()), checks=checks, details=details, records=records)
    (root / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"pass": result["pass_"], "checks": checks, "out": str(root)}), flush=True)
    return 0 if result["pass_"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
