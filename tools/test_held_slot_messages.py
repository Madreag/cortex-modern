"""Assert what a joiner is told, and can do, when the seat it wants is held.

returner: three service peers play a match and one is killed, so the host holds its seat. The same
player comes back through the real menu with its identity but without a ticket the host can match:
its landing must read 'Your slot is held for you. Apply to rejoin, the host decides', the button must
offer 'Apply to Rejoin', and pressing it must put the request beside that seat on the host's panel.

world: a dedicated persistent world and its player are killed and the world restarts from disk with that
player's seat held for it, so every seat is held. A newcomer joins through the menu: its landing must read 'All slots are held for returning players.
Apply for a slot or wait' with 'Apply for a Slot' and 'Wait for a Slot'; Wait must count down the seconds
it has left with Cancel beside it, and a join made again must get the same offer, whose Apply reaches the host.
"""

import argparse
import json
from pathlib import Path
import re
import shutil
import time

from run_sim_test import make_run

OWN_SEAT_LINE = "Your slot is held for you. Apply to rejoin, the host decides"
SLOTS_HELD_LINE = "All slots are held for returning players. Apply for a slot or wait"
WAIT_LINE = "Waiting for a slot to open - "


def service_args(port, ticket, report, extra, players, ticks):
    return ["-net-match-service-e2e", "-net-port", port, "-net-match-peers", players,
            "-net-match-ticks", ticks, "-net-reconnect-ticket", ticket,
            "-net-match-report", report, *extra]


def join_script(name, port, lines):
    return ("wait 40\nactivate ButtonMainToMultiplayer\nwait 12\n"
            f"settext TextMultiplayerName {name}\nactivate ButtonMultiplayerJoinGame\nwait 10\n"
            f"activate ButtonJoinByAddress\nwait 4\nsettext TextJoinAddress 127.0.0.1\nsettext TextJoinPort {port}\nactivate ButtonJoinAddressGo\n"
            "wait_state Failed 240\nwait 5\nassert_substate Landing\n" + "".join(line + "\n" for line in lines))


class Runs:
    def __init__(self, repo, root):
        self.repo, self.root, self.runs = repo, root, {}

    def make(self, name, argv, timeout=420, env=None):
        env = {"CCCP_HEADLESS": "1", **(env or {})}
        self.runs[name] = make_run(self.repo, argv, self.root / name, timeout, env=env)
        return self.runs[name]

    def start(self, name, argv, timeout=420, env=None):
        return self.make(name, argv, timeout, env).start()

    def wait_for(self, name, marker, seconds):
        run = self.runs[name]
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            log = run.out / "stdout.log"
            if log.exists() and marker in log.read_text(errors="replace"):
                return True
            if run.poll() is not None:
                raise RuntimeError(f"{run.out.name} ended before {marker}")
            time.sleep(0.1)
        raise RuntimeError(f"{run.out.name} did not reach {marker}")

    def log(self, name):
        path = self.root / name / "stdout.log"
        return path.read_text(errors="replace") if path.exists() else ""

    def close(self):
        for run in self.runs.values():
            run.close()


def script_checks(log, lines):
    """Every assertion line the menu script printed, and whether the script failed anywhere."""
    printed = [line.strip() for line in log.splitlines() if line.startswith("[menu-script]") and (" PASS" in line or " FAIL" in line)]
    return {"script_lines": printed[-len(lines):] if lines else printed, "script_failed": "[menu-script] FAILED:" in log}


