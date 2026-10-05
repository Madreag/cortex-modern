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
               reports={'client': {'found': True, 'state': 3, 'relayed': True, 'remote_address': '141.101.90.17:40001'}},
               relay_addresses=None, overrides_cleared=True, run_ends_at=1500)
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
        self.assertFalse(self.judge(cloudflare_run(reports={'client': {'found': True, 'relayed': False, 'remote_address': '24.251.145.96:5000'}}))['passed'])

    def test_our_relay_passes_only_with_its_own_addresses_and_a_fixed_offer(self):
        coturn = dict(mode='coturn', relay_addresses=['192.168.50.122', '68.3.162.151'],
                      signals=[('host', signal(relay_line('192.168.50.122', 49201))), ('client-nonce', signal(relay_line('192.168.50.122', 49202)))],
                      offers=[dict(session_id='session-one', match_id='session-one:1', provider='coturn', generation=1, expires_at=2000, server_count=1)],
                      offer_urls=['turn:68.3.162.151:3479?transport=udp'],
                      reports={'client': {'found': True, 'state': 3, 'relayed': True, 'remote_address': '192.168.50.122:49201'}})
        self.assertTrue(self.judge(cloudflare_run(**coturn))['passed'])
        through_cloudflare = dict(coturn, signals=cloudflare_run()['signals'])
        self.assertFalse(self.judge(cloudflare_run(**through_cloudflare))['passed'])

    def test_automatic_records_the_route_and_whether_direct_was_chosen(self):
        session = 'session-one'
        run = cloudflare_run(mode='automatic', connection={'host': 'Automatic', 'client': 'Automatic'},
                             logs={'host': receipts(session, route='direct'), 'client': receipts(session, route='direct')},
                             signals=[('host', signal('candidate:2 1 udp 2130706431 24.251.145.96 51000 typ srflx'))],
                             reports={'client': {'found': True, 'state': 3, 'relayed': False, 'remote_address': '24.251.145.96:51000'}})
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
        # write_text writes CRLF here: the INI login shape reads that line as well as the book (it missed CRLF before).
        self.assertEqual([(Path(row['path']).name, row['kinds']) for row in scan['files_with_secrets']],
                         [('leak.ini', ['minted-credential', 'shape:ini-relay-login'])])
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


def tunnel_rows(**changes):
    at = [f'2026-10-03 02:00:{second:02d} PM MST' for second in (0, 1, 2, 40, 41)]
    rows = [dict(step='before', backend_state='Running'), dict(step='down', exit_code=0, backend_state='Stopped'),
            dict(step='engine-started', peer='client', backend_state='Stopped'), dict(step='engine-ended', peer='client', backend_state='Stopped'),
            dict(step='up', exit_code=0, backend_state='Running')]
    for row, stamp in zip(rows, at):
        row['at'] = stamp
    for index, change in changes.items():
        rows[int(index[1:])].update(change)
    return rows


