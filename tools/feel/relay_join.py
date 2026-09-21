"""Run two relay-only peers against a private TLS directory and a named TURN server."""
from __future__ import annotations

import argparse
from datetime import datetime, timedelta, timezone
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import re
import socket
import struct
import sys
import subprocess
import threading
import time

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from run_sim_test import make_run
from test_directory_ice_join import start_service, list_sessions, patch_settings
from feel_measure import stage_baseline
from feel.retained_resume import compare_live_hashes

STUN_MAGIC = 0x2112A442
# A WSL relay only answers the host while the VM's neighbour entry is resolved, so the keepalive
# makes the VM talk to its gateway instead of merely holding the distribution open.
WSL_KEEPALIVE = 'gw=$(ip route show default | cut -d" " -f3); for i in $(seq 1 240); do ping -c 1 -W 1 $gw >/dev/null 2>&1; sleep 5; done'


def turn_endpoint(url):
    body = url.split('?', 1)[0]
    if body.startswith('turn:') or body.startswith('turns:'):
        body = body.split(':', 1)[1]
    host, _, port = body.rpartition(':')
    return (host or body).strip('[]'), int(port) if port.isdigit() else 3478


def stun_binding(host, port, timeout=1.5):
    try:
        family, kind, proto, _, address = socket.getaddrinfo(host, port, type=socket.SOCK_DGRAM)[0]
    except OSError:
        return False
    with socket.socket(family, kind, proto) as probe:
        probe.settimeout(timeout)
        try:
            probe.sendto(struct.pack('>HHI', 0x0001, 0, STUN_MAGIC) + os.urandom(12), address)
            return len(probe.recvfrom(2048)[0]) >= 20
        except OSError:
            return False


def warm_wsl_path():
    subprocess.run(['wsl', '-e', 'sh', '-c', 'ping -c 1 -W 1 $(ip route show default | cut -d" " -f3) >/dev/null 2>&1; exit 0'],
                   creationflags=subprocess.CREATE_NO_WINDOW, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)


def sample_turn(host, port, samples, stop, period=5.0):
    while not stop.is_set():
        samples['ok' if stun_binding(host, port) else 'fail'] += 1
        stop.wait(period)


