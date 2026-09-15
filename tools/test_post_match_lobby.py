"""Where two online peers land when their match ends: the rematch lobby, not the planet screen.

Two menu-driven peers host and join on loopback, ready up and play a match bounded by
-net-match-ticks, which ends it on the applied frame both peers reach through the ordinary
FinishMatch path. Everything after that is the real match-end routing:

  * lobby arm: each peer dumps its service state where the match end leaves it, then asserts
    MultiplayerScreen with the Lobby panel up and a roster row, and saves a screenshot at
    640x360. Without the routing the peer is left on the main menu (the script runs only while
    MainMenuActive), nothing pumps its service, and the assert reports the screen it was actually
    left on beside "state=Completed".
  * leave arm: both peers reach the rematch lobby, then one presses Back (post_command
    ButtonBackToMain, the real back path: destroy + directory DELETE). The peer that waited
    asserts the screen and the status line it is left on.

What this proves is the screen and panel the menu reports, the service state behind them and the
roster rows' text. Only a headed run shows the pixels; the screenshots are for that review.
"""

import argparse
import json
import re
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_sim_test import make_run, seed_settings  # noqa: E402

SCREEN = re.compile(r"^\[menu-script\] assert_screen expected=(\S+) actual=(\S+) (PASS|FAIL)$", re.M)
SUBSTATE = re.compile(r"^\[menu-script\] assert_substate expected=(\S+) actual=(\S+) (PASS|FAIL)$", re.M)
DUMP = re.compile(r"^\[menu-script\] dump_lobby state=(\S+) members=(\d+) error=\"([^\"]*)\" status=\"([^\"]*)\"", re.M)
LABEL = re.compile(r"^\[menu-script\] assert_label (\S+) \"([^\"]*)\" text=\"([^\"]*)\" (PASS|FAIL)$", re.M)
STATUS_LABEL = "LabelMultiplayerStatus"


def set_resolution(runtime: Path, x: int, y: int) -> None:
    """Retargets the prepared runtime's own Settings.ini - the repo copy stays untouched."""
    settings = runtime / "Userdata" / "Settings.ini"
    text = settings.read_text(encoding="utf-8")
    for name, value in (("ResolutionX", x), ("ResolutionY", y)):
        text, count = re.subn(rf"(?m)^(\s*{name}\s*=\s*)[^\r\n]*", rf"\g<1>{value}", text)
        if count == 0:
            text += f"\n\t{name} = {value}\n"
    settings.write_text(text, encoding="utf-8")


def head(name: str, host: bool, port: int) -> str:
    script = f"wait 40\nactivate ButtonMainToMultiplayer\nwait 12\nsettext TextMultiplayerName {name}\n"
    if host:
        return script + (f"activate ButtonMultiplayerHostGame\nwait 10\nsettext TextHostPort {port}\n"
                         "settext TextHostPlayers 2\n"
                         "activate ButtonMultiplayerCreate\nwait_connected 2\nwait_remote_ready\n"
                         "wait_all_ready\nactivate ButtonMultiplayerStart\n")
    return script + (f"activate ButtonMultiplayerJoinGame\nwait 10\nsettext TextJoinAddress 127.0.0.1\n"
                     f"settext TextJoinPort {port}\nactivate ButtonMultiplayerConnect\n"
                     "wait_connected 2\nactivate ButtonMultiplayerReady\nwait_remote_ready\n")


# The launch stops the menu loop, so a real-time wait longer than the fade-out cannot finish
# before the match: the step after it runs on the first menu frame the match end brings back.
# A lobby-state wait cannot do this - the pre-launch lobby is already in the state it would name.
def lobby_tail(name: str, settle_ms: int) -> str:
    """The match-end assertions: where the peer landed and what its panel shows."""
    return (f"wait_ms {settle_ms}\nwait 20\ndump_lobby\n"
            "assert_screen MultiplayerScreen\nassert_substate Lobby\n"
            "assert_control LabelLobbyPlayer0\n"
            # The line the panel draws has to be the lobby's own status, not a pre-match hint.
            f"assert_label {STATUS_LABEL} ready up for a rematch\n"
            f"dump_lobby\nscreenshot post_match_lobby_{name}\nwait 10\nexit\n")


