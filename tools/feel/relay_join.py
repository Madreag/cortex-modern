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
import sys
import subprocess
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--port', type=int, default=49492)
    parser.add_argument('--game-port', type=int, default=49493)
    parser.add_argument('--turn', required=True)
    parser.add_argument('--keep-wsl-running', action='store_true')
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
    keepalive = None
    if args.keep_wsl_running:
        keepalive = subprocess.Popen(['wsl', '-e', 'sleep', '240'], creationflags=subprocess.CREATE_NO_WINDOW, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
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
            run = make_run(repo, flags, root / peer, 180, env={'CCCP_HEADLESS': '1', 'CC_TEST_GNS_TRACE': '1'})
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
                  directory_no_429=not re.search(r'\b429\b', directory), directory_port=args.port, game_port=args.game_port)
    (root / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f"[relay-join] {'PASS' if passed else 'FAIL'} " + str(root / 'result.json'))
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