class HotspotRows(unittest.TestCase):
    """C4: the six hotspot rows' verdicts, each red on its own defect before any hotspot run exists."""

    def match(self):
        import relay_cloudflare_match as match
        return match

    def test_the_tunnel_receipt_passes_only_when_tailscale_was_down_for_the_whole_match(self):
        judge = self.match().tunnel_receipt
        self.assertTrue(judge(tunnel_rows())['passed'], judge(tunnel_rows())['reasons'])
        for name, rows in {'up while the engine ran': tunnel_rows(s2={'backend_state': 'Running'}),
                           'up when the engine ended': tunnel_rows(s3={'backend_state': 'Running'}),
                           'the down command failed': tunnel_rows(s1={'exit_code': 1}),
                           'never brought back up': tunnel_rows()[:4],
                           'came back without its login': tunnel_rows(s4={'backend_state': 'NeedsLogin'}),
                           'the engine started before the toggle': [tunnel_rows()[0], tunnel_rows()[2], tunnel_rows()[1], *tunnel_rows()[3:]],
                           'no engine row at all': [tunnel_rows()[0], tunnel_rows()[1], tunnel_rows()[4]]}.items():
            with self.subTest(name):
                self.assertFalse(judge(rows)['passed'])

    def test_a_forced_fallback_needs_the_relay_on_the_hotspot_peer_with_no_player_action(self):
        session = 'session-one'
        srflx = 'candidate:2 1 udp 1694498815 172.58.1.2 51000 typ srflx'
        signals = [('host', signal(relay_line('141.101.90.17'), srflx)), ('client-nonce', signal(relay_line('162.159.207.9', 40002), srflx))]
        run = cloudflare_run(mode='automatic', provider='cloudflare', expect_routes={'client': 'relay'}, signals=signals,
                             connection={'host': 'Automatic', 'client': 'Automatic'},
                             logs={'host': receipts(session), 'client': receipts(session)})
        verdict = self.match().judge_relay(run)
        self.assertTrue(verdict['passed'], verdict['reasons'])
        run['logs']['client'] = receipts(session, route='direct')
        run['logs']['host'] = receipts(session, route='direct')
        run['reports'] = {'client': {'found': True, 'state': 3, 'relayed': False, 'remote_address': '24.251.145.96:5000'}}
        verdict = self.match().judge_relay(run)
        self.assertFalse(verdict['passed'])
        self.assertTrue(any('fallback' in reason for reason in verdict['reasons']), verdict['reasons'])

    def test_a_the_connection_panel_must_say_the_route(self):
        panel = self.match().panel_verdict
        self.assertTrue(panel({'pass': True, 'complete': True})['passed'])
        for result in ({'pass': False, 'complete': True}, {'pass': True, 'complete': False}, {}, None):
            with self.subTest(result=result):
                self.assertFalse(panel(result)['passed'])
        watching = {'pass': True, 'complete': True, 'script': {'steps': [{'op': 'watch_route'}]}}
        self.assertFalse(panel(watching)['passed'])
        self.assertTrue(panel(dict(watching, route_watch={'samples': 900, 'moves': []}))['passed'])

    def test_a_every_move_of_the_route_has_its_receipt(self):
        """2026-10-04's row a: the connection moved from the relay to direct within 4 s and its log kept the one relay receipt."""
        history = self.match().route_history
        opening = '[net-ice] session s join_mode=ice resolved to identity x; dialling the ICE half\n'
        relay = ('[net-ice] selected candidate=relay connection=7\n[net-route] RouteAllowed route=relay allowed=1 connection=7 remote=none '
                 'turn=turn.cloudflare.com:3478 offer=s:0@2000\n')
        moved = ('[net-ice] selected candidate=srflx connection=7\n[net-route] RouteAllowed route=direct allowed=1 connection=7 '
                 'remote=24.251.145.96:5000 offer=none change=relay->direct after_ms=4100\n')
        direct_report = {'found': True, 'relayed': False}
        verdict = history(opening + relay + moved, 's', direct_report, None)
        self.assertTrue(verdict['passed'], verdict['reasons'])
        self.assertEqual(verdict['moves'], ['relay->direct'])
        self.assertFalse(history(opening + relay, 's', direct_report, None)['passed'])
        self.assertFalse(history(opening + relay + moved.replace('change=relay->direct', 'change=direct->relay'), 's', direct_report, None)['passed'])
        self.assertFalse(history(opening + relay.replace('offer=s:0@2000', 'offer=s:0@2000 change=direct->relay'), 's', {'found': True, 'relayed': True}, None)['passed'])
        seen = {'moves': [{'what': 'live', 'from': 'relay', 'to': 'direct'}, {'what': 'live', 'from': 'direct', 'to': 'relay'}]}
        self.assertFalse(history(opening + relay + moved, 's', direct_report, seen)['passed'])
        self.assertFalse(history('', 's', direct_report, None)['passed'])

    def test_a_public_relay_is_bound_by_its_own_receipt_and_the_directory_s_offer(self):
        """The public directory is not the run's: no candidate is observable, so the engine's receipt names the relay and its offer."""
        session = 'session-one'
        line = ('[net-route] RouteAllowed route=relay allowed=1 connection=7 remote=none turn=turn.cloudflare.com:3478,turn.cloudflare.com:443 '
                f'offer={session}:0@2000')
        log = '\n'.join([f'[net-ice] session {session} join_mode=ice resolved to identity x; dialling the ICE half',
                          '[net-ice] selected candidate=relay connection=7', line])
        offers = [dict(session_id=session, match_id=f'{session}:0', provider='cloudflare', generation=1, expires_at=2000, server_count=2)]
        run = cloudflare_run(logs={'host': log, 'client': log}, signals=[], offers=offers, offer_urls=None, signals_observed=False,
                             reports={'client': {'found': True, 'state': 3, 'relayed': True, 'remote_address': ''}})
        verdict = self.match().judge_relay(run)
        self.assertTrue(verdict['passed'], verdict['reasons'])
        self.assertTrue(verdict['bindings']['client'].startswith('by its route receipt'))
        for name, changed in {'another offer': line.replace('@2000', '@1999'), 'another relay': line.replace('turn.cloudflare.com:443', 'relay.example.net:443'),
                              'no relay named': line.split(' turn=')[0] + f' offer={session}:0@2000'}.items():
            with self.subTest(name):
                swapped = log.replace(line, changed)
                self.assertFalse(self.match().judge_relay(dict(run, logs={'host': swapped, 'client': swapped}))['passed'])
        self.assertFalse(self.match().judge_relay(dict(run, signals_observed=True))['passed'])

    def test_every_box_holds_the_same_game_data_before_an_engine_starts(self):
        """2026-10-04: six Data text files with CRLF on two boxes were refused as 'modules' by the directory after a 10-minute wait."""
        import tempfile
        match = self.match()

        class Here:
            def __init__(self, name, tree):
                self.name, self.tree, self.local = name, tree, True
        with tempfile.TemporaryDirectory() as folder, mock.patch.object(match, 'DRY_RUN', False):
            trees = []
            for box in ('edith', 'ally'):
                module = Path(folder) / box / 'Data' / 'Base.rte' / 'GUIs'
                module.mkdir(parents=True)
                (module / 'MainMenuSubMenuGUI.ini').write_bytes(b'[Panel]\nWidth = 2\n')
                (Path(folder) / box / 'Data' / 'Tests.rte').mkdir()
                (Path(folder) / box / 'Data' / 'Tests.rte' / 'Preview.lua').write_bytes(b'return 1\n')
                trees.append(Here(box, str(Path(folder) / box)))
            self.assertTrue(match.data_preflight(trees)['passed'])
            (Path(folder) / 'ally' / 'Data' / 'Base.rte' / 'GUIs' / 'MainMenuSubMenuGUI.ini').write_bytes(b'[Panel]\r\nWidth = 2\r\n')
            verdict = match.data_preflight(trees)
            self.assertFalse(verdict['passed'])
            self.assertEqual(len(verdict['reasons']), 1, verdict['reasons'])
            self.assertIn('ally: module Data/Base.rte differs', verdict['reasons'][0])
            self.assertIn('GUIs/MainMenuSubMenuGUI.ini', verdict['reasons'][0])

    def test_b_the_hotspot_host_is_listed_and_given_a_cloudflare_relay(self):
        listing = self.match().listing_evidence
        lines = ['2026-10-03 INFO 1.2.3.4 "POST /v1/sessions HTTP/1.1" 200 -',
                 'INFO register session_id=session-one name=relayproof-b',
                 'INFO relay_offer_issued ' + json.dumps(dict(session_id='session-one', match_id='session-one:1', provider='cloudflare',
                                                              generation=1, expires_at=2000, server_count=2))]
        self.assertTrue(listing(lines, 'session-one', listed=True)['passed'])
        self.assertFalse(listing(lines, 'session-one', listed=False)['passed'])
        self.assertFalse(listing(lines[:2], 'session-one', listed=True)['passed'])
        self.assertFalse(listing(lines, 'session-two', listed=True)['passed'])

    def test_c_four_players_only_the_hotspot_seat_may_be_held(self):
        holds = self.match().seat_holds
        peers = [dict(name='host', holds=0), dict(name='client', holds=0), dict(name='client2', holds=0), dict(name='hotspot', holds=2)]
        self.assertTrue(holds(peers, allowed={'hotspot'})['passed'])
        peers[1]['holds'] = 1
        self.assertFalse(holds(peers, allowed={'hotspot'})['passed'])
        self.assertFalse(holds([dict(name='host', holds=0)], allowed={'hotspot'}, expected={'host', 'client', 'client2', 'hotspot'})['passed'])

    def test_d_an_expiring_credential_is_renewed_on_the_live_connection(self):
        renewal = self.match().renewal_evidence
        renewed = '[net-relay] relay login renewed on 1 live connection(s)'
        route = '[net-ice] session s\n[net-ice] selected candidate=relay connection=7\n[net-route] RouteAllowed route=relay allowed=1 connection=7\n'
        logs = {'host': route + renewed, 'client': route + renewed}
        calls = [dict(status=201, epoch=1000.0), dict(status=201, epoch=1160.0)]
        timed = dict(line_times={peer: [[1170.0, renewed]] for peer in logs}, samples={peer: [[1010.0, 600], [1320.0, 19000]] for peer in logs},
                     first_expiry=1300.0, clocks={peer: (0.0, 0.5) for peer in logs}, session='s', ttl_s=300)
        self.assertTrue(renewal(logs, calls, ['host', 'client'], **timed)['passed'], renewal(logs, calls, ['host', 'client'], **timed)['reasons'])
        self.assertFalse(renewal({'host': route + renewed, 'client': route}, calls, ['host', 'client'],
                                 **dict(timed, line_times={'host': [[1170.0, renewed]], 'client': []}))['passed'])
        self.assertFalse(renewal(logs, calls[:1], ['host', 'client'], **timed)['passed'])
        self.assertFalse(renewal(logs, [dict(status=201, epoch=1000.0), dict(status=403, epoch=1160.0, provider_error_code='1010')], ['host', 'client'], **timed)['passed'])
        self.assertFalse(renewal(logs, calls, ['host', 'client'], **dict(timed, line_times={}))['passed'])

    def test_e_the_survivors_feel_is_judged_on_either_side_of_the_loss(self):
        """2026-10-04 row e: the whole-match bars read 47.5 tps and a 7.5 s wait, both the host loss's own pause."""
        around = self.match().feel_around_loss
        tick = 1000 / 60
        rows = [dict(tick=t, wall_ms=t * tick + (7500 if t >= 618 else 0)) for t in range(1, 1802)]
        verdict = around(rows, '', 618, 1801)
        self.assertTrue(verdict['passed'], verdict['reasons'])
        self.assertGreater(verdict['pause_ms'], 7500)
        slow = [dict(row, wall_ms=row['wall_ms'] + (row['tick'] - 1000) * 2 if row['tick'] > 1000 else row['wall_ms']) for row in rows]
        self.assertFalse(around(slow, '', 618, 1801)['passed'])
        waited = around(rows, '[net-frame-wait] frame=1200 wait_ms=80 on=x\n', 618, 1801)
        self.assertFalse(waited['passed'])
        self.assertFalse(around(rows, '', None, 1801)['passed'])

    def test_e_a_survivor_s_second_dial_is_its_own(self):
        """2026-10-04 row e: a survivor dials its successor as a second identity; its connect receipt names it."""
        senders = self.match().sender_peers
        signals = [('host', 'x'), ('client:aaaa1111bbbb2222cccc', 'x'), ('client:dddd3333eeee4444ffff', 'x')]
        mapped = senders(signals, {'client': ['str:c-aaaa1111bbbb2222', 'str:c-dddd3333eeee4444']}, ['host', 'client'])
        self.assertEqual(mapped['client:dddd3333eeee4444ffff'], 'client')
        mapped = senders(signals, {'client': 'str:c-aaaa1111bbbb2222', 'client2': 'str:c-9999'}, ['host', 'client', 'client2'])
        self.assertTrue(mapped['client:dddd3333eeee4444ffff'].startswith('unmatched:'))
        run = cloudflare_run(reports={'client': {'found': False, 'state': 5, 'relayed': True}})
        self.assertFalse(self.match().judge_relay(run)['passed'])
        self.assertTrue(self.match().judge_relay(dict(run, host_lost=True))['passed'])
        # 2026-10-04 8:02 PM: a survivor's closed report says nothing of the route it plays on after the loss.
        closed = cloudflare_run(reports={'client': {'found': False, 'state': 0, 'relayed': False}})
        verdict = self.match().judge_relay(dict(closed, host_lost=True))
        self.assertTrue(verdict['passed'], verdict['reasons'])

    def test_e_a_cut_registry_lookup_is_asked_again_or_read_from_its_block(self):
        """2026-10-04 9:37 PM row e: two relay addresses inside a block the run had resolved met 'connection forcibly closed'."""
        calls = []
        block = dict(handle='NET-104-16-0-0-1', name='CLOUDFLARENET', start='104.16.0.0', end='104.31.255.255',
                     registrants=['Cloudflare, Inc.'], inside=True, cloudflare=True)

        def lookup(address):
            calls.append(address)
            if address == '104.30.146.20' or calls.count(address) > 1:
                return dict(block, address=address) if address.startswith('104.') else dict(address=address, start='198.51.100.0',
                                                                                            end='198.51.100.255', cloudflare=False)
            return dict(address=address, cloudflare=False, error='URLError: [WinError 10054] forcibly closed')
        rows = self.match().registrants_of(['104.30.146.20', '104.30.150.12', '198.51.100.7'], lookup=lookup, pause_s=0)
        self.assertEqual([row['cloudflare'] for row in rows], [True, True, False])
        self.assertEqual(rows[1]['within'], '104.30.146.20')
        self.assertEqual(calls, ['104.30.146.20', '198.51.100.7', '198.51.100.7'])
        self.assertNotIn('error', rows[2])

    def test_e_a_killed_host_s_summary_is_its_successor_s(self):
        """2026-10-04 8:02 PM row e: the killed host writes no report; the seats are read from the successor's summary."""
        writers = self.match().summary_writers
        run = {'tag': 'rp', 'kill_host_at_tick': 620, 'peers': [{'name': 'host'}, {'name': 'client'}, {'name': 'client2'}]}
        logs = {'client': '[net-match] Host left - rp-client is now hosting; boundary=630 round=9\n', 'client2': ''}
        self.assertEqual(writers(run, logs), ['host', 'client'])
        self.assertEqual(writers(dict(run, kill_host_at_tick=None), logs), ['host'])

    def test_d_a_login_that_expired_before_the_run_ended_is_already_revoked(self):
        """2026-10-04 row d: the first of three five-minute logins expired mid-match; Cloudflare answered its revoke 404."""
        revoked = self.match().revoked_every_login
        minted = [dict(username='one', expires_at=1000), dict(username='two', expires_at=1300), dict(username='three', expires_at=1600)]
        self.assertTrue(revoked(minted, [404, 204, 204], 1500))
        self.assertFalse(revoked(minted, [204, 404, 204], 1200))
        self.assertFalse(revoked(minted, [204, 204], 1500))
        self.assertFalse(revoked(minted, [], 1500))
        self.assertTrue(revoked(minted + [dict(username='one', expires_at=1000)], [204, 204, 204], 1500))

    def test_e_the_survivors_name_one_successor_after_the_relayed_host_is_lost(self):
        migration = self.match().migration_declarations
        line = '[net-match] Host left - Client is now hosting; boundary=640 round=1'
        successor = '\n[net-ice] selected candidate=relay connection=9\n[net-route] RouteAllowed route=relay allowed=1 connection=9'
        seats, ticks = {'host': 'Host', 'client': 'Client', 'client2': 'Client2'}, {'client': 1801, 'client2': 1801}
        both = {'client': line + successor, 'client2': line + successor}
        self.assertEqual(migration(both, ['client', 'client2'], 's', seats, ticks)['boundary'], 640)
        self.assertTrue(migration(both, ['client', 'client2'], 's', seats, ticks)['passed'])
        other = '[net-match] Host left - Client2 is now hosting; boundary=640 round=1'
        self.assertFalse(migration({'client': line + successor, 'client2': other + successor}, ['client', 'client2'], 's', seats, ticks)['passed'])
        self.assertFalse(migration({'client': line + successor, 'client2': ''}, ['client', 'client2'], 's', seats, ticks)['passed'])
        self.assertFalse(migration({'client': line + '\n' + line + successor, 'client2': line + successor}, ['client', 'client2'], 's', seats, ticks)['passed'])
        # 2026-10-04 row e: each survivor dials its successor when it finds the host lost, before the handover line.
        lost = '[net-match] host lost; collecting surviving peers at applied frame 617 final_frame=1802'
        early = {name: lost + successor + '\n' + line for name in ('client', 'client2')}
        self.assertTrue(migration(early, ['client', 'client2'], 's', seats, ticks)['passed'])
        stale = {name: successor.lstrip('\n') + '\n' + lost + '\n' + line for name in ('client', 'client2')}
        self.assertFalse(migration(stale, ['client', 'client2'], 's', seats, ticks)['passed'])

    def test_f_relay_only_chosen_by_hand_is_read_from_the_menu_script(self):
        chosen = self.match().menu_choice
        log = '[menu-script] combo_select ComboNetworkConnection Relay only\n[menu-script] assert_label ComboNetworkConnection "Relay only" text="Relay only" PASS'
        self.assertTrue(chosen(log, 'Relay only')['passed'])
        self.assertFalse(chosen(log.replace('PASS', 'FAIL'), 'Relay only')['passed'])
        self.assertFalse(chosen('', 'Relay only')['passed'])

    def test_every_hotspot_item_reads_a_check_its_row_computes(self):
        scenario = json.loads((Path(__file__).resolve().parent / 'e2e/mp-relay-hotspot.json').read_text(encoding='utf-8'))
        required = self.match().REQUIRED
        judged_apart = ('offer', 'direct_expected')
        unread = [item['id'] for item in scenario['checklist'] if item['run'] in required and ':' not in item['check'] and
                  item['check'] not in judged_apart and item['check'] not in required[item['run']]]
        self.assertEqual(unread, [], 'items whose check their row never computes read as absent and fail every run')

    def test_a_busy_session_keeps_its_offer_in_the_directory_lines(self):
        match = self.match()
        session = '9dc65d53-88b9-48d8-bc3f-6efd89225510'
        offer = f'2026-10-05 04:43:30,000 INFO relay_offer_issued {{"session_id": "{session}", "provider": "cloudflare"}}'
        polls = [f'2026-10-05 04:45:41,408 INFO 127.0.0.1 "GET /v1/sessions/{session}/signals?peer=host&after={n}&wait=2 HTTP/1.1" 200 -'
                 for n in range(600)]
        lines = [offer, *polls, f'2026-10-05 04:48:10,000 INFO 127.0.0.1 "DELETE /v1/sessions/{session} HTTP/1.1" 204 -']

        def ssh(argv, **_):
            # The Mac's shell runs the command's own pipeline.
            tail = re.search(r'\| tail -(\d+)\s*$', argv[-1])
            return mock.Mock(stdout='\n'.join(lines[-int(tail.group(1)):] if tail else lines) + '\n')
        with mock.patch.object(match.subprocess, 'run', side_effect=ssh):
            got = match.public_directory_lines(session)
        self.assertIn(offer, got, 'a four-player session logs more polls than the window: its offer must stay')
        self.assertIn(lines[-1], got)
        self.assertLessEqual(len(got), 402)

    def test_every_hotspot_row_is_declared_with_its_lever(self):
        scenario = json.loads((Path(__file__).resolve().parent / 'e2e/mp-relay-hotspot.json').read_text(encoding='utf-8'))
        named = {run['name']: run for run in scenario['runs']}
        runs = {run['name'][0]: run for run in scenario['runs'] if run['name'] != 'a-automatic-forced'}
        self.assertEqual(sorted(runs), list('abcdefg'))
        self.assertEqual(sorted(named), ['a-automatic-fallback', 'a-automatic-forced', 'b-hotspot-host', 'c-four-players', 'd-credential-expiry',
                                         'e-migration-relayed', 'f-relay-by-hand', 'g-relay-only-public'])
        self.assertTrue(any(peer.get('tailscale_down') for peer in runs['a']['peers']))
        # On a carrier NAT that can be punched Automatic ends direct: the natural form proves its route history, the forced form the fallback.
        self.assertNotIn('expect_routes', runs['a'])
        forced = named['a-automatic-forced']
        self.assertEqual(forced['expect_routes'], {'client': 'relay'})
        hotspot = next(peer for peer in forced['peers'] if peer['box'] == 'ally')
        self.assertEqual(hotspot['env'], {'CC_TEST_ICE_GATHER_RELAY_ONLY': '1'})
        self.assertEqual(hotspot['connection'], 'Automatic')
        self.assertTrue(hotspot.get('tailscale_down'))
        self.assertEqual(next(peer for peer in runs['b']['peers'] if peer['name'] == 'host')['box'], 'ally')
        self.assertEqual(len(runs['c']['peers']), 4)
        self.assertEqual(sorted(peer['box'] for peer in runs['c']['peers']), ['ally', 'edith', 'erol-pc', 'erol-pc'])
        for row in 'def':
            self.assertEqual(runs[row]['directory'], 'tunnel', row)
        self.assertEqual(runs['g']['timing_peers'], ['host'])
        self.assertEqual(runs['g']['holds_allowed'], ['client'])
        self.assertLessEqual(runs['d']['relay_ttl_cap'], 600)
        self.assertGreater(runs['d']['ticks'] / 60, runs['d']['relay_ttl_cap'])
        self.assertTrue(runs['e']['kill_host_at_tick'])
        self.assertTrue(any(peer.get('menu_script') for peer in runs['f']['peers']))
        # Two Relay only players meet in the public directory, the hotspot box off the tailnet, and stay relayed.
        self.assertEqual(runs['g']['directory'], 'public')
        self.assertEqual({peer['connection'] for peer in runs['g']['peers']}, {'RelayOnly'})
        self.assertTrue(next(peer for peer in runs['g']['peers'] if peer['box'] == 'ally').get('tailscale_down'))
        self.assertEqual(runs['g']['expect_routes'], {'client': 'relay', 'host': 'relay'})
        for run in scenario['runs']:
            for peer in run['peers']:
                if peer['box'] == 'ally':
                    self.assertTrue(peer.get('hotspot'), run['name'])