def leave_tail(name: str, settle_ms: int, leaver: bool) -> str:
    if leaver:
        return (f"wait_ms {settle_ms}\nwait 20\ndump_lobby\nassert_screen MultiplayerScreen\n"
                "post_command ButtonBackToMain\nwait 20\nassert_screen MainScreen\n"
                f"dump_lobby\nscreenshot post_match_left_{name}\nwait 10\nexit\n")
    # The waiter is not told to leave: it stays where the end of the other peer's lobby leaves it.
    return (f"wait_ms {settle_ms}\nwait 20\ndump_lobby\nassert_screen MultiplayerScreen\n"
            "wait 240\nassert_screen MultiplayerScreen\ndump_lobby\n"
            f"assert_label {STATUS_LABEL} ready up for a rematch\n"
            f"screenshot post_match_waiter_{name}\nwait 10\nexit\n")


def status_label_vs_dump(log: str):
    """What the status label drew against the service status the peer dumped just before it.

    The dump and the label read the same frame's state, so a difference is what the panel wrote
    over the lobby's own line - the defect this arm is here for."""
    pairs, status = [], None
    for line in log.splitlines():
        dump = DUMP.match(line)
        if dump:
            status = dump.group(4)
            continue
        label = LABEL.match(line)
        if label and label.group(1) == STATUS_LABEL:
            pairs.append({"dump_status": status, "label_text": label.group(3), "verdict": label.group(4),
                          "equal": status is not None and label.group(3) == status})
    return pairs


def read_log(out: Path) -> str:
    text = ""
    for name in ("stdout.log", "stderr.log"):
        path = out / name
        if path.exists():
            text += path.read_text(encoding="utf-8", errors="replace")
    return text


def newest_shot(out: Path, stem: str) -> Path:
    shots = out / "runtime" / "ScreenShots"
    found = sorted(shots.glob(f"{stem}*.png"), key=lambda p: p.stat().st_mtime) if shots.exists() else []
    return found[-1] if found else shots / f"{stem}.png"


