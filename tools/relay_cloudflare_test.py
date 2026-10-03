"""Unit rows of the Cloudflare relay proof: the directory's request (CF1), the match evidence (CF2, the coturn
alternative, the three-way table) and the hotspot rows' verdicts and tunnel receipt (C4). No engine, no network."""
from __future__ import annotations

import io
from email.message import Message
import json
import re
import sys
import unittest
from pathlib import Path
from unittest import mock
from urllib.error import HTTPError

sys.path.insert(0, str(Path(__file__).resolve().parent))
from session_directory import session_directory as directory  # noqa: E402

CONFIG = {'backend': 'cloudflare', 'turn_key_id': 'unit-key-id-0001', 'api_token': 'unit-api-token-never-sent'}
SERVERS = [{'urls': ['stun:stun.cloudflare.com:3478']},
           {'urls': ['turn:turn.cloudflare.com:3478?transport=udp', 'turn:turn.cloudflare.com:3478?transport=tcp',
                     'turns:turn.cloudflare.com:5349?transport=tcp'],
            'username': 'unit-minted-username', 'credential': 'unit-minted-credential'}]


def answer(status: int = 201, body: dict | None = None) -> mock.MagicMock:
    response = mock.MagicMock()
    response.__enter__.return_value = response
    response.status = status
    response.read.return_value = json.dumps(body if body is not None else {'iceServers': SERVERS}).encode()
    return response


class CloudflareRequest(unittest.TestCase):
    """CF1: Cloudflare refuses urllib's default agent with 403 error 1010, so every real mint failed."""

    def mint(self, ttl: int = 600):
        provider = directory.TurnCredentialProvider(CONFIG)
        with mock.patch.object(directory, 'urlopen', return_value=answer()) as fetch:
            offer = provider.mint('match:1', ttl, 1000)
        return offer, fetch.call_args

    def test_the_request_names_the_product_and_its_version(self):
        _, call = self.mint()
        agent = call.args[0].get_header('User-agent')
        self.assertIsNotNone(agent, 'the Cloudflare request carries no User-Agent; urllib sends its default, which Cloudflare refuses')
        self.assertRegex(agent, r'^cccp-session-directory/\d+(\.\d+)*$')
        self.assertNotIn('urllib', agent.lower())

    def test_the_request_has_cloudflare_s_documented_shape_and_nothing_else(self):
        _, call = self.mint(900)
        request = call.args[0]
        self.assertEqual(request.get_method(), 'POST')
        self.assertEqual(request.full_url, 'https://rtc.live.cloudflare.com/v1/turn/keys/unit-key-id-0001/credentials/generate-ice-servers')
        self.assertEqual({name.lower() for name, _ in request.header_items()}, {'authorization', 'content-type', 'user-agent'})
        self.assertEqual(request.get_header('Authorization'), 'Bearer unit-api-token-never-sent')
        self.assertEqual(request.get_header('Content-type'), 'application/json')
        self.assertEqual(json.loads(request.data), {'ttl': 900})
        timeout = call.kwargs.get('timeout', call.args[1] if len(call.args) > 1 else None)
        self.assertIsNotNone(timeout, 'an unbounded provider call would hold the directory thread')

    def test_cloudflare_s_201_becomes_the_offer_with_its_lifetime(self):
        offer, _ = self.mint(900)
        self.assertEqual(offer['expires_at'], 1900)
        self.assertEqual([server['urls'] for server in offer['iceServers']], [server['urls'] for server in SERVERS])

    def test_a_refusal_is_sanitized_and_names_no_key(self):
        provider = directory.TurnCredentialProvider(CONFIG)
        refused = HTTPError('https://rtc.live.cloudflare.com/', 403, 'Forbidden', Message(), io.BytesIO(b'error code: 1010'))
        with mock.patch.object(directory, 'urlopen', side_effect=refused):
            with self.assertRaises(directory.TurnError) as error:
                provider.mint('match:1', 600, 1000)
        self.assertEqual((error.exception.status, error.exception.body), (502, {'error': 'relay_provider_unavailable'}))
        for secret in (CONFIG['api_token'], CONFIG['turn_key_id']):
            self.assertNotIn(secret, json.dumps(error.exception.body) + str(error.exception))


def signal(*candidates: str) -> str:
    """A rendezvous blob as the directory carries it: base64 of bytes with the candidate lines inside."""
    import base64
    body = b'\x0a\x08identity' + b''.join(b'\x1a' + bytes([len(text)]) + text.encode() for text in candidates)
    return base64.b64encode(body).decode()


