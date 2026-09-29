"""Exercise the engine's HTTPS directory transport and certificate refusal controls."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import ssl
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from run_sim_test import make_run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--port', type=int, default=50318)
    args = parser.parse_args()
    if sys.platform == 'win32':
        raise SystemExit('This proof runs on a POSIX box.')
    repo, root = args.repo.resolve(), args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    cert, key, other = (root / name for name in ('cert.pem', 'key.pem', 'other-cert.pem'))
    with (root / 'openssl.log').open('w') as log:
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                        '-keyout', str(key), '-out', str(cert), '-subj', '/CN=directory-pin.invalid'],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
        subprocess.run(['openssl', 'req', '-x509', '-new', '-key', str(key), '-days', '1',
                        '-out', str(other), '-subj', '/CN=other-leaf.invalid'],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    pins = {name: hashlib.sha256(ssl.PEM_cert_to_DER_cert(path.read_text())).hexdigest()
            for name, path in (('correct', cert), ('wrong_same_key', other))}
    (root / 'certificate-pins.json').write_text(json.dumps(pins, indent=2) + '\n')
    turn = root / 'turn.json'
    turn.write_text(json.dumps({'backend': 'coturn', 'static_auth_secret': 'local-directory-transport-proof',
                                'relay_urls': ['turn:127.0.0.1:50317?transport=udp']}))
    service_log = root / 'service.log'
    service_stdout = root / 'service-stdout.log'
    url = f'https://127.0.0.1:{args.port}'
    result = {'pass': False, 'url': url, 'port': args.port, 'pins': pins, 'cases': {}}
    def requests():
        text = service_log.read_text(errors='replace') if service_log.exists() else ''
        return re.findall(r'"(GET|POST|DELETE) ([^ ]+) HTTP/1\.[01]" (\d+)', text)
    with service_stdout.open('w') as service_output:
        service = subprocess.Popen([sys.executable, str(repo / 'tools/session_directory/session_directory.py'),
            '--bind', '127.0.0.1', '--port', str(args.port), '--cert', str(cert), '--key', str(key),
            '--turn-config', str(turn), '--log-file', str(service_log), '--expiry-s', '120'],
            stdout=service_output, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 10
            while 'session_directory listening' not in service_stdout.read_text(errors='replace'):
                if service.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError('HTTPS directory did not start; see service-stdout.log')
                time.sleep(0.05)
            for name, pin in (('wrong-pin', pins['wrong_same_key']), ('empty-pin', ''), ('correct-pin', pins['correct'])):
                before = len(requests())
                before_lines = len(service_log.read_text(errors='replace').splitlines())
                engine_args = ['-net-directory-probe', url]
                if pin:
                    engine_args.append(pin)
                run = make_run(repo, engine_args, root / name, 90, env={'CCCP_HEADLESS': '1'})
                try:
                    record = run.start().finish()
                finally:
                    run.close()
                text = (root / name / 'stdout.log').read_text(errors='replace')
                seen = requests()[before:]
                handshake_failures = [line for line in service_log.read_text(errors='replace').splitlines()[before_lines:]
                                      if 'tls handshake failed' in line]
                correct = name == 'correct-pin'
                error = 'certificate pin mismatch' if name == 'wrong-pin' else 'certificate verification failed'
                passed = (record['exit_code'] == 0 and '[net-directory-probe] PASS' in text
                          and ('POST', '/v1/sessions', '200') in seen and ('GET', '/v1/sessions', '200') in seen) if correct else (
                          record['exit_code'] != 0 and error in text and not seen and bool(handshake_failures))
                result['cases'][name] = {'pass': passed, 'record': record, 'requests': seen,
                                         'handshake_failures': handshake_failures,
                                         'diagnostics': [line for line in text.splitlines() if '[net-directory-probe]' in line and 'error=' in line]}
            before = len(requests())
            run = make_run(repo, ['-net-directory-selftest'], root / 'live-selftest', 180, env={'CCCP_HEADLESS': '1'})
            (Path(run.cwd) / 'DirectoryHttpFixture.json').write_text(json.dumps({'url': url, 'pin': pins['correct']}))
            try:
                record = run.start().finish()
            finally:
                run.close()
            text = (root / 'live-selftest/stdout.log').read_text(errors='replace')
            seen = requests()[before:]
            minted = any(method == 'POST' and path.endswith('/ice-servers') and status == '200' for method, path, status in seen)
            result['cases']['mint'] = {'pass': record['exit_code'] == 0 and minted and
                '[net-directory-selftest] PASS live_https_register_list_mint' in text,
                'record': record, 'requests': seen}
            result['pass'] = all(case['pass'] for case in result['cases'].values())
        except Exception as error:
            result['error'] = repr(error)
        finally:
            service.terminate()
            try:
                service.wait(timeout=10)
            except subprocess.TimeoutExpired:
                service.kill()
                service.wait(timeout=10)
    (root / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'pass': result['pass'], 'cases': {name: case['pass'] for name, case in result['cases'].items()},
                      'error': result.get('error')}))
    return 0 if result['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
