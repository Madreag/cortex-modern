"""The TURN relay rows: a relayed connection outlives its permissions, and takes renewed logins live.

    python tools/turn_relay_rows.py hold --seconds 360 --turn 192.168.50.122:3479 --out <dir>
    python tools/turn_relay_rows.py renew --turn 192.168.50.122:3479 --out <dir>

Runs -net-p2p-selftest relay-hold / relay-renew through the runner with CCCP_HEADLESS=1. The TURN login is
minted from the directory coturn backend read by path (or inherited CC_TEST_TURN_USER / CC_TEST_TURN_PASS) and handed
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
    "renewed_login_answers": re.compile(r"ICE: TURN .*the renewed login"),
    "turn_warnings": re.compile(r"ICE: TURN .*(refused|timed out|holds no allocation)"),
}
COTURN_LINES = {
    "create_permission": re.compile(r"CREATE_PERMISSION processed, success"),
    "refresh": re.compile(r"REFRESH processed, success"),
    "peer_deleted": re.compile(r"peer \S+ deleted"),
    "allocate": re.compile(r"ALLOCATE processed, success"),
    "error_401": re.compile(r"error 401"),
    "error_441": re.compile(r"error 441"),
}


def read_login(conf: Path) -> tuple[str, str]:
    if os.environ.get("CC_TEST_TURN_USER") and os.environ.get("CC_TEST_TURN_PASS"):
        return os.environ["CC_TEST_TURN_USER"], os.environ["CC_TEST_TURN_PASS"]
    from session_directory.session_directory import TurnCredentialProvider
    import uuid
    config=json.loads(conf.read_text(encoding='utf-8'))
    if config.get('backend')!='coturn':
        raise ValueError('standalone TURN rows require a coturn directory backend; Cloudflare needs the A63.1 capability gate')
    offer=TurnCredentialProvider(config).mint('selftest-'+uuid.uuid4().hex,900,int(time.time()))
    server=next(row for row in offer['iceServers'] if row.get('username') and row.get('credential'))
    return server['username'],server['credential']


def fetch_coturn(spec: str, started: datetime, ended: datetime, relays: set[str], out: Path, book=None) -> dict:
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
    from acceptance_relay_policy import public_native_bytes
    if book is None:raise ValueError('coturn log capture requires its in-memory credential book')
    (out / "coturn.log").write_bytes(public_native_bytes(('\n'.join(lines)+'\n').encode(),book))
    counts = {name: sum(1 for line in lines if rx.search(line)) for name, rx in COTURN_LINES.items()}
    counts.update(lines=len(lines), sessions={number: sessions[number] for number in sorted(mine)}, other_sessions_in_window=len(sessions) - len(mine))
    return counts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("row", choices=("hold", "renew"))
    parser.add_argument("--turn", required=True, help="TURN server host:port")
    parser.add_argument("--seconds", type=int, default=360, help="hold: how long the relayed connection must keep passing data")
    parser.add_argument("--turn-config", "--login-conf", dest='login_conf', type=Path, default=Path("D:/mx/coturn-20260920/directory-coturn.json"))
    parser.add_argument("--coturn-log", default="", help="<ssh host>:<coturn log path>, fetched for the run window")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument('--host-box')
    parser.add_argument('--client-box')
    parser.add_argument('--inventory', type=Path)
    parser.add_argument('--collection-root', type=Path)
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--backends', choices=('cloudflare,coturn',))
    parser.add_argument('--stop-file',type=Path,help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.host_box or args.client_box:
        from acceptance_relay_pair import run
        return run(args,args.row,REPO)
    if args.dry_run:
        print(json.dumps(dict(row=args.row,engine_count=1,seconds=args.seconds)))
        return 0
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    user, password = read_login(args.login_conf)
    from acceptance_relay_policy import CredentialBook,sweep_retained
    from relay_private import inherited_environment,public_value
    book=CredentialBook();book.add('minted-username',user);book.add('minted-credential',password)

    words = ["relay-hold", str(args.seconds), args.turn] if args.row == "hold" else ["relay-renew", args.turn]
    timeout = args.seconds + 180 if args.row == "hold" else 180
    started = datetime.now()
    with inherited_environment(dict(CC_TEST_TURN_USER=user,CC_TEST_TURN_PASS=password)):
        run = make_run(REPO, ["-net-p2p-selftest", *words], out / "engine", timeout, env={"CCCP_HEADLESS": "1"})
    try:
        with inherited_environment(dict(CC_TEST_TURN_USER=user,CC_TEST_TURN_PASS=password)):run.start()
        if args.stop_file:
            while run.poll() is None:
                if args.stop_file.is_file():run.terminate(reason='coordinator closed its owned selftest')
                time.sleep(.05)
        record = run.finish()
    finally:
        try:run.close()
        finally:
            scan=sweep_retained(out,book)
            (out/'secret-scan.json').write_text(json.dumps(scan,indent=2)+'\n',encoding='utf-8')
    ended = datetime.now()
    time.sleep(1)

    log = (out / "engine" / "stdout.log").read_text(encoding="utf-8-sig", errors="replace") if (out / "engine" / "stdout.log").exists() else ""
    verdicts = re.findall(r"^\[net-p2p-selftest\] (PASS|FAIL.*)$", log, re.M)
    result = {
        "row": args.row,
        "turn": args.turn,
        "seconds": args.seconds if args.row == "hold" else None,
        "started": started.strftime('%Y-%m-%d %I:%M:%S %p'),
        "ended": ended.strftime('%Y-%m-%d %I:%M:%S %p'),
        "exit_code": record.get("exit_code"),
        "timed_out": record.get("timed_out"),
        "verdict": verdicts[-1] if verdicts else None,
        "passed": record.get("exit_code") == 0 and bool(verdicts) and verdicts[-1] == "PASS" and scan['passed'],
        "gns": {name: len(rx.findall(log)) for name, rx in GNS_LINES.items()},
        "hold_lines": [line for line in log.splitlines() if "hold " in line and "[net-p2p-selftest]" in line][-40:],
    }
    result['login_in_log']=any(value in log for value in (user,password)) or not scan['passed']
    relays = {f"{address}:{port}" for address, port in re.findall(r"candidate:\S+ \d+ udp \d+ (\S+) (\d+) typ relay", log)}
    result["relays"] = sorted(relays)
    if args.coturn_log:
        result["coturn"] = fetch_coturn(args.coturn_log, started, ended, relays, out,book)
    result=public_value(result,(user,password))
    scan=sweep_retained(out,book,previous=scan)
    result['passed']=result['passed'] and scan['passed']
    (out/'secret-scan.json').write_text(json.dumps(scan,indent=2)+'\n',encoding='utf-8')
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: v for k, v in result.items() if k != "hold_lines"}, indent=2))
    return 0 if result["passed"] and not result.get("login_in_log") else 1


if __name__ == "__main__":
    raise SystemExit(main())