class DirectoryPerRun(unittest.TestCase):
    """Two runs in one driver process: each run's offer receipt lands in its own service.log (a run's verdict reads only
    its own root, so a receipt logged into the previous run's file reads as 'no offer issued')."""

    def test_each_run_logs_its_offer_receipt_to_its_own_root(self):
        import tempfile
        import relay_cloudflare_match as match
        from relay_secrets import SecretBook
        fields = dict(name='unit', activity='t', scene='t', mode='pvp', peer_count=2, seats_free=1, game_version='1', build_id='a',
                      network_protocol_version=1, lockstep_codec_version=1, controller_frame_version=1, match_config_hash='a' * 64,
                      session_identity_hash='b' * 64, module_manifest_hash='c' * 64, listen_port=41010, listen_addrs=['127.0.0.1'],
                      join_mode='ice')
        fixed = [dict(urls=['turn:relay.example:3478?transport=udp'], username='unit-fixed-user', credential='unit-fixed-pass')]
        with tempfile.TemporaryDirectory() as folder:
            counts = []
            for index in range(2):
                root = Path(folder) / f'run{index}'
                root.mkdir()
                with mock.patch('sys.stderr', io.StringIO()):
                    run = match.Directory(root, 0, None, 86400, SecretBook())
                    store = run.server.store
                    row = store.register(fields, '127.0.0.1', 10, '0123456789abcdef')
                    store.mint_ice_servers(row['session_id'], dict(token=row['token'], match_id='m:1', ttl=600, iceServers=fixed),
                                           '0123456789abcdef', 11)
                    run.stop()
                log = root / 'service.log'
                counts.append(log.read_text(encoding='utf-8').count('relay_offer_issued') if log.is_file() else 0)
        self.assertEqual(counts, [1, 1])


