"""Record ak47_fire.ccreplay from a two-peer P4 Alpha Duel plus a named Ronin spawn.

    python tools/record_ak47_fire.py --repo D:/Projects/item4-feel --out D:/mx/w90/record
"""

import argparse
import json
import shutil
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_sim_test import make_run
from wait_engine_idle import wait_engine_idle

FIXTURE_TXT = Path(r"D:\Projects\stage2_p4\fixtures\ak47_fire.txt")
FIXTURE_REPLAY = Path(r"D:\Projects\stage2_p4\fixtures\ak47_fire.ccreplay")
SPAWN = "AHuman:Ronin Rifleman:Ronin.rte:790:200:20:0"
PORT = 47690
TICKS = 750
DELAY = 3


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=PORT)
    parser.add_argument("--ticks", type=int, default=TICKS)
    parser.add_argument("--copy-fixture", action="store_true")
    options = parser.parse_args()
    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    wait_engine_idle()
    replay = root / "ak47_fire.ccreplay"
    host_report = root / "host_report.json"
    client_report = root / "client_report.json"
    common = [
        "-net-match-service-e2e",
        "-net-port",
        str(options.port),
        "-net-match-ticks",
        str(options.ticks),
        "-net-match-input-delay",
        str(DELAY),
        "-tick-hashes",
        "-max-ticks",
        str(options.ticks),
        "-free-run-sim",
    ]
    host_args = [
        *common,
        "-net-host",
        "-net-match-report",
        str(host_report),
        "-out",
        str(root / "host_trace.json"),
        "-net-replay-out",
        str(replay),
        "-input-script",
        str(FIXTURE_TXT),
        "-net-match-e2e-spawn",
        SPAWN,
    ]
    client_args = [
        *common,
        "-net-join",
        "127.0.0.1",
        "-net-match-report",
        str(client_report),
        "-out",
        str(root / "client_trace.json"),
    ]
    host = make_run(options.repo, host_args, root / "host", timeout=420)
    client = make_run(options.repo, client_args, root / "client", timeout=420)
    records = {}

    def drive(run, key):
        try:
            run.start()
            records[key] = run.finish()
        except Exception as exc:
            records[key] = {"error": repr(exc)}

    try:
        host_thread = threading.Thread(target=drive, args=(host, "host"))
        host_thread.start()
        time.sleep(1.5)
        client_thread = threading.Thread(target=drive, args=(client, "client"))
        client_thread.start()
        host_thread.join()
        client_thread.join()
    finally:
        host.close()
        client.close()

    result = {
        "host": {k: records.get("host", {}).get(k) for k in ("exit_code", "timed_out", "pid", "error")},
        "client": {k: records.get("client", {}).get(k) for k in ("exit_code", "timed_out", "pid", "error")},
        "replay": str(replay) if replay.exists() else None,
        "replay_bytes": replay.stat().st_size if replay.exists() else 0,
        "spawn": SPAWN,
        "port": options.port,
        "ticks": options.ticks,
    }
    if options.copy_fixture and replay.exists():
        shutil.copy2(replay, FIXTURE_REPLAY)
        result["fixture_replay"] = str(FIXTURE_REPLAY)
    (root / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2), flush=True)
    return 0 if replay.exists() and records.get("host", {}).get("exit_code") == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