def relay_line(address: str, port: int = 40001) -> str:
    return f'candidate:1 1 udp 16777215 {address} {port} typ relay raddr 0.0.0.0 rport 0'


def receipts(session: str, route: str = 'relay', allowed: int = 1, connection: int = 7) -> str:
    return '\n'.join([f'[net-ice] session {session} join_mode=ice resolved to identity x; dialling the ICE half',
                      f'[net-ice] selected candidate={route} connection={connection}',
                      f'[net-route] RouteAllowed route={route} allowed={allowed} connection={connection}'])


def cloudflare_run(**changes):
    session = 'session-one'
    run = dict(mode='cloudflare', session_id=session,
               connection={'host': 'RelayOnly', 'client': 'RelayOnly'},
               logs={'host': receipts(session), 'client': receipts(session)},
               signals=[('host', signal(relay_line('141.101.90.17'))), ('client-nonce', signal(relay_line('162.159.207.9', 40002)))],
               offers=[dict(session_id=session, match_id=f'{session}:1', provider='cloudflare', generation=1, expires_at=2000, server_count=2)],
               offer_urls=['stun:stun.cloudflare.com:3478', 'turn:turn.cloudflare.com:3478?transport=udp', 'turns:turn.cloudflare.com:443?transport=tcp'],
               client_connection={'relayed': True, 'remote_address': '141.101.90.17:40001'},
               relay_addresses=None)
    run.update(changes)
    return run


class CloudflareMatchEvidence(unittest.TestCase):
    """CF2: the relay is proved by the transport's own route receipts and the candidates each peer sent, never a label."""

    def judge(self, run):
        import relay_cloudflare_match as match
        return match.judge_relay(run)

    def test_a_relayed_match_through_cloudflare_on_both_forced_peers_passes(self):
        verdict = self.judge(cloudflare_run())
        self.assertTrue(verdict['passed'], verdict['reasons'])
        self.assertEqual(sorted(verdict['peers']), ['client', 'host'])

    def test_a_direct_route_on_a_relay_only_peer_fails(self):
        run = cloudflare_run()
        run['logs']['client'] = receipts('session-one', route='direct')
        self.assertFalse(self.judge(run)['passed'])

    def test_a_route_receipt_from_another_session_fails(self):
        run = cloudflare_run()
        run['logs']['host'] = receipts('session-other')
        self.assertFalse(self.judge(run)['passed'])

    def test_a_refused_relay_route_fails(self):
        run = cloudflare_run()
        run['logs']['host'] = receipts('session-one', allowed=0)
        self.assertFalse(self.judge(run)['passed'])

    def test_a_relay_outside_cloudflare_fails(self):
        run = cloudflare_run(signals=[('host', signal(relay_line('68.3.162.151'))), ('client-nonce', signal(relay_line('162.159.207.9')))])
        verdict = self.judge(run)
        self.assertFalse(verdict['passed'])
        self.assertTrue(any('68.3.162.151' in reason for reason in verdict['reasons']), verdict['reasons'])

    def test_a_peer_that_sent_no_relay_candidate_fails(self):
        run = cloudflare_run(signals=[('host', signal(relay_line('141.101.90.17')))])
        self.assertFalse(self.judge(run)['passed'])

    def test_a_relay_only_peer_that_offered_a_direct_candidate_fails(self):
        direct = 'candidate:2 1 udp 2130706431 192.168.50.44 51000 typ host'
        run = cloudflare_run(signals=[('host', signal(relay_line('141.101.90.17'), direct)), ('client-nonce', signal(relay_line('162.159.207.9')))])
        self.assertFalse(self.judge(run)['passed'])

    def test_an_offer_for_another_session_or_provider_fails(self):
        for offer in (dict(session_id='session-other', match_id='m', provider='cloudflare', generation=1, expires_at=2000, server_count=2),
                      dict(session_id='session-one', match_id='m', provider='fixed', generation=1, expires_at=2000, server_count=1)):
            with self.subTest(offer=offer):
                self.assertFalse(self.judge(cloudflare_run(offers=[offer]))['passed'])

    def test_an_offer_naming_another_relay_server_fails(self):
        run = cloudflare_run(offer_urls=['turn:turn.cloudflare.com:3478?transport=udp', 'turn:relay.example.net:3478?transport=udp'])
        self.assertFalse(self.judge(run)['passed'])

    def test_the_client_report_must_agree_the_connection_is_relayed(self):
        self.assertFalse(self.judge(cloudflare_run(client_connection={'relayed': False, 'remote_address': '24.251.145.96:5000'}))['passed'])

    def test_our_relay_passes_only_with_its_own_addresses_and_a_fixed_offer(self):
        coturn = dict(mode='coturn', relay_addresses=['192.168.50.122', '68.3.162.151'],
                      signals=[('host', signal(relay_line('192.168.50.122', 49201))), ('client-nonce', signal(relay_line('192.168.50.122', 49202)))],
                      offers=[dict(session_id='session-one', match_id='m', provider='fixed', generation=1, expires_at=2000, server_count=1)],
                      offer_urls=['turn:68.3.162.151:3479?transport=udp'], client_connection={'relayed': True, 'remote_address': '192.168.50.122:49201'})
        self.assertTrue(self.judge(cloudflare_run(**coturn))['passed'])
        through_cloudflare = dict(coturn, signals=cloudflare_run()['signals'])
        self.assertFalse(self.judge(cloudflare_run(**through_cloudflare))['passed'])

    def test_automatic_records_the_route_and_whether_direct_was_chosen(self):
        session = 'session-one'
        run = cloudflare_run(mode='automatic', connection={'host': 'Automatic', 'client': 'Automatic'},
                             logs={'host': receipts(session, route='direct'), 'client': receipts(session, route='direct')},
                             signals=[('host', signal('candidate:2 1 udp 2130706431 24.251.145.96 51000 typ srflx'))], offers=[], offer_urls=[],
                             client_connection={'relayed': False, 'remote_address': '24.251.145.96:51000'})
        verdict = self.judge(run)
        self.assertTrue(verdict['passed'], verdict['reasons'])
        self.assertEqual(verdict['routes'], {'host': 'direct', 'client': 'direct'})
        self.assertTrue(verdict['direct_as_expected'])

    def test_the_rtt_comes_from_the_transport_s_own_line(self):
        import relay_cloudflare_match as match
        log = '[net-match] auto input delay: peer 2 rtt 31ms -> 4 frames (manual floor 2)\n[net-match] delay change peer=2 frame=900 delay=5 revision=3'
        self.assertEqual(match.transport_rtts(log), [{'peer': 2, 'rtt_ms': 31, 'delay_frames': 4}])
        self.assertEqual(match.delay_changes(log), [{'peer': 2, 'frame': 900, 'delay': 5}])

    def test_cloudflare_s_published_ranges_hold_its_turn_server_and_nothing_else(self):
        import relay_cloudflare_match as match
        self.assertTrue(match.cloudflare_address('141.101.90.1'))
        self.assertTrue(match.cloudflare_address('2a06:98c1:3200::1'))
        for other in ('68.3.162.151', '24.251.145.96', '192.168.50.122', '8.8.8.8', 'not-an-address'):
            self.assertFalse(match.cloudflare_address(other), other)