class OracleCorrections(unittest.TestCase):
    """Two expectations the first live run (edith-pair-2, 2026-10-03 10:33 MST) proved wrong, each with field evidence."""

    def test_cloudflare_s_registered_relay_space_is_cloudflare(self):
        import relay_cloudflare_match as match
        # Both relay candidates of the live run; ARIN RDAP: NET-104-16-0-0-1 104.16.0.0/12 CLOUDFLARENET, Cloudflare, Inc.
        for address in ('104.30.136.195', '104.30.146.169'):
            self.assertTrue(match.cloudflare_address(address), address)
        for other in ('104.32.0.1', '68.3.162.151', '192.168.50.122'):
            self.assertFalse(match.cloudflare_address(other), other)

    def test_a_report_written_after_the_connection_closed_is_no_route_evidence(self):
        import relay_cloudflare_match as match
        closed = {'end_reason': 0, 'found': False, 'relay_pop': 0, 'relayed': False, 'remote_address': '', 'remote_identity': '', 'state': 0}
        verdict = match.judge_relay(cloudflare_run(reports={'client': closed}))
        self.assertFalse(verdict['passed'])
        self.assertEqual(verdict['reports'], {'client': 'closed'})
        self.assertFalse(match.judge_relay(cloudflare_run(reports={}))['passed'])
        found_direct = dict(closed, found=True, relayed=False, remote_address='24.251.145.96:5000', state=4)
        self.assertFalse(match.judge_relay(cloudflare_run(reports={'client': found_direct}))['passed'])


