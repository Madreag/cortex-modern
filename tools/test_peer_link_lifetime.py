"""One local pair retains its match links through twelve thousand common ticks."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import select
import socket
import threading
from contextlib import contextmanager
import sys
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--repo', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--port', type=int, default=41961)
args = parser.parse_args()
repo, root = args.repo.resolve(), args.out.resolve()
root.mkdir(exist_ok=False)
sys.path.insert(0, str(repo / 'tools'))
from run_sim_test import engine_executable, make_run, seed_settings
from test_directory_cadence import local_directory, stage_activity
from test_lobby_lifecycle import wait_for_log

@contextmanager
def peer_path(port):
    front = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    back = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    front.bind(('127.0.0.1', port + 3))
    back.bind(('127.0.0.1', 0))
    stopped = threading.Event()
    receipt = {'packets': 0, 'dropped': 0, 'first_ms': None, 'quiet_after_ms': 26000}

    def forward():
        sender = None
        while not stopped.is_set():
            ready, _, _ = select.select([front, back], [], [], .1)
            for wire in ready:
                data, address = wire.recvfrom(65535)
                now_ms = time.monotonic_ns() // 1000000
                if receipt['first_ms'] is None:
                    receipt['first_ms'] = now_ms
                receipt['packets'] += 1
                if now_ms - receipt['first_ms'] >= receipt['quiet_after_ms']:
                    receipt['dropped'] += 1
                    continue
                if wire is front:
                    sender = address
                    back.sendto(data, ('127.0.0.1', port + 2))
                elif sender:
                    front.sendto(data, sender)

    worker = threading.Thread(target=forward, daemon=True)
    worker.start()
    try:
        yield receipt
    finally:
        stopped.set()
        worker.join()
        front.close()
        back.close()

common = 'in_play\nwait_ms 1000\nactivate ButtonMainToMultiplayer\nwait_ms 300\n'
scripts = {
    'host': common + 'settext TextMultiplayerName Host\nactivate ButtonMultiplayerHostGame\nwait_ms 300\n'
            'combo_select ComboHostActivity Directory Cadence\nwait_ms 300\n' +
            f'setup_host_port {args.port}\ncombo_select ComboHostPlayers 2\nactivate ButtonMultiplayerCreate\n'
            'wait_connected 2 80\nwait_remote_ready 80\nactivate ButtonMultiplayerStart\n'
            'wait_lockstep 12000 250\nwait_connected 1 10\nopen_local_pause\n'
            'activate ButtonEndMatch\nactivate ButtonLeaveConfirm\nwait_round_end 10\nwait_ms 1000\nexit\n',
    'guest': common + 'settext TextMultiplayerName Guest\nactivate ButtonMultiplayerJoinGame\nwait_ms 300\n'
             'activate ButtonJoinByAddress\nwait 4\nsettext TextJoinAddress 127.0.0.1\n' +
             f'settext TextJoinPort {args.port}\nactivate ButtonJoinAddressGo\nwait_connected 2 80\n'
             'activate ButtonMultiplayerReady\nwait_lockstep 12000 250\nopen_local_pause\n'
             'activate ButtonLeaveMatch\nactivate ButtonLeaveConfirm\nwait_ms 1500\nexit\n',
}
runs, records, checks, details = {}, {}, {}, {}
tip = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip()
result = dict(source_tip=tip, binary_sha256=hashlib.sha256(engine_executable(repo).read_bytes()).hexdigest(),
              driver_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              topology='single-box', local_directory=True, relay_proof=False)
try:
    with peer_path(args.port + 10) as path_receipt, local_directory(root / 'directory', 'links', args.port + 1) as (server, requests, pin):
        for role in ('host', 'guest'):
            if role == 'guest':
                deadline = time.monotonic() + 20
                while not server.store._sessions and time.monotonic() < deadline:
                    time.sleep(.05)
                with server.store._lock:
                    session = next(iter(server.store._sessions))
                scripts[role] = scripts[role].replace('TextJoinAddress 127.0.0.1', 'TextJoinAddress session:' + session)
            script = root / (role + '.menu.txt')
            script.write_text(scripts[role])
            out = root / role
            run = make_run(repo, ['-module', 'DirectoryCadence.rte', '-menu-script', script,
                                 '-num-lua-states', '4', '-net-match-ticks', '0', '-net-rendezvous-log', '4',
                                 '-net-match-report', out / 'match-report.json',
                                 '-net-live-tick-hashes', out / 'live.jsonl'], out, timeout=330, env={'CC_TEST_PEER_LINK_PORT': str(args.port + 10)})
            seed_settings(run, {'SessionDirectoryUrl': f'127.0.0.1:{args.port + 1}',
                                'SessionDirectoryCertSha256': pin, 'NetworkHostVisibility': 1,
                                'NetworkIceEnable': 1, 'NetworkConnectionMode': 1,
                                'NetworkHostRelayMode': 0, 'NetworkStunServers': '', 'NetworkInputDelayFrames': 3})
            stage_activity(repo, run)
            runs[role] = run.start()
            if role == 'host':
                wait_for_log(run, 'activate ButtonMultiplayerCreate click ButtonMultiplayerCreate PASS', 45)
        with ThreadPoolExecutor(max_workers=2) as pool:
            futures = {role: pool.submit(run.finish) for role, run in runs.items()}
            records = {role: future.result() for role, future in futures.items()}
    logs = {role: (root / role / 'stdout.log').read_text(errors='replace') for role in runs}
    rows = {role: [json.loads(line) for line in (root / role / 'live.jsonl').read_text().splitlines()]
            for role in runs}
    hashes = {}
    for role in runs:
        checks[role + '_exit'] = records[role]['exit_code'] == 0 and not records[role].get('timed_out')
        leave = re.search(r'\[net-match-leave\].*frame=(\d+).*reason=Match left', logs[role])
        last = int(leave[1]) if leave else 2**64 - 1
        hashes[role] = {row['tick']: row['total'] for row in rows[role] if row['tick'] < last}
        end = re.search(r'\[net-match-leave\].*reason=Match left|\[net-peer-departure\].*source=round-notice', logs[role])
        before_leave = logs[role][:end.start()] if end else logs[role]
        closes = [line for line in before_leave.splitlines() if '[net-transport] closed' in line]
        checks[role + '_no_transport_close'] = not closes
        details[role + '_closes'] = closes
        details[role + '_connections'] = [line for line in logs[role].splitlines()
                                         if '[net-ice] connected' in line or '[net-peer-link]' in line]
    checks['purposeful_retirement'] = any('[net-peer-link] retired source=local' in text for text in logs.values())
    checks['retained_route_confirmed'] = any(re.search(r'retained_sent=[1-9]\d* retained_received=[1-9]\d*.*reader_detached=1', text) for text in logs.values())
    shared = sorted(set(hashes['host']) & set(hashes['guest']))
    differences = [tick for tick in shared if hashes['host'][tick] != hashes['guest'][tick]]
    checks['common_ticks'] = len(shared) >= 12000
    checks['identical_hashes'] = not differences
    checks['real_leave'] = all(any(line.startswith('[menu-script] ' + step) and line.endswith('PASS')
                                 for line in logs['guest'].splitlines())
                             for step in ('open_local_pause', 'activate ButtonLeaveMatch', 'activate ButtonLeaveConfirm'))
    details.update(common_ticks=len(shared), first_mismatches=differences[:10], peer_path=path_receipt)
except Exception as error:
    checks['completed'] = False
    details['error'] = str(error)
finally:
    for run in runs.values():
        run.close()
result.update(pass_=bool(checks) and all(checks.values()), checks=checks, details=details, records=records)
(root / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps({'pass': result['pass_'], 'checks': checks, 'details': details, 'out': str(root)}), flush=True)
raise SystemExit(not result['pass_'])
