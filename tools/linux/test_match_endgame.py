"""Focus, comma locale, chat and a graceful host departure.

Every launch goes through run_sim_test's platform runner. Each case retains its menu/probe
scripts, process records, complete tick hashes and match reports. No desktop automation.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from compare_sim_traces import strict_compare, load_trace
from feel.retained_resume import PER_PEER_SUBSYSTEMS
from run_sim_test import make_run, seed_settings
from test_lobby_lifecycle import menu_script, wait_for_log

TICKS = 900


def menu(command):
    return {"op": "menu", "command": command}


def wait(tick):
    # sim_frame also counts the lobby; only the committed round frame proves gameplay has begun.
    return {"op": "wait", "service": "Running", "lockstep_frame_at_least": tick}


def probe_steps(case, who):
    steps = [wait(100)]
    if case == "focus" and who == "host":
        steps += [menu("game_key D down"), {"op": "wait", "renders": 4},
                  menu("assert_game_input 0 L_RIGHT 1"),
                  menu("window_event focus_lost"), {"op": "wait", "renders": 4},
                  menu("assert_window_focus 0"), wait(220), menu("assert_game_input 0 L_RIGHT 1"),
                  menu("window_event minimized"), wait(340), menu("assert_window_focus 0"),
                  menu("window_event restored"), menu("window_event focus_gained"),
                  {"op": "wait", "renders": 4}, menu("assert_window_focus 1"),
                  menu("window_event mouse_enter"), wait(450), menu("assert_game_input 0 L_RIGHT 1"), menu("game_key D up")]
    elif case == "chat" and who == "host":
        steps += [menu("game_key W down"), {"op": "wait", "renders": 4},
                  menu("assert_game_input 0 L_UP 1"), menu("game_key W up"),
                  {"op": "key_down", "key": "CHAT"}, {"op": "key_up", "key": "CHAT"},
                  {"op": "wait", "chat_entry_open": True, "renders": 4},
                  menu("game_key W down"), {"op": "wait", "renders": 4},
                  menu("assert_game_input 0 L_UP 0"), wait(300),
                  menu("assert_game_input 0 L_UP 0"), menu("game_key W up"),
                  {"op": "send_chat", "text": "row514 chat during play", "scope": "all"},
                  {"op": "key_down", "key": "Escape"}, {"op": "key_up", "key": "Escape"},
                  {"op": "wait", "chat_entry_open": False, "renders": 4}]
    elif case == "chat":
        steps += [{"op": "wait", "control": "LabelMatchChatNewest", "text_contains": "row514 chat during play"}]
    elif case == "graceful" and who == "host":
        steps += [wait(300), menu("open_local_pause"),
                  {"op": "wait", "screen": "Pause", "scope": "menu"},
                  menu("activate ButtonLeaveMatch"),
                  {"op": "wait", "screen": "PauseLeaveConfirm", "scope": "menu"},
                  menu("activate ButtonLeaveConfirm"),
                  {"op": "wait", "service": "Completed", "scope": "menu"},
                  {"op": "signal", "name": "leave-complete", "scope": "menu"}, {"op": "finish"}]
        return steps
    elif case == "graceful":
        steps += [{"op": "wait", "control": "LabelNetMatchHandoverToast", "text_contains": "is now hosting"},
                  {"op": "assert_control", "control": "LabelNetMatchHandoverToast", "text_contains": "ClientA"}]
    return steps + [wait(850), {"op": "finish"}]


def run_case(repo, root, case, port):
    root.mkdir(parents=True, exist_ok=False)
    names = ("host", "clienta", "clientb") if case == "graceful" else ("host", "clienta")
    runs, records, probes = {}, {}, {}
    result = {"pass": False, "case": case, "ticks": TICKS, "checks": {}}
    inputs = root / "input.txt"
    inputs.write_text("".join(f"player={seat} 1 899 AIM=0.8,0.6\nplayer={seat} 100 500 L_RIGHT\n"
                              for seat in range(len(names))), encoding="utf-8")
    try:
        for who in names:
            host = who == "host"
            name = "Host" if host else "ClientA" if who == "clienta" else "ClientB"
            script = menu_script(name, host, len(names), port)
            if host:
                script = script.replace("activate ButtonMultiplayerCreate", "combo_select ComboHostActivity P4 Alpha Duel - Base.rte\nactivate ButtonMultiplayerCreate")
                script += f"wait_connected {len(names)} 90\nwait_remote_ready 90\nwait_all_ready 90\nactivate ButtonMultiplayerStart\n"
            script += (f"wait_file {root / 'host-probe/leave-complete.json'} 180\nwait_ms 200\nexit\n"
                       if case == "graceful" and host else "wait_ms 180000\nexit\n")
            path = root / f"{who}.menu.txt"
            path.write_text(script, encoding="utf-8")
            probe = root / f"{who}-probe" / "script.json"
            probe.parent.mkdir()
            probe.write_text(json.dumps({"schema": 1, "timeout_ms": 180000, "steps": probe_steps(case, who)}, indent=2) + "\n")
            args = ["-menu-script", path, "-seed", "42", "-net-match-report", root / f"{who}-match.json"]
            # The host's completed leave and clean exit bound its deliberately shorter run.
            if not (case == "graceful" and host):
                args += ["-max-ticks", TICKS, "-tick-hashes", "-out", root / f"{who}-trace.json"]
            if case not in ("chat", "focus"):
                args += ["-input-script", inputs]
            env = {"CCCP_HEADLESS": "1", "CC_TEST_NET_UI_SCRIPT": str(probe)}
            if case == "locale":
                env.update(LANG="C" if host else "de_DE.UTF-8", LC_ALL="C" if host else "de_DE.UTF-8")
                env["CC_TEST_PROCESS_LOCALE"] = "C" if host else "German_Germany.1252" if sys.platform == "win32" else "de_DE.UTF-8"
            run = make_run(repo, args, root / who, 210, env=env)
            seed_settings(run, {"NetworkDisplayName": name, "NetworkInputDelayFrames": 3, "NetworkIceEnable": 0,
                                "NetworkChatVisible": 1, "NetworkSlowPlayerPolicy": "Substitute"})
            runs[who] = run.start()
            if host:
                wait_for_log(run, "activate ButtonMultiplayerCreate ok=1", 90)
            elif who == "clienta" and len(names) == 3:
                wait_for_log(run, "activate ButtonMultiplayerConnect ok=1", 90)
        with ThreadPoolExecutor(max_workers=len(names)) as pool:
            futures = {who: pool.submit(run.finish) for who, run in runs.items()}
            records = {who: future.result() for who, future in futures.items()}
        for who in names:
            path = root / f"{who}-probe/net-ui-result.json"
            probes[who] = json.loads(path.read_text()) if path.exists() else {"pass": False, "error": "probe result missing"}
            result["checks"][f"{who}_probe"] = probes[who].get("pass", False)
            result["checks"][f"{who}_exit"] = records[who].get("exit_code") == 0 and not records[who].get("timed_out")
            if case == "locale":
                log = (root / who / "stdout.log").read_text(errors="replace")
                result["checks"][f"{who}_locale_active"] = "decimal_probe=" + ("1.5" if who == "host" else "1,5") in log
        pairs = [("clienta", "clientb")] if case == "graceful" else [("host", "clienta")]
        result["comparisons"] = {}
        for left, right in pairs:
            ok, detail = strict_compare(root / f"{left}-trace.json", root / f"{right}-trace.json",
                                        TICKS, per_peer=PER_PEER_SUBSYSTEMS)
            result["comparisons"][f"{left}-{right}"] = detail
            result["checks"]["equal_complete_hashes"] = ok
            if case != "graceful":
                result["checks"]["no_paused_ticks"] = detail["paused_ticks"] == 0
        if case == "focus":
            result["holds"] = {}
            for who in names:
                report = json.loads((root / f"{who}-match.json").read_text())
                lockstep = report.get("runner", {}).get("lockstep", {})
                result["holds"][who] = {key: value for key, value in lockstep.items() if "hold" in key or "pause" in key}
                peer_holds = {peer: counters["holds"] for peer, counters in lockstep["peers"].items()}
                result["holds"][who]["per_peer_holds"] = peer_holds
                result["checks"][f"{who}_zero_hold_counters"] = (len(peer_holds) == len(names)
                    and all(count == 0 for count in peer_holds.values()) and lockstep["ai_held_peer_ids"] == []
                    and lockstep["relay_congestion_holds"] == 0)
                # A committed seat hold is also printed at its boundary, including a zero-length recovery.
                log = (root / who / "stdout.log").read_text(errors="replace")
                result["checks"][f"{who}_no_seat_hold"] = "[net-match] held client:" not in log
        result["pass"] = all(result["checks"].values())
    except Exception as error:
        result["error"] = repr(error)
    finally:
        for run in runs.values():
            run.close()
    result.update(records=records, probes=probes)
    (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"case": case, "pass": result["pass"], "error": result.get("error"),
                      "failed": [name for name, ok in result["checks"].items() if not ok]}), flush=True)
    return result["pass"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--case", choices=("focus", "locale", "chat", "graceful"), required=True)
    parser.add_argument("--port", type=int, default=50302)
    args = parser.parse_args()
    return 0 if run_case(args.repo.resolve(), args.out.resolve(), args.case, args.port) else 1


if __name__ == "__main__":
    raise SystemExit(main())