class LeakScrub(unittest.TestCase):
    """A relay login an engine wrote into a file (the host's replay carried one on 2026-10-03) is found, revoked with the
    provider and blanked in place; nothing prints the login."""

    def test_a_login_inside_a_binary_file_is_revoked_and_blanked(self):
        import tempfile
        import relay_scrub as scrub
        blob = (b'\x00\x01RPLY' + json.dumps({'iceServers': SERVERS}).encode() + b'\x00tail')
        revoked = []
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'match.ccreplay'
            path.write_bytes(blob)
            result = scrub.scrub([path], revoke=lambda username: revoked.append(username) or 204)
            after = path.read_bytes()
        self.assertEqual(revoked, ['unit-minted-username'])
        self.assertEqual(len(after), len(blob))
        for secret in (b'unit-minted-username', b'unit-minted-credential'):
            self.assertNotIn(secret, after)
        self.assertEqual(result['files'][0]['logins'], 1)
        self.assertEqual(result['revokes'], [204])
        self.assertNotIn('unit-minted', json.dumps(result))

    def test_an_archive_whose_bytes_spell_an_escape_is_read_as_its_members(self):
        """2026-10-04: an engine autosave (a zip) whose bytes held a backslash-u escape run was unescaped as if it were text; the
        mangled copy no longer parsed as a zip and the sweep called the whole run INCOMPLETE. A login inside a member is still found."""
        import io
        import tempfile
        import zipfile
        from relay_secrets import SecretBook, sweep
        def archive(member: bytes) -> bytes:
            buffer = io.BytesIO()
            with zipfile.ZipFile(buffer, 'w', zipfile.ZIP_STORED) as out:
                out.writestr('Save.ini', member)
            return buffer.getvalue()
        book = SecretBook()
        book.add_offer({'iceServers': SERVERS})
        with tempfile.TemporaryDirectory() as folder:
            clean = Path(folder) / 'clean' / 'tick-791.ccsave'
            clean.parent.mkdir()
            clean.write_bytes(archive(b'Name = \\u00e9t\\u00e9\n'))
            self.assertIn(b'\\u00e9', clean.read_bytes())
            scan = sweep([clean.parent], book.finder())
            self.assertEqual(scan['status'], 'CLEAN', scan)
            leaked = Path(folder) / 'leaked' / 'tick-792.ccsave'
            leaked.parent.mkdir()
            leaked.write_bytes(archive(b'Name = \\u00e9\nTurnPass = unit-minted-credential\n'))
            self.assertNotEqual(sweep([leaked.parent], book.finder())['status'], 'CLEAN')

    def test_a_file_without_a_login_is_left_byte_for_byte(self):
        import tempfile
        import relay_scrub as scrub
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'clean.ccreplay'
            path.write_bytes(b'\x00\x01 no login here')
            result = scrub.scrub([path], revoke=lambda username: 204)
            self.assertEqual(path.read_bytes(), b'\x00\x01 no login here')
        self.assertEqual(result['files'][0]['logins'], 0)

    def test_the_driver_revokes_every_login_it_minted(self):
        import tempfile
        import relay_cloudflare_match as match
        from relay_secrets import SecretBook
        calls = []

        def fake(request, *args, **kwargs):
            calls.append((request.get_method(), request.full_url.rsplit('/', 3)[-3:], request.get_header('User-agent')))
            if request.full_url.endswith('generate-ice-servers'):
                return answer()
            response = mock.MagicMock()
            response.__enter__.return_value = response
            response.status = 204
            return response
        fields = dict(name='unit', activity='t', scene='t', mode='pvp', peer_count=2, seats_free=1, game_version='1', build_id='a',
                      network_protocol_version=1, lockstep_codec_version=1, controller_frame_version=1, match_config_hash='a' * 64,
                      session_identity_hash='b' * 64, module_manifest_hash='c' * 64, listen_port=41010, listen_addrs=['127.0.0.1'],
                      join_mode='ice')
        with tempfile.TemporaryDirectory() as folder, mock.patch('sys.stderr', io.StringIO()), mock.patch.object(directory, 'urlopen', fake):
            run = match.Directory(Path(folder), 0, CONFIG, 600, SecretBook())
            store = run.server.store
            row = store.register(fields, '127.0.0.1', 10, '0123456789abcdef')
            store.mint_ice_servers(row['session_id'], dict(token=row['token'], match_id='m:1', ttl=600), '0123456789abcdef', 11)
            run.stop()
        self.assertEqual(run.revokes, [204])
        self.assertEqual(calls[-1][0], 'POST')
        self.assertEqual(calls[-1][1][-1], 'revoke')
        self.assertTrue(str(calls[-1][2]).startswith('cccp-session-directory/'))


