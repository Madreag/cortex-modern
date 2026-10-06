"""Assert a hosted lobby announces the agreed input delay on both peers.

The host and one client each run ``dump_lobby`` once everyone is ready; the menu-script
output must carry ``input_delay="Input delay: <N> (...)`` — the host's announced value,
not a locally recomputed one. GUI-hosted lobbies always run auto mode, so both peers
must report ``(auto``. Follows tools/test_lobby_lifecycle.py.
"""

import argparse
import json
from pathlib import Path
import re
import time

from run_sim_test import seed_settings
from test_menu_readback import spread, managed_case


def menu_script(name, host, players, port):
    script = f"wait 40\nactivate ButtonMainToMultiplayer\nwait 12\nsettext TextMultiplayerName {name}\n"
    if host:
        return script + f"activate ButtonMultiplayerHostGame\nwait 10\nsetup_host_port {port}\ncombo_select ComboHostPlayers {players}\nwait 4\nactivate ButtonMultiplayerCreate\n"
    return script + f"activate ButtonMultiplayerJoinGame\nwait 10\nactivate ButtonJoinByAddress\nwait 4\nsettext TextJoinAddress 127.0.0.1\nsettext TextJoinPort {port}\nactivate ButtonJoinAddressGo\nwait_connected {players}\nactivate ButtonMultiplayerReady\n"


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


@managed_case
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=47613)
    if spread:
        spread.add_arguments(parser)
    options = parser.parse_args()
    if not spread:
        parser.error("lobby input delay requires the shared spread executor")
    options.spread = True
    spread.configure(options)
    players = 2
    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    result = {"pass": False, "checks": {}, "details": {}, "topology": "spread", "proof": False}
    runs = {}
    executor = None

    def start(name, script):
        path = root / f"{name}.txt"
        path.write_text(script, encoding="utf-8")
        out = root / name
        # The delay box is read-only under the auto policy; the floor the lobby sends is a setting.
        run = executor.make_run(options.repo, ["-menu-script", path], out, 120)
        seed_settings(run, {"NetworkInputDelayFrames": 3})
        runs[name] = run.start()
        return runs[name]

    try:
        executor = spread.prepare_case(options.repo, root,
                                      [spread.Peer("host", os="windows", reviewed=True), spread.Peer("client", os="windows")],
                                      spread.Match(options.port))
        host_script = menu_script("Host", True, players, options.port) + \
            f"wait_connected {players}\nwait_remote_ready\nwait_all_ready\ndump_lobby\nwait 10\nexit\n"
        host_run = start("host", host_script)
        wait_for_log(host_run, "activate ButtonMultiplayerCreate click ButtonMultiplayerCreate PASS")
        client_script = menu_script("Client", False, players, options.port) + "wait_all_ready\ndump_lobby\nwait 10\nexit\n"
        start("client", client_script)
        for name, run in runs.items():
            record = run.finish()
            log = (run.out / "stdout.log").read_text(errors="replace")
            errors = re.findall(r"^.*(?:FAILED|FAIL|EXCEPTION_|RTE Assert|RTE Abort|Runtime Error).*$", log, re.M)
            dumps = [line for line in log.splitlines() if "dump_lobby" in line and "input_delay=" in line]
            delays = [re.search(r'input_delay="([^"]*)"', line).group(1) for line in dumps]
            result["checks"][f"{name}_process"] = record["exit_code"] == 0 and not record["timed_out"]
            result["checks"][f"{name}_steps"] = not errors and "allready -> OK" in log
            result["checks"][f"{name}_input_delay_present"] = bool(delays) and all(d.startswith("Input delay: ") for d in delays)
            result["checks"][f"{name}_input_delay_auto"] = bool(delays) and all("(auto" in d for d in delays)
            result["details"][name] = {"exit": record["exit_code"], "errors": errors, "delays": delays}
        if "delays" in result["details"].get("host", {}) and "delays" in result["details"].get("client", {}):
            host_d, client_d = result["details"]["host"]["delays"], result["details"]["client"]["delays"]
            result["checks"]["same_announced_delay"] = bool(host_d and client_d) and \
                re.sub(r", \d+ms ping", "", host_d[-1]) == re.sub(r", \d+ms ping", "", client_d[-1])
        result["pass"] = all(result["checks"].values())
    except Exception as error:
        result["error"] = str(error)
    finally:
        for run in runs.values():
            run.close()
        if executor:
            result.update(topology="spread", peer_boxes=executor.result()["peer_boxes"], spread=executor.result(), proof=result["pass"])
        (root / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({"pass": result["pass"], "error": result.get("error"),
                      "failed": [k for k, v in result["checks"].items() if not v], "out": str(root)}), flush=True)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