def require_turn_reachable(host, port):
    for attempt in range(3):
        if stun_binding(host, port):
            return attempt
        warm_wsl_path()
    raise RuntimeError(f'the TURN server {host}:{port} did not answer a STUN binding from this host after 3 attempts; '
                       'the peers would time out on rendezvous with no relay candidate')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--port', type=int, default=49492)
    parser.add_argument('--game-port', type=int, default=49493)
    parser.add_argument('--turn', required=True)
    parser.add_argument('--keep-wsl-running', action='store_true')
    parser.add_argument('--rendezvous-log', type=int, default=0)
    args = parser.parse_args()
    if not all(49470 <= value <= 49499 for value in (args.port, args.game_port)):
        parser.error('ports must stay in 49470..49499')
    username, password = os.environ['CC_TEST_TURN_USER'], os.environ['CC_TEST_TURN_PASS']
    repo, root = Path(__file__).resolve().parents[2], args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'localhost')])
    now = datetime.now(timezone.utc)
    certificate = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
                   .serial_number(x509.random_serial_number()).not_valid_before(now - timedelta(minutes=1))
                   .not_valid_after(now + timedelta(days=1)).add_extension(x509.SubjectAlternativeName([
                       x509.DNSName('localhost'), x509.IPAddress(ipaddress.ip_address('127.0.0.1'))]), critical=False)
                   .sign(key, hashes.SHA256()))
    cert, key_path = root / 'cert.pem', root / 'key.pem'
    cert.write_bytes(certificate.public_bytes(serialization.Encoding.PEM))
    key_path.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    pin = hashlib.sha256(certificate.public_bytes(serialization.Encoding.DER)).hexdigest()
    settings = dict(SessionDirectoryUrl=f'127.0.0.1:{args.port}', SessionDirectoryCertSha256=pin,
                    NetworkIceEnable='1', NetworkStunServers='', NetworkConnectionMode='RelayOnly',
                    NetworkHostRelayMode='Fixed', NetworkTurnServers=args.turn,
                    NetworkTurnUser=username, NetworkTurnPass=password,
                    NetworkPlayerTurnServers=args.turn, NetworkPlayerTurnUser=username, NetworkPlayerTurnPass=password)
    turn_host, turn_port = turn_endpoint(args.turn)
    keepalive = None
    if args.keep_wsl_running:
        keepalive = subprocess.Popen(['wsl', '-e', 'sh', '-c', WSL_KEEPALIVE], creationflags=subprocess.CREATE_NO_WINDOW,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    warm_attempts = require_turn_reachable(turn_host, turn_port)
    reachable, sampler_stop = {'ok': 0, 'fail': 0}, threading.Event()
    sampler = threading.Thread(target=sample_turn, args=(turn_host, turn_port, reachable, sampler_stop), daemon=True)
    sampler.start()
    service = start_service(root, args.port, cert, key_path)
    runs, records = {}, {}
    try:
        session_id = ''
        for peer in ('host', 'client'):
            flags = ['-net-match-service-e2e', '-net-port', str(args.game_port), '-net-match-ticks', '1200',
                     '-net-match-peers', '2', '-net-ice', 'on', '-num-lua-states', '4', '-seed', '42',
                     '-net-match-auto-delay', '-tick-hashes', '-net-match-service-preset', 'Determinism FeelBaseline',
                     '-net-match-service-module', 'UserScenes.rte', '-out', str(root / f'{peer}_trace.json'),
                     '-net-match-report', str(root / f'{peer}_report.json'),
                     '-net-live-tick-hashes', str(root / f'{peer}-live.jsonl')]
            flags += ['-net-host'] if peer == 'host' else ['-net-join-session', session_id]
            if args.rendezvous_log:
                flags += ['-net-rendezvous-log', str(args.rendezvous_log)]
            run = make_run(repo, flags, root / peer, 180, env={'CCCP_HEADLESS': '1'})
            runs[peer] = run
            patch_settings(Path(run.cwd), {**settings, 'SessionDirectoryInstallKey': f'feel-relay-{peer}-install'})
            stage_baseline(run, 1200)
            run.start()
            if peer == 'host':
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline:
                    rows = list_sessions(args.port)
                    if rows:
                        session_id = rows[0]['session_id']
                        break
                    if run.poll() is not None:
                        break
                    time.sleep(1)
                if not session_id:
                    raise RuntimeError('the host did not publish its directory row')
        for peer in ('client', 'host'):
            records[peer] = runs[peer].finish()
    finally:
        sampler_stop.set()
        sampler.join(timeout=5)
        for run in runs.values():
            run.close()
        service.terminate()
        service.wait(timeout=10)
        if keepalive is not None:
            keepalive.terminate()
            keepalive.wait(timeout=10)
    checks = {}
    for peer in ('host', 'client'):
        log = (root / peer / 'stdout.log').read_text(encoding='utf-8-sig', errors='replace')
        lines = [line for line in log.splitlines() if '[net-ice] selected candidate=' in line or '[net-route] RouteAllowed' in line]
        report = json.loads((root / f'{peer}_report.json').read_text()) if (root / f'{peer}_report.json').exists() else {}
        checks[peer] = dict(lines=lines, completed=records[peer].get('exit_code') == 0 and report.get('running_ticks', 0) >= 1200,
                            relay_candidate=bool(re.search(r'\[net-ice\] selected candidate=.*relay', log)),
                            relay_allowed='[net-route] RouteAllowed route=relay allowed=1' in log,
                            no_429=not re.search(r'\b429\b', log))
    directory = (root / 'service.log').read_text(encoding='utf-8', errors='replace')
    comparisons = compare_live_hashes(root / 'host-live.jsonl', root / 'client-live.jsonl', 1)
    passed = (all(all(value for key, value in checks[peer].items() if key != 'lines') for peer in checks)
              and not re.search(r'\b429\b', directory) and bool(comparisons)
              and all(row['compared_ticks'] > 0 and row['mismatched_ticks'] == 0 for row in comparisons))
    result = dict(passed=passed, checks=checks, comparisons=comparisons,
                  directory_no_429=not re.search(r'\b429\b', directory), directory_port=args.port, game_port=args.game_port,
                  relay=dict(endpoint=f'{turn_host}:{turn_port}', warm_attempts=warm_attempts, reachable=reachable))
    (root / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f"[relay-join] {'PASS' if passed else 'FAIL'} " + str(root / 'result.json'))
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