class FeelBars(unittest.TestCase):
    """The feel numbers a relay row is judged by: the feel driver's six measured pins, its own thresholds, every one PASS."""

    def timing(self, **changes):
        pins = {name: {'status': 'PASS', 'value': 0} for name in ('item9a_wall_tps', 'item9a_net_wait', 'item9a_steady_stalls',
                                                                  'item9a_missing_frame_stalls', 'item9a_longest_wait',
                                                                  'item9a_confirmed_horizon_lag')}
        pins['item9a_harness_cost'] = {'status': 'MISS', 'value': None, 'reason': 'missing cost coverage: [sim_dump]'}
        pins['input_carried'] = {'status': 'MISS', 'value': None, 'reason': 'raw.jsonl: FileNotFoundError'}
        pins.update(changes)
        return {'peers': {'host': {'pins': pins, 'pass_check': False}}}

    def test_the_six_measured_pins_decide_and_the_unmeasurable_ones_are_listed(self):
        import relay_cloudflare_match as match
        verdict = match.feel_bars(self.timing(), 'host')
        self.assertTrue(verdict['passed'], verdict)
        self.assertEqual(sorted(verdict['unmeasured']), ['input_carried', 'item9a_harness_cost'])
        self.assertIs(verdict['pass_check'], False)

    def test_a_failed_or_missing_measured_pin_fails(self):
        import relay_cloudflare_match as match
        self.assertFalse(match.feel_bars(self.timing(item9a_wall_tps={'status': 'FAIL', 'value': 41.2}), 'host')['passed'])
        self.assertFalse(match.feel_bars(self.timing(item9a_longest_wait={'status': 'MISS', 'value': None}), 'host')['passed'])
        self.assertFalse(match.feel_bars({'peers': {}}, 'host')['passed'])

    def test_a_live_rows_one_wait_at_a_held_frame_is_the_spikes(self):
        import relay_cloudflare_match as match
        steady = dict(item9a_steady_stalls={'status': 'FAIL', 'value': 1}, item9a_missing_frame_stalls={'status': 'FAIL', 'value': 1})
        log = '[net-frame-wait] frame=1120 wait_ms=2\n[net-frame-wait] frame=200 wait_ms=40\n'
        verdict = match.feel_bars(self.timing(**steady), 'host', log=log, spikes={1120}, ticks=1201)
        self.assertTrue(verdict['passed'], verdict)
        self.assertEqual(verdict['live_spike_reading'], dict(steady=0, spike_waits=[(1120, 2)],
                                                             pins={'item9a_steady_stalls': 'FAIL', 'item9a_missing_frame_stalls': 'FAIL'}))
        # A wait at a frame no hold answered, a second wait at a held frame, or a spike's wait over 50 ms stays a failure.
        self.assertFalse(match.feel_bars(self.timing(**steady), 'host', log='[net-frame-wait] frame=1119 wait_ms=2\n', spikes={1120}, ticks=1201)['passed'])
        self.assertFalse(match.feel_bars(self.timing(**steady), 'host', log=log + '[net-frame-wait] frame=1120 wait_ms=3\n', spikes={1120},
                                         ticks=1201)['passed'])
        self.assertFalse(match.feel_bars(self.timing(item9a_longest_wait={'status': 'FAIL', 'value': 97}, **steady), 'host', log=log,
                                         spikes={1120}, ticks=1201)['passed'])
        self.assertFalse(match.feel_bars(self.timing(**steady), 'host')['passed'])


class LoginSweep(unittest.TestCase):
    """The pattern sweep finds a relay login in the clear without being told any secret, and passes a blanked one."""

    def test_a_clear_login_is_found_and_a_blanked_one_is_not(self):
        import tempfile
        import relay_login_sweep as sweep
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'clear.ccreplay').write_bytes(b'\x00' + json.dumps({'iceServers': SERVERS}).encode())
            (root / 'blank.ccreplay').write_bytes(b'\x00{"username":"xxxxxxxx","credential":"xxxxxxxxxx"}')
            result = sweep.sweep([root], set())
        self.assertEqual([Path(row['path']).name for row in result['files_with_logins']], ['clear.ccreplay'])
        self.assertNotIn('unit-minted', json.dumps(result))


if __name__ == '__main__':
    unittest.main()
