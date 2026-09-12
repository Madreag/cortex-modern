"""Assert the F6 seats panel shows the hold pause text while a dropped seat is held.

Three service-e2e peers start a match; the driver kills the departing one mid-match so
the survivors hold its seat for reclaim. The host runs a NetModerationGUIProbe script
(CC_TEST_NET_UI_SCRIPT) that opens the seats panel during the stall the drop causes and
waits for frames drawn by the lockstep wait's own UI pump, asserts the "Match paused:
waiting for ..." title once the host has applied the leave, then asserts the neutral
title once the hold lapses and the panel stays open. The probe result lands in
net-ui-result.json beside the probe script.
"""

import argparse
import json
from pathlib import Path
import time

from run_sim_test import make_run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=44282)
    options = parser.parse_args()
    players = 3
    ticks = 2200
    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    signal = root / "drop.signal.json"
    hold_signal = root / "hold.signal.json"
    probe = {
        "schema": 1,
        "timeout_ms": 180000,
        "steps": [
            {"op": "wait", "service": "Running"},
            {"op": "wait_file", "path": str(signal)},
            {"op": "key_down", "key": "F6"},
            {"op": "key_up", "key": "F6"},
            {"op": "wait", "panel_open": True},
            # The sim thread is parked in the lockstep wait, so these frames come from its UI pump.
            {"op": "wait", "renders": 5},
            {"op": "wait_file", "path": str(hold_signal)},
            {"op": "wait", "renders": 2},
            {"op": "assert_control", "control": "NetworkSeatsTitle", "equals": {"visible": True},
             "text_contains": "Match paused: waiting for"},
            # The hold budget is 1200 frames from the leave frame, so the lapse is counted in sim
            # frames, not wall time; once it lapses the title falls back while the panel stays open.
            {"op": "wait", "sim_at_least": 1550},
            {"op": "assert_control", "control": "NetworkSeatsTitle", "equals": {"visible": True},
             "text_contains": "match continues"},
            {"op": "finish"},
        ],
    }
    probe_path = root / "probe.json"
    probe_path.write_text(json.dumps(probe, indent=2), encoding="utf-8")
    result = {"pass": False, "checks": {}, "details": {}}
    runs = {}

    def start(name, host):
        out = root / name
        argv = ["-net-match-service-e2e", "-net-port", options.port, "-net-match-ticks", ticks,
                "-net-match-report", out / "report.json", "-net-match-input-delay", "3",
                "-out", out / "trace.json"]
        argv += ["-net-host", "-net-match-peers", players] if host else ["-net-join", "127.0.0.1"]
        env = {"CC_TEST_NET_UI_SCRIPT": str(probe_path)} if host else {}
        runs[name] = make_run(options.repo, argv, out, 300, env=env).start()

    try:
        start("host", True)
        start("departing", False)
        start("stayer", False)
        host_log = root / "host" / "stdout.log"
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            if host_log.exists() and "lobby_snapshot: state=Running" in host_log.read_text(errors="replace"):
                break
            if runs["host"].poll() is not None:
                raise RuntimeError("host ended before the match started")
            time.sleep(0.1)
        else:
            raise RuntimeError("host never reached a running match")
        time.sleep(4)
        runs["departing"].terminate()
        signal.write_text("{}", encoding="utf-8")
        # The hold only starts once the transport gives up on the killed peer, which the probe
        # cannot see; tell it when the host reports the leave.
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            if "left the match at frame" in host_log.read_text(errors="replace"):
                break
            if runs["host"].poll() is not None:
                raise RuntimeError("host ended before it applied the leave")
            time.sleep(0.1)
        else:
            raise RuntimeError("host never applied the departing peer's leave")
        hold_signal.write_text("{}", encoding="utf-8")
        for name in ("host", "stayer"):
            result["details"][name] = {"exit": runs[name].finish()["exit_code"]}
        probe_result = json.loads((root / "net-ui-result.json").read_text(errors="replace"))
        result["checks"]["probe_pass"] = probe_result.get("pass") is True
        result["checks"]["host_exit"] = result["details"]["host"]["exit"] == 0
        result["checks"]["stayer_exit"] = result["details"]["stayer"]["exit"] == 0
        result["details"]["probe"] = {"complete": probe_result.get("complete"),
                                      "error": probe_result.get("error"),
                                      "failed_step": probe_result.get("failed_step")}
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
