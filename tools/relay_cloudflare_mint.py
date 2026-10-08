"""A live check that the session directory mints a Cloudflare relay login through its own HTTP API.

    python tools/relay_cloudflare_mint.py --turn-config <cloudflare key file> --out <dir>
                                          [--module <a session_directory.py>] [--ttl 600] [--scan <root> ...]

The directory runs in this process on a loopback port with the key file's backend, read by path. One session
registers, asks for its relay (the host's POST) and fetches the offer back (a Relay only client's GET). The receipt
keeps public evidence only: the directory's statuses, Cloudflare's own status for each request it answered, the
lifetime and the server URLs. The key, the token and every minted login stay in memory; the scan proves that no
file under the output or a --scan root holds one. --module runs another copy of the directory (a RED on a build
from before a fix); nothing else differs between the two runs.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import importlib.util
import json
import re
import sys
import time
import urllib.request
from pathlib import Path
from urllib.error import HTTPError

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from relay_secrets import SecretBook, read_turn_config  # noqa: E402

MST = dt.timezone(dt.timedelta(hours=-7))
INSTALL_KEY = 'relay-cloudflare-mint-proof'
REGISTER = dict(connection_protocol=1, name='relay mint proof', activity='relay proof', scene='none', mode='pvp', peer_count=2, seats_free=1,
                game_version='7.0.0', build_id='relay-proof', network_protocol_version=1, lockstep_codec_version=1,
                controller_frame_version=1, match_config_hash='a' * 64, session_identity_hash='b' * 64,
                module_manifest_hash='c' * 64, listen_port=41010, listen_addrs=['127.0.0.1'], join_mode='ice')


def stamp() -> str:
    return dt.datetime.now(MST).strftime('%Y-%m-%d %H:%M:%S MST')


def load_directory(path: Path | None):
    if path is None:
        from session_directory import session_directory as module
        return module
    spec = importlib.util.spec_from_file_location('session_directory_under_test', path)
    if spec is None or spec.loader is None:
        raise SystemExit(f'{path}: not a Python module')
    module = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(path.parent))
    try:
        spec.loader.exec_module(module)
    finally:
        sys.path.pop(0)
    return module


def record_provider(module, calls: list[dict]) -> None:
    """Cloudflare's own answer to each request the directory sends: the status, and its error code on a refusal."""
    real = module.urlopen

    def call(request, *args, **kwargs):
        try:
            response = real(request, *args, **kwargs)
        except HTTPError as error:
            text = error.read(128).decode('ascii', 'replace')
            code = re.search(r'error code:\s*(\d+)', text)
            calls.append({'status': error.code, 'provider_error_code': code[1] if code else None,
                          'user_agent': request.get_header('User-agent')})
            raise
        calls.append({'status': response.status, 'user_agent': request.get_header('User-agent')})
        return response
    module.urlopen = call


def book_mints(store, book: SecretBook) -> None:
    real = store.turn_provider.mint

    def mint(*args, **kwargs):
        offer = real(*args, **kwargs)
        book.add_offer(offer)
        return offer
    store.turn_provider.mint = mint


def http(port: int, method: str, path: str, body: dict | None = None) -> tuple[int, dict]:
    data = json.dumps(body).encode() if body is not None else None
    request = urllib.request.Request(f'http://127.0.0.1:{port}{path}', data=data, method=method,
                                     headers={'X-Install-Key': INSTALL_KEY, 'Content-Type': 'application/json'})
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return response.status, json.loads(response.read() or b'{}')
    except HTTPError as error:
        try:
            return error.code, json.loads(error.read() or b'{}')
        except ValueError:
            return error.code, {}


def public_offer(offer: dict, wall: int) -> dict:
    servers = offer.get('iceServers', [])
    return {'ttl_granted_s': offer.get('expires_at', 0) - wall, 'server_count': len(servers),
            'urls': [url for server in servers for url in server.get('urls', [])],
            'servers_with_login': sum(bool(server.get('username')) and bool(server.get('credential')) for server in servers)}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--turn-config', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--module', type=Path)
    parser.add_argument('--ttl', type=int, default=600)
    parser.add_argument('--scan', type=Path, action='append', default=[])
    options = parser.parse_args(argv)
    out = options.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    module = load_directory(options.module)
    config = read_turn_config(options.turn_config)
    book = SecretBook()
    book.add_turn_config(config)
    calls: list[dict] = []
    record_provider(module, calls)
    server = module.spawn_server(port=0, insecure_http=True, log_file=out / 'service.log', turn_config=config, create_owner_key=True, caller_mode="direct")
    book_mints(server.store, book)
    source = Path(str(module.__file__)).resolve()
    receipt = {'schema': 1, 'check': 'cloudflare-mint', 'started': stamp(), 'turn_config': str(options.turn_config),
               'backend': config.get('backend'), 'module': str(source), 'module_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
               'directory_user_agent': getattr(module, 'USER_AGENT', None), 'ttl_requested_s': options.ttl}
    try:
        status, row = http(server.port, 'POST', '/v1/sessions', REGISTER)
        receipt['register'] = status
        if status == 200:
            session, token = row['session_id'], row['token']
            book.add('directory-session-token', token)
            wall = int(time.time())
            status, offer = http(server.port, 'POST', f'/v1/sessions/{session}/ice-servers',
                                 {'token': token, 'match_id': f'{session}:1', 'ttl': options.ttl})
            receipt['mint'] = {'status': status, **({'error': offer.get('error')} if status != 200 else public_offer(offer, wall))}
            status, fetched = http(server.port, 'GET', f'/v1/sessions/{session}/ice-servers')
            receipt['fetch'] = {'status': status, **({'error': fetched.get('error')} if status != 200 else {'same_offer': fetched == offer})}
    finally:
        server.stop()
    receipt['provider_calls'] = calls
    issued = [line.split('relay_offer_issued ', 1)[1] for line in (out / 'service.log').read_text(encoding='utf-8').splitlines()
              if 'relay_offer_issued ' in line] if (out / 'service.log').is_file() else []
    receipt['relay_offer_issued'] = [json.loads(line) for line in issued]
    receipt['finished'] = stamp()
    mint = receipt.get('mint', {})
    receipt['passed'] = bool(calls and calls[-1]['status'] == 201 and mint.get('status') == 200 and mint.get('servers_with_login', 0) >= 1
                             and receipt.get('fetch', {}).get('same_offer') and receipt['relay_offer_issued'])
    (out / 'mint-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    scan = book.scan([out, *options.scan])
    (out / 'secret-scan.json').write_text(json.dumps(scan, indent=2) + '\n', encoding='utf-8')
    print(f"[cloudflare-mint] {'PASS' if receipt['passed'] and scan['clean'] else 'FAIL'} provider={[c['status'] for c in calls]} "
          f"cloudflare_error={[c.get('provider_error_code') for c in calls]} mint={mint.get('status')} ttl={mint.get('ttl_granted_s')} "
          f"servers={mint.get('server_count')} fetch={receipt.get('fetch', {}).get('status')} "
          f"scan: {scan['secrets']} secrets ({', '.join(scan['kinds'])}) in {scan['files_scanned']} files, "
          f"{len(scan['files_with_secrets'])} files hold one")
    return 0 if receipt['passed'] and scan['clean'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