class SecretScan(unittest.TestCase):
    """A credential that appears in any file the lane writes fails the row; the scan never prints the value."""

    def test_a_minted_credential_in_a_file_is_found_by_kind_and_never_echoed(self):
        import tempfile
        from relay_secrets import SecretBook
        book = SecretBook()
        book.add_offer({'iceServers': SERVERS})
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'clean.log').write_text('route=relay\n', encoding='utf-8')
            (root / 'leak.ini').write_text('NetworkTurnPass = unit-minted-credential\n', encoding='utf-8')
            scan = book.scan([root])
        self.assertFalse(scan['clean'])
        self.assertEqual([(Path(row['path']).name, row['kinds']) for row in scan['files_with_secrets']], [('leak.ini', ['minted-credential'])])
        self.assertNotIn('unit-minted-credential', json.dumps(scan))

    def test_the_scan_never_enters_a_junction_or_symlink(self):
        import os
        import subprocess
        import tempfile
        from relay_secrets import SecretBook
        book = SecretBook()
        book.add('backend-api_token', 'outside-the-run-root-secret')
        with tempfile.TemporaryDirectory() as outside, tempfile.TemporaryDirectory() as folder:
            (Path(outside) / 'key.json').write_text('outside-the-run-root-secret', encoding='utf-8')
            link = Path(folder) / 'Data'
            try:
                os.symlink(outside, link, target_is_directory=True)
            except OSError:
                subprocess.run(['cmd', '/c', 'mklink', '/J', str(link), outside], capture_output=True, check=True)
            scan = book.scan([folder])
            os.rmdir(link)
        self.assertTrue(scan['clean'], scan)
        self.assertEqual(scan['files_scanned'], 0)


if __name__ == '__main__':
    unittest.main()