def returner_arm(repo, root, port, result):
    runs = Runs(repo, root)
    players, ticks = 3, 2400
    trigger, signal = root / "join.trigger", root / "panel.signal.json"
    try:
        # The host's seats panel shows the request while the seat is held; the probe opens it once the request is in.
        probe = {"schema": 1, "timeout_ms": 180000, "steps": [
            {"op": "wait", "service": "Running"},
            {"op": "wait_file", "path": str(signal)},
            {"op": "key_down", "key": "F6"},
            {"op": "key_up", "key": "F6"},
            {"op": "wait", "panel_open": True},
            {"op": "wait", "renders": 3},
            {"op": "assert_control", "control": "NetworkSeatApplicant0", "equals": {"visible": True},
             "text_contains": "Client - same name"},
            {"op": "finish"},
        ]}
        (root / "probe.json").write_text(json.dumps(probe, indent=2), encoding="utf-8")
        runs.start("host", service_args(port, root / "host.ticket", root / "host_report.json",
                                        ["-net-host", "-net-match-e2e-resync"], players, ticks),
                   env={"CC_TEST_NET_UI_SCRIPT": str(root / "probe.json")})
        departing = runs.start("departing", service_args(port, root / "departing.ticket", root / "departing_report.json",
                                                         ["-net-join", "127.0.0.1", "-net-match-e2e-resync"], players, ticks))
        runs.start("stayer", service_args(port, root / "stayer.ticket", root / "stayer_report.json",
                                          ["-net-join", "127.0.0.1", "-net-match-e2e-resync"], players, ticks))
        runs.wait_for("host", "lobby_snapshot: state=Running", 120)
        identity = Path(departing.cwd) / "Userdata/NetworkIdentity.key"
        if not identity.is_file():
            raise RuntimeError("the departing peer wrote no identity to return with")
        lines = [f"assert_error {OWN_SEAT_LINE}", "assert_label ButtonMultiplayerReconnect Apply to Rejoin",
                 "assert_enabled ButtonMultiplayerReconnect 1", "assert_enabled ButtonMultiplayerWaitSlot 0",
                 "activate ButtonMultiplayerReconnect", "wait_ms 12000", "dump_lobby", "exit"]
        (root / "returner.txt").write_text(join_script("Client", port, lines), encoding="utf-8")
        # The same player: its identity comes back with it, its ticket does not.
        returner = runs.make("returner", ["-menu-script", root / "returner.txt", "-num-lua-states", 4,
                                          "-net-reconnect-ticket", root / "returner.ticket",
                                          "-net-match-report", root / "returner_report.json",
                                          "-net-join-wait-for", trigger])
        (Path(returner.cwd) / "Userdata").mkdir(parents=True, exist_ok=True)
        shutil.copy2(identity, Path(returner.cwd) / "Userdata/NetworkIdentity.key")
        returner.start()
        time.sleep(4)
        departing.terminate()
        runs.wait_for("host", "left the match at frame", 180)
        trigger.write_text("{}", encoding="utf-8")
        runs.wait_for("host", "[net-reconnect] applicant", 90)
        signal.write_text("{}", encoding="utf-8")
        exits = {name: runs.runs[name].finish()["exit_code"] for name in ("host", "stayer", "returner")}
        log = runs.log("returner")
        probe_result = json.loads((root / "net-ui-result.json").read_text(errors="replace"))
        host_log = runs.log("host")
        result["details"]["returner"] = {"exits": exits, **script_checks(log, lines),
                                         "refusal": next((line for line in host_log.splitlines() if "seat_held_for_you" in line or "live_match" in line), ""),
                                         "probe": {key: probe_result.get(key) for key in ("pass", "error", "failed_step")}}
        checks = result["checks"]
        checks["returner_told_its_slot_is_held"] = f'assert_error "{OWN_SEAT_LINE}"' in log and f'status="{OWN_SEAT_LINE}" PASS' in log
        checks["returner_offered_apply_to_rejoin"] = 'assert_label ButtonMultiplayerReconnect "Apply to Rejoin" text="Apply to Rejoin" PASS' in log
        checks["returner_offered_no_wait"] = "assert_enabled ButtonMultiplayerWaitSlot expected=0 actual=0 PASS" in log
        checks["returner_apply_pressed"] = "activate ButtonMultiplayerReconnect ok=1" in log
        checks["returner_script_ran_clean"] = "[menu-script] FAILED:" not in log
        checks["host_sees_the_request_beside_the_seat"] = probe_result.get("pass") is True
        checks["returner_host_exit"] = exits["host"] == 0
    except Exception as error:
        result.setdefault("errors", []).append(f"returner: {error}")
        result["checks"]["returner_arm_completed"] = False
    finally:
        runs.close()


