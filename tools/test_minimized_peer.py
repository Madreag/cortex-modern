"""A lockstep peer whose window is minimized keeps ticking and sending its input: the others never hold its seat or wait on it.

Two service peers on loopback; the client's window is minimized from tick 600 to tick 2400 (30 s) by the engine's test lever
CCCP_TEST_MINIMIZE_TICKS and then restored. The client must run at >= 59.5 ticks a second over that window, its seat never held,
the host never waiting a frame over 50 ms on it, every shared tick's live hash equal, the window really minimized and restored.

  python tools/test_minimized_peer.py --out D:/mx/<lane>/minimized-peer
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
from run_sim_test import make_run  # noqa: E402

FROM_TICK, TO_TICK, TICKS = 600, 2400, 3000


def live_rows(path: Path) -> dict[int, dict]:
    rows = {}
    for line in path.read_text(encoding="utf-8").splitlines() if path.is_file() else []:
        row = json.loads(line) if line.strip() else {}
        if "abandon_from" in row:
            rows = {tick: value for tick, value in rows.items() if tick < row["abandon_from"]}
        elif "tick" in row:
            rows[row["tick"]] = row
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=46002)
    options = parser.parse_args()
    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    runs, result = {}, {"checks": {}, "details": {}}

    def peer(name: str, role: list) -> list:
        return ["-net-match-service-e2e", "-net-port", options.port, "-net-match-peers", 2, "-net-match-ticks", TICKS,
                "-net-reconnect-ticket", root / f"{name}.ticket", "-net-match-report", root / f"{name}_report.json",
                "-net-live-tick-hashes", root / f"{name}-live.jsonl", *role]

    try:
        runs["host"] = make_run(REPO, peer("host", ["-net-host"]), root / "host", 300).start()
        runs["client"] = make_run(REPO, peer("client", ["-net-join", "127.0.0.1"]), root / "client", 300,
                                  env={"CCCP_TEST_MINIMIZE_TICKS": f"{FROM_TICK}:{TO_TICK}"}).start()
        for name in ("host", "client"):
            result["details"][name] = {"exit": runs[name].finish()["exit_code"]}
        host_log = (root / "host/stdout.log").read_text(encoding="utf-8", errors="replace")
        client_log = (root / "client/stdout.log").read_text(encoding="utf-8", errors="replace")
        window = re.findall(r"\[selftest\] window tick=(\d+) minimized=(\d) hidden=(\d)", client_log)
        result["details"]["window"] = window
        states = {int(tick): minimized == "1" for tick, minimized, _ in window}
        result["checks"]["window_minimized"] = states.get(FROM_TICK + 60) is True
        result["checks"]["window_restored"] = states.get(TO_TICK + 60) is False
        client = live_rows(root / "client-live.jsonl")
        host = live_rows(root / "host-live.jsonl")
        if FROM_TICK in client and TO_TICK in client:
            seconds = (client[TO_TICK]["wall_ms"] - client[FROM_TICK]["wall_ms"]) / 1000
            result["details"]["client_tps_minimized"] = round((TO_TICK - FROM_TICK) / seconds, 3) if seconds > 0 else None
        result["checks"]["client_ticks_while_minimized"] = (result["details"].get("client_tps_minimized") or 0) >= 59.5
        result["details"]["holds"] = re.findall(r"\[net-match\] hold peer=\d+ frame=(\d+)", host_log)
        result["checks"]["seat_never_held"] = not result["details"]["holds"] and "hold of this seat" not in client_log
        waits = [(int(tick), int(ms)) for tick, ms in re.findall(r"\[net-frame-wait\] frame=(\d+) wait_ms=(\d+)", host_log)]
        result["details"]["host_waits_over_50"] = [(tick, ms) for tick, ms in waits if FROM_TICK <= tick <= TO_TICK + 60 and ms > 50]
        result["checks"]["host_never_waited"] = not result["details"]["host_waits_over_50"]
        shared = sorted(set(host) & set(client))
        differing = [tick for tick in shared if host[tick].get("subsystems") != client[tick].get("subsystems")]
        result["details"]["shared_ticks"], result["details"]["differing_ticks"] = len(shared), differing[:5]
        result["checks"]["hashes_equal"] = len(shared) >= TICKS - 10 and not differing
        result["checks"]["exits"] = all(row["exit"] == 0 for row in result["details"].values() if isinstance(row, dict) and "exit" in row)
    except Exception as error:  # every peer is closed and the verdict written whatever happened
        result["error"] = f"{type(error).__name__}: {error}"
    finally:
        for run in runs.values():
            run.close()
    result["pass"] = bool(result["checks"]) and all(result["checks"].values()) and "error" not in result
    (root / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({"pass": result["pass"], "failed": [k for k, v in result["checks"].items() if not v], "error": result.get("error"),
                      "client_tps_minimized": result["details"].get("client_tps_minimized"), "out": str(root)}), flush=True)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
