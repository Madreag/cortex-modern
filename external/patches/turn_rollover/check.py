"""Detect the short mint and expired allocation on the old library and the fix."""
import argparse
import importlib.util
import json
from pathlib import Path
import sys
import threading
import time
from types import SimpleNamespace
from fixture import TurnFixture


def directory_check(source, dependencies):
    sys.path.insert(0, str(dependencies))
    spec = importlib.util.spec_from_file_location('directory_probe', source)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    module.LOGGER.disabled = True
    store = module.SessionDirectory.__new__(module.SessionDirectory)
    store._lock = threading.RLock()
    session = SimpleNamespace(ice_offer={'expires_at': int(time.time()) + 21, 'iceServers': []}, ice_fixed=False)
    store._signalling = lambda *args: session
    store.connection_bootstrap = lambda *args: {}
    store.connections = SimpleNamespace(check_in=lambda *args: ({'seat': 1, 'generation': 1}, True, None), set_relay=lambda *args: True)
    store.turn_limiter = SimpleNamespace(probe=lambda *args: None, commit=lambda *args: None)
    store.turn_max_ttl = module.TURN_MAX_TTL
    ttls = []

    def mint(match, ttl, now):
        ttls.append(ttl)
        return dict(match_id=match, expires_at=now + ttl, iceServers=[])
    store.turn_provider = SimpleNamespace(mint=mint)
    request = dict(operation='check-in', connection_protocol=module.CONNECTION_PROTOCOL)
    store.connection_request('probe', request, time.monotonic(), 'probe')
    session.ice_fixed = True
    expiry = session.ice_offer['expires_at']
    fixed = store.connection_request('probe', request, time.monotonic(), 'probe')['relay']['expires_at'] == expiry
    session.ice_offer['expires_at'] = int(time.time()) - 1
    expired = store.connection_request('probe', request, time.monotonic(), 'probe').get('relay_error') == 'relay_credential_expired'
    return dict(ttl_s=ttls[0], fixed_expiry_preserved=fixed, expired_fixed_refused=expired, passed=ttls[0] >= 3600 and fixed and expired)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--directory-source', type=Path, required=True)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--address', required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--label', required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(args.repo / 'tools'))
    import posix_test_runner as runner
    directory = directory_check(args.directory_source, args.repo / 'tools/session_directory')
    with (args.out / 'turn.jsonl').open('w', encoding='utf-8') as log:
        fixture = TurnFixture(args.address, log)
        first, second = args.out / 'first.private', args.out / 'second.private'
        fixture.login(35, first)
        fixture.login(150, second)
        fixture.thread.start()
        try:
            record = runner.run([str(args.binary), fixture.uri, str(first), str(second)], cwd=args.out, out=args.out / 'native', timeout=115, env={'CCCP_HEADLESS': '1'}, startup_checks=False)
        finally:
            fixture.close()
    output = (args.out / 'native/stdout.log').read_text(encoding='utf-8', errors='replace')
    network = '[turn-check] PASS' in output
    result = dict(label=args.label, passed=directory['passed'] and network and not fixture.errors, directory=directory, network=network, turn=fixture.counts, errors=fixture.errors)
    (args.out / 'result.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(f"[relay-expiry-check] {'GREEN' if result['passed'] else 'RED'} {args.label} ttl_s={directory['ttl_s']} network={int(network)} wrong_credentials={fixture.counts['wrong_credentials']} expired_credentials={fixture.counts['expired_credentials']}", flush=True)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
