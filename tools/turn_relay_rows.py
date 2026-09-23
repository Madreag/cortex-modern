"""The TURN relay rows: a relayed connection outlives its permissions, and takes a renewed login live.

    python tools/turn_relay_rows.py hold --seconds 360 --turn 192.168.50.122:3479 --out <dir>
    python tools/turn_relay_rows.py renew --turn 192.168.50.122:3479 --out <dir>

Runs -net-p2p-selftest relay-hold / relay-renew through the runner with CCCP_HEADLESS=1. The TURN login is
read from a coturn config's "user=<name>:<password>" line (or CC_TEST_TURN_USER / CC_TEST_TURN_PASS) and handed
to the engine through its environment only: it never reaches a command line, a log or the run record.
With --coturn-log, the TURN server's own log lines for the run window are fetched over ssh as evidence.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timedelta
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
from run_sim_test import make_run  # noqa: E402

GNS_LINES = {
    "permissions_installed": re.compile(r"ICE: TURN permissions of relay \S+ installed"),
    "allocation_refreshed": re.compile(r"ICE: TURN relay \S+ refreshed for"),
    "login_updated": re.compile(r"ICE: TURN login updated"),
    "relay_login_renewed": re.compile(r"\[net-relay\] relay login renewed on \d+ live connection"),
    "turn_warnings": re.compile(r"ICE: TURN .*(refused|timed out|holds no allocation)"),
}
COTURN_LINES = {
    "create_permission": re.compile(r"CREATE_PERMISSION processed, success"),
    "refresh": re.compile(r"REFRESH processed, success"),
    "peer_deleted": re.compile(r"peer \S+ deleted"),
    "allocate": re.compile(r"ALLOCATE processed, success"),
    "error_401": re.compile(r"error 401"),
}


def read_login(conf: Path) -> tuple[str, str]:
    if os.environ.get("CC_TEST_TURN_USER") and os.environ.get("CC_TEST_TURN_PASS"):
        return os.environ["CC_TEST_TURN_USER"], os.environ["CC_TEST_TURN_PASS"]
    for line in conf.read_text(encoding="utf-8", errors="replace").splitlines():
        match = re.match(r"\s*user\s*=\s*([^:\s]+):(\S+)", line)
        if match:
            return match[1], match[2]
    raise SystemExit(f"no user=<name>:<password> line in {conf}")


def fetch_coturn(spec: str, started: datetime, ended: datetime, relays: set[str], out: Path) -> dict:
    """spec = <ssh host>:<log path>. Keeps the run window's lines of the sessions that allocated this run's relays."""
    host, _, path = spec.partition(":")
    hours = sorted({(started + timedelta(hours=h)).strftime("%Y-%m-%dT%H:") for h in range(int((ended - started).total_seconds() // 3600) + 2)})
    pattern = "|".join(re.escape(h) for h in hours)
    command = f"/bin/zsh -lc \"grep -a -E '^({pattern})' {path}\""
    text = subprocess.run(["ssh", "-o", "BatchMode=yes", host, command], capture_output=True, text=True, timeout=120).stdout
    begin, end = started.strftime("%Y-%m-%dT%H:%M:%S"), ended.strftime("%Y-%m-%dT%H:%M:%S")
    window = [line for line in text.splitlines() if begin <= line[:19] <= end]
    # coturn names the relay address on the line before the session's "allocation new".
    sessions, pending = {}, None
    for line in window:
        relay = re.search(r"Local relay addr: (\S+)", line)
        if relay:
            pending = relay[1]
            continue
        session = re.search(r"allocation new.*\(session (\d+)\)", line)
        if session and pending:
            sessions[session[1]] = pending
            pending = None
    mine = {number for number, relay in sessions.items() if relay in relays}
    lines = [line for line in window if any(f"(session {number})" in line for number in mine)]
    (out / "coturn.log").write_text("\n".join(lines) + "\n", encoding="utf-8")
    counts = {name: sum(1 for line in lines if rx.search(line)) for name, rx in COTURN_LINES.items()}
    counts.update(lines=len(lines), sessions={number: sessions[number] for number in sorted(mine)}, other_sessions_in_window=len(sessions) - len(mine))
    return counts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("row", choices=("hold", "renew"))
    parser.add_argument("--turn", required=True, help="TURN server host:port")
    parser.add_argument("--seconds", type=int, default=360, help="hold: how long the relayed connection must keep passing data")
    parser.add_argument("--login-conf", type=Path, default=Path("D:/mx/coturn-20260920/turnserver-fixed.conf"))
    parser.add_argument("--coturn-log", default="", help="<ssh host>:<coturn log path>, fetched for the run window")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    user, password = read_login(args.login_conf)
    # The engine inherits the login; passing it through make_run's env would copy it into launch.json.
    os.environ["CC_TEST_TURN_USER"], os.environ["CC_TEST_TURN_PASS"] = user, password

    words = ["relay-hold", str(args.seconds), args.turn] if args.row == "hold" else ["relay-renew", args.turn]
    timeout = args.seconds + 180 if args.row == "hold" else 180
    started = datetime.now()
    run = make_run(REPO, ["-net-p2p-selftest", *words], out / "engine", timeout, env={"CCCP_HEADLESS": "1"})
    try:
        run.start()
        record = run.finish()
    finally:
        run.close()
    ended = datetime.now()
    time.sleep(1)

    log = (out / "engine" / "stdout.log").read_text(encoding="utf-8-sig", errors="replace") if (out / "engine" / "stdout.log").exists() else ""
    verdicts = re.findall(r"^\[net-p2p-selftest\] (PASS|FAIL.*)$", log, re.M)
    result = {
        "row": args.row,
        "turn": args.turn,
        "seconds": args.seconds if args.row == "hold" else None,
        "started": started.isoformat(timespec="seconds"),
        "ended": ended.isoformat(timespec="seconds"),
        "exit_code": record.get("exit_code"),
        "timed_out": record.get("timed_out"),
        "verdict": verdicts[-1] if verdicts else None,
        "passed": record.get("exit_code") == 0 and bool(verdicts) and verdicts[-1] == "PASS",
        "gns": {name: len(rx.findall(log)) for name, rx in GNS_LINES.items()},
        "hold_lines": [line for line in log.splitlines() if "hold " in line and "[net-p2p-selftest]" in line][-40:],
    }
    if password in log:
        result["password_in_log"] = True
    relays = {f"{address}:{port}" for address, port in re.findall(r"candidate:\S+ \d+ udp \d+ (\S+) (\d+) typ relay", log)}
    result["relays"] = sorted(relays)
    if args.coturn_log:
        result["coturn"] = fetch_coturn(args.coturn_log, started, ended, relays, out)
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: v for k, v in result.items() if k != "hold_lines"}, indent=2))
    return 0 if result["passed"] and not result.get("password_in_log") else 1


if __name__ == "__main__":
    raise SystemExit(main())