def world_arm(repo, root, port, result):
    from test_autosave_restore import _carry_world_state
    runs = Runs(repo, root)
    # The world outlives the newcomer's whole script; its round cap only bounds a run that goes wrong.
    ticks = 60 * 900
    try:
        common = ["-net-match-peers", 2, "-net-match-input-delay", 3, "-net-autosave-seconds", 1, "-net-match-ticks", ticks]
        # Boot one: a dedicated world and its one player, killed together once the world has checkpoints on disk.
        runs.start("host", ["-net-dedicated", "-net-persistent-world", "-net-port", port, *common,
                            "-net-match-report", root / "host_report.json"], timeout=1200)
        runs.wait_for("host", "[net-world] identity", 120)
        player = runs.start("player", ["-net-match-service-e2e", "-net-join", "127.0.0.1", "-net-port", port, *common,
                                       "-net-reconnect-ticket", root / "player.ticket", "-net-match-report", root / "player_report.json"],
                            timeout=1200)
        runs.wait_for("player", "lobby_snapshot: state=Running", 240)
        deadline = time.monotonic() + 120
        while len(re.findall(r"(?m)^\[autosave\] retained tick=(\d+)", runs.log("host"))) < 3:
            if time.monotonic() > deadline:
                raise RuntimeError("the world wrote no checkpoints to restart from")
            time.sleep(0.2)
        runs.runs["host"].terminate(code=137, reason="world host killed")
        player.terminate()
        # Boot two: the same install restarts the world from its newest checkpoint; its player's seat is held for it and the
        # player stays away, so every seat of the restarted world is held when the newcomer arrives.
        restarted = runs.make("restarted", ["-net-dedicated", "-net-persistent-world", "-net-port", port + 2, *common,
                                            "-net-match-report", root / "restarted_report.json"], timeout=1200)
        _carry_world_state(root, "host", Path(restarted.cwd))
        restarted.start()
        runs.wait_for("restarted", "[autosave] resuming", 120)
        port += 2
        lines = [f"assert_error {SLOTS_HELD_LINE}", "assert_label ButtonMultiplayerReconnect Apply for a Slot",
                 "assert_enabled ButtonMultiplayerReconnect 1", "assert_enabled ButtonMultiplayerWaitSlot 1",
                 "activate ButtonMultiplayerWaitSlot", f"wait_label LabelMultiplayerStatus {WAIT_LINE}",
                 f"assert_label LabelMultiplayerStatus {WAIT_LINE}", "assert_label ButtonMultiplayerLeave Cancel",
                 "wait_ms 10000", f"assert_label LabelMultiplayerStatus {WAIT_LINE}",
                 "activate ButtonMultiplayerLeave", "wait 10", "assert_substate Landing",
                 "activate ButtonMultiplayerJoinGame", "wait 10", "activate ButtonJoinAddressGo",
                 "wait_state Failed 240", "wait 5", f"assert_error {SLOTS_HELD_LINE}",
                 "activate ButtonMultiplayerReconnect", "wait_ms 12000", "dump_lobby", "exit"]
        (root / "newcomer.txt").write_text(join_script("Newcomer", port, lines), encoding="utf-8")
        runs.start("newcomer", ["-menu-script", root / "newcomer.txt", "-num-lua-states", 4,
                                "-net-reconnect-ticket", root / "newcomer.ticket",
                                "-net-match-report", root / "newcomer_report.json"])
        exits = {"newcomer": runs.runs["newcomer"].finish()["exit_code"]}
        log = runs.log("newcomer")
        host_log = runs.log("restarted")
        result["details"]["world"] = {"exits": exits, **script_checks(log, lines)}
        checks = result["checks"]
        checks["newcomer_told_all_slots_are_held"] = f'assert_error "{SLOTS_HELD_LINE}" status="{SLOTS_HELD_LINE}" PASS' in log
        checks["newcomer_offered_apply_for_a_slot"] = 'assert_label ButtonMultiplayerReconnect "Apply for a Slot" text="Apply for a Slot" PASS' in log
        checks["newcomer_offered_wait"] = "assert_enabled ButtonMultiplayerWaitSlot expected=1 actual=1 PASS" in log
        counted = [int(found) for found in re.findall(r'assert_label LabelMultiplayerStatus "[^"]*" text="' + re.escape(WAIT_LINE) + r'(\d+) s" PASS', log)]
        result["details"]["world"]["countdown_s"] = counted
        checks["newcomer_wait_counts_down"] = len(counted) == 2 and counted[0] > counted[1]
        checks["newcomer_wait_cancels"] = 'assert_label ButtonMultiplayerLeave "Cancel" text="Cancel" PASS' in log and log.count("assert_substate expected=Landing actual=Landing PASS") >= 2
        checks["newcomer_offered_again_on_a_new_join"] = log.count(f'status="{SLOTS_HELD_LINE}" PASS') >= 2
        # Between rounds the host takes no application (moderation is a running match's); the Apply is answered, never left silent.
        answered = "[net-reconnect] applicant" in host_log or "admission refused reason=SeatNotSubstitutable" in host_log
        result["details"]["world"]["apply_answer"] = next((line.split("status=", 1)[1][:120] for line in log.splitlines() if line.startswith("[menu-script] dump_lobby")), "")
        checks["newcomer_apply_answered_by_the_host"] = "activate ButtonMultiplayerReconnect ok=1" in log and answered
        # The wait knocks every two seconds; each knock must reach the host's answer, never trip on the last knock's leftovers.
        knocks = log.count("admission refused reason=SessionFull role=client key=slots_held")
        result["details"]["world"]["slots_held_answers"] = knocks
        checks["newcomer_every_knock_answered"] = knocks >= 4 and "key=participant_identity" not in log
        checks["newcomer_script_ran_clean"] = "[menu-script] FAILED:" not in log
    except Exception as error:
        result.setdefault("errors", []).append(f"world: {error}")
        result["checks"]["world_arm_completed"] = False
    finally:
        runs.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=47637)
    parser.add_argument("--arm", choices=("returner", "world", "all"), default="all")
    options = parser.parse_args()
    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    result = {"pass": False, "checks": {}, "details": {}}
    if options.arm in ("returner", "all"):
        (root / "returner").mkdir()
        returner_arm(options.repo, root / "returner", options.port, result)
    if options.arm in ("world", "all"):
        (root / "world").mkdir()
        world_arm(options.repo, root / "world", options.port + 10, result)
    result["pass"] = bool(result["checks"]) and all(result["checks"].values())
    (root / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({"pass": result["pass"], "errors": result.get("errors"),
                      "failed": [k for k, v in result["checks"].items() if not v], "out": str(root)}), flush=True)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