def run_arm(repo: Path, root: Path, port: int, ticks: int, timeout: int, settle_ms: int, arm: str) -> dict:
    root.mkdir(parents=True, exist_ok=True)
    scripts = {
        "Host": head("Host", True, port) + (lobby_tail("host", settle_ms) if arm == "lobby"
                                            else leave_tail("host", settle_ms, leaver=False)),
        "Guest": head("Guest", False, port) + (lobby_tail("guest", settle_ms) if arm == "lobby"
                                               else leave_tail("guest", settle_ms, leaver=True)),
    }
    runs, records = {}, {}
    for who, text in scripts.items():
        path = root / f"{who}.txt"
        path.write_text(text, encoding="utf-8")
        runs[who] = make_run(repo, ["-menu-script", path, "-num-lua-states", 4,
                                    "-net-match-ticks", ticks,
                                    "-net-match-report", root / f"{who}_report.json"],
                             root / who, timeout)
        set_resolution(runs[who].cwd, 640, 360)
        # The delay box is read-only under the auto policy; the floor the host sends is a setting.
        seed_settings(runs[who], {"NetworkInputDelayFrames": 3})

    def drive(who: str) -> None:
        try:
            records[who] = runs[who].start().finish()
        except Exception as exc:  # noqa: BLE001
            records[who] = {"error": repr(exc)}

    host = threading.Thread(target=drive, args=("Host",), daemon=True)
    host.start()
    # The joiner needs the host's listening socket; the menu path takes a moment to open it.
    time.sleep(2.0)
    guest = threading.Thread(target=drive, args=("Guest",), daemon=True)
    guest.start()
    host.join()
    guest.join()
    for run in runs.values():
        run.close()
    logs = {who: read_log(root / who) for who in scripts}
    detail = {"records": {who: {"exit_code": records.get(who, {}).get("exit_code"),
                                "timed_out": records.get(who, {}).get("timed_out"),
                                "error": records.get(who, {}).get("error")} for who in scripts},
              "screens": {who: SCREEN.findall(logs[who]) for who in scripts},
              "substates": {who: SUBSTATE.findall(logs[who]) for who in scripts},
              "dumps": {who: DUMP.findall(logs[who]) for who in scripts},
              "dump_lines": {who: [l for l in logs[who].splitlines() if "dump_lobby" in l] for who in scripts},
              "labels": {who: LABEL.findall(logs[who]) for who in scripts},
              "status_label": {who: status_label_vs_dump(logs[who]) for who in scripts},
              "failures": {who: [l for l in logs[who].splitlines() if "[menu-script] FAILED" in l]
                           for who in scripts},
              "launched": {who: "[menu-mp] launching the match" in logs[who] for who in scripts},
              # The round ended for this peer: its service reached Completed, or the rematch lobby
              # its end opened has already appended its own suffix to the match result.
              "match_complete": {who: any("state=Completed" in l or "- ready up for a rematch" in l
                                          for l in logs[who].splitlines() if "dump_lobby" in l)
                                 for who in scripts}}
    return {"detail": detail, "logs": logs, "root": root}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=48171)
    parser.add_argument("--ticks", type=int, default=240, help="applied frames the match runs before it ends")
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--wait-seconds", type=int, default=20,
                        help="real-time guard that keeps the post-match steps out of the launch fade-out")
    parser.add_argument("--arm", choices=["lobby", "leave", "both"], default="both")
    options = parser.parse_args()
    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    result = {"pass": False, "checks": {}, "details": {}}

    try:
        arms = ["lobby", "leave"] if options.arm == "both" else [options.arm]
        for index, arm in enumerate(arms):
            run = run_arm(options.repo, root / arm, options.port + index * 4, options.ticks,
                          options.timeout, options.wait_seconds * 1000, arm)
            detail = run["detail"]
            result["details"][arm] = detail
            for who in ("Host", "Guest"):
                result["checks"][f"{arm}_{who}_launched"] = detail["launched"][who]
                result["checks"][f"{arm}_{who}_match_ended"] = detail["match_complete"][who]
                result["checks"][f"{arm}_{who}_no_script_failure"] = not detail["failures"][who]
                result["checks"][f"{arm}_{who}_exit"] = detail["records"][who]["exit_code"] == 0
                # Every assert_screen the peer ran landed on the multiplayer screen, and it ran at
                # least the one the match end is about.
                screens = detail["screens"][who]
                result["checks"][f"{arm}_{who}_on_multiplayer_screen"] = bool(screens) and \
                    ("MultiplayerScreen", "MultiplayerScreen", "PASS") in screens
                # The status line the panel drew is the one the service published that frame. The
                # leave arm's guest walks out to the main screen, so only it has no line to assert.
                pairs = detail["status_label"][who]
                expects_label = not (arm == "leave" and who == "Guest")
                result["checks"][f"{arm}_{who}_status_label_is_the_lobby_line"] = \
                    bool(pairs) == expects_label and all(pair["equal"] for pair in pairs)
            if arm == "lobby":
                for who in ("Host", "Guest"):
                    result["checks"][f"lobby_{who}_lobby_panel"] = \
                        ("Lobby", "Lobby", "PASS") in detail["substates"][who]
                    shot = newest_shot(root / arm / who, f"post_match_lobby_{who.lower()}")
                    result["details"][f"lobby_{who}_screenshot"] = str(shot)
                    result["checks"][f"lobby_{who}_screenshot"] = shot.exists() and shot.stat().st_size > 0
                    # The rematch lobby shows the match's roster, not an empty panel.
                    result["checks"][f"lobby_{who}_roster"] = any(
                        "Host(" in line and "Guest(" in line for line in detail["dump_lines"][who])
            else:
                # The leaver's back press takes it to the main screen; the waiter stays on the
                # multiplayer screen with the line the ended lobby left it.
                result["checks"]["leave_leaver_left"] = ("MainScreen", "MainScreen", "PASS") in \
                    detail["screens"]["Guest"]
                waiter = detail["dumps"]["Host"]
                result["details"]["leave_waiter_status"] = waiter
                result["checks"]["leave_waiter_has_a_line"] = bool(waiter) and any(
                    error or status for _, _, error, status in waiter)
                shot = newest_shot(root / arm / "Host", "post_match_waiter_host")
                result["details"]["leave_waiter_screenshot"] = str(shot)
                result["checks"]["leave_waiter_screenshot"] = shot.exists() and shot.stat().st_size > 0
        result["pass"] = all(result["checks"].values())
    except Exception as error:  # noqa: BLE001
        result["error"] = str(error)
    (root / "result.json").write_text(json.dumps(result, indent=2, default=str), encoding="utf-8")
    print(json.dumps({"pass": result["pass"], "error": result.get("error"),
                      "failed": [k for k, v in result["checks"].items() if not v], "out": str(root)}), flush=True)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
