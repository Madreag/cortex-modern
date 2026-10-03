"""The independent read's counterexamples (F1-F6 of astra-read-relay-20261003), one unit row each: every one is a run or
an artifact the landed relay driver judged wrongly. No engine, no network, no real login (every value here is fake)."""
from __future__ import annotations

import contextlib
import copy
import gzip
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import relay_cloudflare_match as match  # noqa: E402
import relay_login_sweep  # noqa: E402
import relay_scrub  # noqa: E402
from relay_secrets import SecretBook  # noqa: E402

USER = 'u-6f"x\\k-77'          # a minted username the directory accepts, with a quote and a backslash
CRED = 'c"r\\ed-0123456789'
FIXED_USER, FIXED_PASS = 'bq7zrt', 'Pw-9q8w7e6'
LOGIN = {'iceServers': [{'urls': ['turn:relay.example:3478?transport=udp'], 'username': USER, 'credential': CRED}]}
HERE_SCENARIOS = TOOLS / 'e2e'
SESSION = '6d7c0768-f1b2-4510-a76b-03b600116232'


def book() -> SecretBook:
    secrets = SecretBook()
    secrets.add('minted-username', USER)
    secrets.add('minted-credential', CRED)
    secrets.add('fixed-username', FIXED_USER)
    secrets.add('fixed-password', FIXED_PASS)
    return secrets


def plain(document) -> bytes:
    return json.dumps(document).encode()


class EncodedForms(unittest.TestCase):
    """F3: a login in each form an engine or a harness can retain it, found by the book, the shape sweep and the scrub."""

    def check(self, name: str, data: bytes, kinds: set[str]):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / name
            path.write_bytes(data)
            scan = book().scan([folder])
            self.assertFalse(scan['clean'], f'{name}: the book scan reads it as clean')
            self.assertTrue(kinds <= set(scan['files_with_secrets'][0]['kinds']), scan['files_with_secrets'])
            shapes = relay_login_sweep.sweep([Path(folder)], set())
            self.assertTrue(shapes['files_with_logins'], f'{name}: the shape sweep finds no login')
            relay_scrub.scrub([Path(folder)], book=book())
            self.assertTrue(book().scan([folder])['clean'], f'{name}: a login is left after the scrub')
            self.assertFalse(relay_login_sweep.sweep([Path(folder)], set())['files_with_logins'], f'{name}: a login shape is left after the scrub')
            self.assertNotIn(b'c\\"r', b''.join(p.read_bytes() for p in Path(folder).rglob('*') if p.is_file()))

    def test_escaped_json_inside_a_json_string(self):
        self.check('capture.json', plain({'config': json.dumps(LOGIN)}), {'minted-username', 'minted-credential'})

    def test_a_hex_encoded_config_payload(self):
        self.check('resume.manifest', b'ConfigPayload = ' + plain(LOGIN).hex().encode() + b'\n', {'minted-username', 'minted-credential'})

    def test_ini_values_short_and_long(self):
        ini = f'SettingsMan\n\tNetworkTurnServers = turn:relay.example:3479\n\tNetworkTurnUser = {FIXED_USER}\n\tNetworkTurnPass = {FIXED_PASS}\n'
        self.check('Settings.ini', ini.encode(), {'fixed-username', 'fixed-password'})

    def test_a_zip_member(self):
        packed = io.BytesIO()
        with zipfile.ZipFile(packed, 'w', zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('Replay.ccrp', b'RPLY\x00\x01' + plain(LOGIN) + b'\x00tail')
        self.check('diagnostics.zip', packed.getvalue(), {'minted-username', 'minted-credential'})

    def test_a_gzip_member(self):
        self.check('match.ccreplay.gz', gzip.compress(b'RPLY' + plain(LOGIN)), {'minted-username', 'minted-credential'})

    def test_an_unreadable_file_is_incomplete_never_clean(self):
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / 'ok.log').write_text('route=relay\n')
            (Path(folder) / 'locked.ccreplay').write_bytes(b'RPLY')
            real = Path.read_bytes

            def read_bytes(path):
                if path.name == 'locked.ccreplay':
                    raise PermissionError(13, 'locked')
                return real(path)
            with mock.patch.object(Path, 'read_bytes', read_bytes):
                shapes = relay_login_sweep.sweep([Path(folder)], set())
                scanned = book().scan([folder])
        self.assertEqual(shapes.get('status', 'CLEAN' if not shapes['files_with_logins'] else 'LEAKED'), 'INCOMPLETE')
        self.assertEqual(scanned.get('status', 'CLEAN' if scanned['clean'] else 'LEAKED'), 'INCOMPLETE')
        self.assertFalse(scanned['clean'])

    def test_a_tick_hash_that_decodes_to_archive_magic_is_data_not_a_damaged_file(self):
        # A live run's trace: one of its hashes decoded as hex starts with gzip's magic (EDITH, 2026-10-03 2:49 PM).
        trace = plain({'ticks': [{'tick': 1, 'hash': '1f8b' + '7c' * 30}, {'tick': 2, 'hash': '504b0304' + '00' * 28}]})
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / 'host_trace.json').write_bytes(trace)
            scanned = book().scan([folder])
            shapes = relay_login_sweep.sweep([Path(folder)], set())
        self.assertEqual(scanned['status'], 'CLEAN', scanned['incomplete'])
        self.assertEqual(shapes['status'], 'CLEAN', shapes['incomplete'])

    def test_a_damaged_archive_file_is_incomplete_and_one_inside_a_value_is_still_read(self):
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / 'match.ccreplay.gz').write_bytes(gzip.compress(b'RPLY' + plain(LOGIN))[:-12])
            self.assertEqual(book().scan([folder])['status'], 'INCOMPLETE')
        with tempfile.TemporaryDirectory() as folder:
            cut = gzip.compress(b'RPLY' + plain(LOGIN) + bytes(range(256)) * 4)[:-40]
            (Path(folder) / 'capture.manifest').write_bytes(b'Payload = ' + cut.hex().encode() + b'\n')
            scanned = book().scan([folder])
        self.assertEqual(scanned['status'], 'LEAKED', scanned['incomplete'])

    def test_an_unlistable_directory_is_incomplete_never_clean(self):
        import relay_secrets
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / 'sub').mkdir()
            real = os.scandir

            def scandir(path):
                if Path(path).name == 'sub':
                    raise PermissionError(13, 'denied')
                return real(path)
            with mock.patch.object(relay_secrets.os, 'scandir', scandir):
                scanned = book().scan([folder])
        self.assertEqual(scanned.get('status', 'CLEAN' if scanned['clean'] else 'LEAKED'), 'INCOMPLETE')


class FakeRemote:
    def __init__(self, fail_on: str = '-peers.json'):
        self.fail_on, self.sent = fail_on, []

    def mkdir(self, path):
        pass

    def scp_to(self, local, remote):
        if str(local).endswith(self.fail_on):
            raise RuntimeError('scp failed: connection reset (synthetic)')
        self.sent.append(str(remote))


class FakeBox:
    def __init__(self, name='edith'):
        self.name, self.tree, self.alias, self.task = name, 'D:/mx/lane/engine', name, 'cortex-session1'
        self.path_prepend, self.max_engines, self.computer = [], 2, name.upper()
        self.remote = FakeRemote()


def legacy_fixed_run():
    return dict(name='coturn', relay='coturn', host_relay_mode='Fixed', relay_urls=['turn:relay.example:3479?transport=udp'],
                timeout_s=420, peers=[dict(name='host', box='edith', connection='RelayOnly'), dict(name='client', box='edith', connection='RelayOnly')])


class TransferFailure(unittest.TestCase):
    """F1: a fixed relay login must never reach a file; a transfer that fails mid-ship leaves no unredacted write behind."""

    def test_a_failed_spec_transfer_leaves_no_login_on_any_path(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder) / 'coturn'
            root.mkdir()
            (root / 'cert.pem').write_text('certificate')
            boxes = {'edith': FakeBox()}
            run = legacy_fixed_run()
            ports = dict(directory=48700, host=48701, client=48702)
            try:
                specs = match.build_specs(None, run, root, ports, boxes, 'a' * 64, (FIXED_USER, FIXED_PASS), 1201)
                with contextlib.suppress(RuntimeError):
                    match.ship(boxes['edith'], root, specs, ports['directory'], Path(folder) / 'payload')
            except ValueError:
                pass
            scan = book().scan([folder])
        self.assertTrue(scan['clean'], scan['files_with_secrets'])


def verdict_from(name: str, kind: str, peers: list[str], **checks) -> dict:
    base = {key: True for key in ('identities', 'builds', 'exits', 'full_history', 'hashes_equal', 'holds', 'feel_bars', 'relay',
                                  'rtt_recorded', 'no_secret_in_files', 'provider_201', 'logins_revoked', 'relay_registrant',
                                  'renewal', 'migration', 'listing', 'seat_holds', 'offer_fresh', 'endpoint', 'route_receipts',
                                  'frames_past_expiry', 'tunnel', 'panel', 'menu_choice')}
    for peer in ('host', 'client', 'client2', 'hotspot'):
        base.update({f'tunnel:{peer}': True, f'panel:{peer}': True, f'menu_choice:{peer}': True})
    base.update(checks)
    return dict(name=name, relay=kind, root='D:/mx/x', passed=all(base.values()), checks=base, session_id=SESSION,
                relay_evidence=dict(passed=base['relay'], reasons=[], routes={peer: 'relay' for peer in peers},
                                    peers={peer: dict(route='relay', accepted=[dict(line='[net-route] RouteAllowed route=relay allowed=1 connection=7')],
                                                      relay_addresses=['104.30.146.169'], candidates=['104.30.146.169:4000 relay']) for peer in peers},
                                    direct_as_expected=None),
                rtt=[dict(peer=2, rtt_ms=4, delay_frames=5)], compared_ticks={'host/client': 1201}, desyncs=0)


def scenario(name: str) -> dict:
    return json.loads((HERE_SCENARIOS / f'{name}.json').read_text(encoding='utf-8'))


class MandatoryChecks(unittest.TestCase):
    """F2: a run that failed any mandatory check fails the review and the exit status, whatever the checklist names."""

    def review(self, scenario_name: str, verdict: dict):
        whole = scenario(scenario_name)
        only = dict(whole, runs=[run for run in whole['runs'] if run['name'] == verdict['name']],
                    checklist=[item for item in whole['checklist'] if item['run'] == verdict['name']])
        with tempfile.TemporaryDirectory() as folder:
            document = match.review(only, [verdict], Path(folder))
        self.assertFalse(document['passed'], f'{verdict["name"]}: review passed a failed run')
        status = match.exit_status(document, [verdict]) if hasattr(match, 'exit_status') else 0
        self.assertNotEqual(status, 0)

    def test_a_login_left_unrevoked_fails(self):
        self.review('mp-relay-cloudflare', verdict_from('cloudflare', 'cloudflare', ['host', 'client'], logins_revoked=False))

    def test_a_relay_outside_the_provider_registry_fails(self):
        self.review('mp-relay-cloudflare', verdict_from('cloudflare', 'cloudflare', ['host', 'client'], relay_registrant=False))

    def test_a_login_in_a_file_fails_every_row(self):
        self.review('mp-relay-hotspot', verdict_from('d-credential-expiry', 'cloudflare', ['host', 'client'], no_secret_in_files=False))

    def test_a_peer_with_no_evidence_fails(self):
        verdict = verdict_from('b-hotspot-host', 'automatic', ['client'], relay=False)
        verdict['relay_evidence']['reasons'] = ['host: no accepted route receipt for session None']
        self.review('mp-relay-hotspot', verdict)

    def test_an_incomplete_history_fails(self):
        self.review('mp-relay-hotspot', verdict_from('a-automatic-fallback', 'automatic', ['host', 'client'], full_history=False))

    def test_row_e_missing_client2_candidate_fails(self):
        verdict = verdict_from('e-migration-relayed', 'cloudflare', ['client', 'client2'], relay=False)
        verdict['relay_evidence']['reasons'] = ['client2: sent no relay candidate through the directory']
        self.review('mp-relay-hotspot', verdict)


def signal(identity: str, *candidates: str) -> str:
    import base64
    body = b'\x42' + bytes([len(identity)]) + identity.encode() + b''.join(b'\x1a' + bytes([len(text)]) + text.encode() for text in candidates)
    return base64.b64encode(body).decode()


def relay_line(address: str, port: int) -> str:
    return f'candidate:1 1 udp 16777215 {address} {port} typ relay raddr 0.0.0.0 rport 0'


def receipts(session: str, route: str = 'relay', connection: int = 7) -> str:
    return '\n'.join([f'[net-ice] session {session} join_mode=ice resolved to identity x; dialling the ICE half',
                      f'[net-ice] selected candidate={route} connection={connection}',
                      f'[net-route] RouteAllowed route={route} allowed=1 connection={connection}'])


def three_peer_run(**changes):
    run = dict(mode='cloudflare', session_id=SESSION, connection={'host': 'RelayOnly', 'client': 'RelayOnly', 'client2': 'RelayOnly'},
               logs={peer: receipts(SESSION) for peer in ('host', 'client', 'client2')},
               signals=[('host', signal('str:h-6d7c0768', relay_line('104.30.150.145', 4001))),
                        ('client:aaaa1111bbbb2222cccc3333', signal('str:c-aaaa1111', relay_line('104.30.146.169', 4002))),
                        ('client:dddd4444eeee5555ffff6666', signal('str:c-dddd4444', relay_line('104.30.146.170', 4003)))],
               identities={'client': 'str:c-aaaa1111bbbb2222', 'client2': 'str:c-dddd4444eeee5555'},
               offers=[dict(session_id=SESSION, match_id=f'{SESSION}:1', provider='cloudflare', generation=1, expires_at=4102444800, server_count=2)],
               offer_urls=['turn:turn.cloudflare.com:3478?transport=udp'], client_connection={'found': True, 'relayed': True, 'remote_address': '104.30.150.145:4001'},
               relay_addresses=None, run_ends_at=4000000000)
    run.update(changes)
    return run


class Wiring(unittest.TestCase):
    """F4: the hotspot rows wired end to end: names, ticks, every joiner's identity, row f's order."""

    def hotspot(self, name):
        return next(run for run in scenario('mp-relay-hotspot')['runs'] if run['name'] == name)

    def specs(self, run, root):
        boxes = {name: FakeBox(name) for name in ('edith', 'ally', 'z13')}
        ports = dict(directory=48700, **{peer['name']: 48701 + index for index, peer in enumerate(run['peers'])})
        return match.build_specs(None, run, root, ports, boxes, 'a' * 64, None, int(run.get('ticks', 1201)))

    def test_the_public_lookup_name_is_the_name_the_engine_lists(self):
        run = dict(self.hotspot('a-automatic-fallback'), tag='rpabc123')
        with tempfile.TemporaryDirectory() as folder:
            specs = self.specs(run, Path(folder))
        host = next(spec for spec in specs if spec['peer'] == 'host')
        flags = host['flags']
        self.assertIn('-net-player-name', flags)
        self.assertEqual(flags[flags.index('-net-player-name') + 1], match.display_name(run, run['peers'][0]))

    def test_each_row_runs_its_declared_ticks(self):
        seen = []

        def capture(scenario_, run, out, boxes, ticks, book_):
            seen.append(ticks)
            return dict(name=run['name'], dry_run=True)
        with tempfile.TemporaryDirectory() as folder, mock.patch.object(match, 'run_one', capture), contextlib.redirect_stdout(io.StringIO()):
            match.main(['--scenario', 'mp-relay-hotspot', '--out', str(Path(folder) / 'x'), '--run', 'd-credential-expiry',
                        '--run', 'e-migration-relayed', '--dry-run'])
        self.assertEqual(seen, [24000, 1801])

    def test_every_joiner_keeps_its_own_candidates(self):
        verdict = match.judge_relay(three_peer_run())
        self.assertTrue(verdict['passed'], verdict['reasons'])
        self.assertEqual(verdict['peers']['client2']['relay_addresses'], ['104.30.146.170'])

    def test_row_f_chooses_relay_only_before_any_connection(self):
        run = self.hotspot('f-relay-by-hand')
        with tempfile.TemporaryDirectory() as folder:
            specs = self.specs(run, Path(folder))
        client = next(spec for spec in specs if spec['peer'] == 'client')
        self.assertNotIn('-net-match-service-e2e', client['flags'])
        self.assertIn('-menu-script', client['flags'])
        late = '[net-ice] session x join_mode=ice\n[menu-script] assert_label ComboNetworkConnection "Relay only" text="Relay only" PASS'
        self.assertFalse(match.menu_choice(late, 'Relay only')['passed'])


class Binding(unittest.TestCase):
    """F5: the route is bound to the provider, the phase and a fresh offer."""

    def test_every_inherited_relay_setting_is_cleared(self):
        run = next(run for run in scenario('mp-relay-cloudflare')['runs'] if run['name'] == 'cloudflare')
        for peer in run['peers']:
            settings = match.peer_settings(run, peer, 48700, 'a' * 64, None)
            for key in ('NetworkTurnServers', 'NetworkTurnUser', 'NetworkTurnPass', 'NetworkPlayerTurnServers',
                        'NetworkPlayerTurnUser', 'NetworkPlayerTurnPass'):
                self.assertEqual(settings.get(key), '', f'{peer["name"]}: {key} inherited')
            self.assertEqual(settings.get('NetworkHostRelayMode'), 'Directory', peer['name'])

    def test_a_stale_or_foreign_offer_is_rejected(self):
        two = dict(connection={'host': 'RelayOnly', 'client': 'RelayOnly'}, logs={'host': receipts(SESSION), 'client': receipts(SESSION)},
                   signals=three_peer_run()['signals'][:2])
        self.assertTrue(match.judge_relay(three_peer_run(**two))['passed'], 'the fresh two-peer baseline must pass')
        for offer in (dict(session_id=SESSION, match_id=f'{SESSION}:1', provider='cloudflare', generation=1, expires_at=1, server_count=2),
                      dict(session_id=SESSION, match_id='another-session:1', provider='cloudflare', generation=1, expires_at=4102444800, server_count=2)):
            with self.subTest(offer=offer):
                self.assertFalse(match.judge_relay(three_peer_run(offers=[offer], **two))['passed'])

    def test_a_closed_report_is_no_route_proof(self):
        closed = {'end_reason': 0, 'found': False, 'relay_pop': 0, 'relayed': False, 'remote_address': '', 'remote_identity': '', 'state': 0}
        run = three_peer_run(mode='automatic', provider='cloudflare', expect_routes={'client': 'relay'}, signals=[], offer_urls=None,
                             client_connection=closed, connection={'host': 'Automatic', 'client': 'Automatic'},
                             logs={'host': receipts(SESSION), 'client': receipts(SESSION)})
        self.assertFalse(match.judge_relay(run)['passed'])


def tunnel(**changes):
    rows = [dict(step='before', at='2026-10-03 14:00:00 MST', backend_state='Running'),
            dict(step='down', at='2026-10-03 14:00:01 MST', exit_code=0, backend_state='Stopped'),
            dict(step='engine-started', at='2026-10-03 14:00:05 MST', peer='client', backend_state='Stopped'),
            dict(step='engine-ended', at='2026-10-03 14:01:05 MST', peer='client', backend_state='Stopped'),
            dict(step='up', at='2026-10-03 14:01:06 MST', exit_code=0, backend_state='Running')]
    for index, change in changes.items():
        rows[int(index[1:])].update(change)
    return rows


class Evidence(unittest.TestCase):
    """F6: the tunnel, the precondition, the restoration and the renewal admit no incomplete evidence."""

    def test_a_missing_bracket_or_a_running_middle_fails(self):
        self.assertTrue(match.tunnel_receipt(tunnel())['passed'])
        self.assertFalse(match.tunnel_receipt([row for row in tunnel() if row['step'] != 'engine-started'])['passed'])
        self.assertFalse(match.tunnel_receipt([row for row in tunnel() if row['step'] != 'engine-ended'])['passed'])
        middle = tunnel()
        middle.insert(3, dict(step='engine-sample', at='2026-10-03 14:00:30 MST', peer='client', backend_state='Running'))
        self.assertFalse(match.tunnel_receipt(middle)['passed'])

    def test_an_unknown_network_precondition_is_refused(self):
        script = Path(os.environ.get('CC_RELAY_HOTSPOT_SCRIPT', 'D:/Projects/reviews/takeover-20260909/grok-workers/lead-tools/hotspot_relay.sh'))
        with tempfile.TemporaryDirectory() as folder:
            shim = Path(folder) / 'ssh'
            shim.write_text('#!/usr/bin/env bash\ncase "$*" in *BackendState*) echo Running;; *) echo "";; esac\n', encoding='utf-8', newline='\n')
            env = dict(os.environ, PATH=folder + os.pathsep + os.environ.get('PATH', ''), RELAY_REPO=str(TOOLS.parent),
                       RELAY_SSH=shim.as_posix())
            bash = 'C:/Program Files/Git/bin/bash.exe' if os.name == 'nt' else 'bash'
            done = subprocess.run([bash, str(script), 'a', '--dry-run'], env=env, capture_output=True, text=True, timeout=300)
        self.assertNotEqual(done.returncode, 0, done.stdout[-400:])
        self.assertIn('REFUSED', done.stdout + done.stderr)

    def test_the_tailnet_comes_back_after_a_cleanup_exception(self):
        calls = []

        class Run:
            record = dict(pid=1, exit_code=0, exe_sha256='0' * 64)

            def start(self): pass
            def finish(self): return self.record
            def close(self): pass
            def poll(self): return 0

        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            spec = dict(peer='client', box='ally', repo=str(root), flags=['-net-host'], tailscale_down=True, settings={})
            spec_path = root / 'ally-peers.json'
            spec_path.write_text(json.dumps(dict(root=str(root), peers=[spec], directory=dict(port=1, cert=''))))
            harness = mock.MagicMock()
            with mock.patch.object(match, 'tailscale', lambda command: calls.append(command[0]) or dict(backend_state='Running')), \
                 mock.patch('edith_cross.harness', return_value=harness), mock.patch('edith_cross.prepare_peer', return_value=Run()), \
                 mock.patch('edith_cross.redact', side_effect=RuntimeError('cleanup failed (synthetic)')), contextlib.redirect_stdout(io.StringIO()):
                with contextlib.suppress(RuntimeError):
                    match.remote_peers(str(spec_path))
            restored = (root / 'ally-tailscale.json').is_file()
        self.assertIn('up', calls)
        self.assertTrue(restored)

    def test_a_zero_connection_renewal_is_no_renewal(self):
        zero = '[net-relay] relay login renewed on 0 live connection(s)'
        verdict = match.renewal_evidence({'host': zero, 'client': zero}, [dict(status=201), dict(status=201)], ['host', 'client'])
        self.assertFalse(verdict['passed'])


def rdap(registrant_roles=('registrant',), name='Cloudflare, Inc.', start='104.16.0.0', end='104.31.255.255') -> dict:
    vcard = ['vcard', [['version', {}, 'text', '4.0'], ['fn', {}, 'text', name]]]
    return dict(handle='NET-104-16-0-0-1', name='CLOUDFLARENET', startAddress=start, endAddress=end,
                entities=[dict(roles=list(registrant_roles), vcardArray=vcard)])


class ProofPieces(unittest.TestCase):
    """The pieces RESUME 1 added beside the counterexamples: the registry rule, the Fixed pair's death beside a live
    control, the relay selected after a menu choice, and the sweep on a failure before the judge."""

    def test_the_registry_needs_a_cloudflare_registrant_holding_the_address(self):
        self.assertTrue(match.judge_registrant('104.30.150.145', rdap())['cloudflare'])
        for name, record in {'no registrant role': rdap(registrant_roles=('abuse',)), 'another registrant': rdap(name='Example Hosting'),
                             'a range without the address': rdap(start='104.16.0.0', end='104.23.255.255'),
                             'no range': {k: v for k, v in rdap().items() if k != 'endAddress'}}.items():
            with self.subTest(name):
                self.assertFalse(match.judge_registrant('104.30.150.145', record)['cloudflare'])

    def test_the_fixed_pair_is_dead_only_beside_a_live_control(self):
        pair = match.coturn_rest_login('unit-secret', 420, now=1_000_000)
        expiry = int(pair[0].split(':')[0])

        def answers(control, expired):
            return lambda server, username, password: expired if username == pair[0] else control
        dead, alive = dict(result='refused', error_code=401), dict(result='allocated')
        receipt = lambda allocate: match.pair_ttl_receipt(('192.0.2.1', 3490), pair, 'unit-secret', SecretBook(), now=lambda: expiry + 10,
                                                          sleep=lambda seconds: None, allocate=allocate)
        self.assertTrue(receipt(answers(alive, dead))['passed'])
        self.assertFalse(receipt(answers(dead, dead))['passed'])
        self.assertFalse(receipt(answers(alive, alive))['passed'])
        self.assertFalse(receipt(answers(alive, dict(result='refused', error_code=438)))['passed'])

    def test_the_relay_must_be_selected_after_the_choice(self):
        chosen = '[menu-script] assert_label ComboNetworkConnection "Relay only" text="Relay only" PASS'
        self.assertTrue(match.menu_choice(chosen + '\n' + receipts(SESSION), 'Relay only', SESSION)['passed'])
        self.assertFalse(match.menu_choice(chosen + '\n' + receipts(SESSION, route='direct'), 'Relay only', SESSION)['passed'])
        self.assertFalse(match.menu_choice(chosen + '\n' + receipts('another-session'), 'Relay only', SESSION)['passed'])

    def test_a_published_username_or_fixture_is_reported_never_a_secret(self):
        import relay_secrets
        published = {FIXED_USER.encode(), b'private-user'}
        public = lambda value: 'in the repository' if value in published else None
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / 'notes.md').write_text(f'the {FIXED_USER} project\n{{"username": "private-user"}}\n', encoding='utf-8')
            sweep = relay_secrets.sweep([folder], book().finder(), public=public)
            self.assertEqual(sweep['status'], 'CLEAN', sweep['files_with_secrets'])
            self.assertEqual(sweep['public'][0]['hits'], 2)
            (Path(folder) / 'Settings.ini').write_text(f'\tNetworkTurnPass = {FIXED_PASS}\n\tNetworkTurnUser = {USER}\n', encoding='utf-8')
            leaked = relay_secrets.sweep([folder], book().finder(), public=lambda value: 'in the repository')
        self.assertEqual(leaked['status'], 'LEAKED')
        self.assertEqual({kind for row in leaked['files_with_secrets'] for kind in row['kinds']} & {'fixed-password', 'minted-username'},
                         {'fixed-password', 'minted-username'})

    def test_a_menu_check_without_a_session_fails(self):
        run = dict(match.REQUIRED)
        self.assertIn('pair_blanked', run['fixed'])
        self.assertNotIn('no_secret_in_files', run['fixed'])
        self.assertIn('no_secret_in_files', run['cloudflare'])

    def test_a_failure_before_the_judge_still_sweeps_every_box(self):
        swept = []

        class Directory:
            def __init__(self, *args, **kwargs):
                self.pin, self.revokes, self.minted, self.signals, self.offers, self.provider_calls = 'a' * 64, [204], [], [], [], []

            def stop(self):
                pass

        class Tunnel:
            def __init__(self, box, ports, log_path):
                self.box = box

            def open(self):
                raise RuntimeError('the ssh -R tunnel did not open (synthetic)')

            def close(self):
                pass
        run = dict(name='cloudflare', relay='cloudflare', host_relay_mode='Directory', timeout_s=420,
                   peers=[dict(name='host', box='edith', connection='RelayOnly'), dict(name='client', box='edith', connection='RelayOnly')])
        passing = dict(status='PASS', head='a' * 40, executable_sha256='b' * 64, errors=[])
        with tempfile.TemporaryDirectory() as folder, \
             mock.patch.object(match, 'identity', return_value=passing), mock.patch.object(match, 'choose_ports', return_value=list(range(48700, 48705))), \
             mock.patch.object(match, 'Directory', Directory), mock.patch.object(match, 'Tunnel', Tunnel), \
             mock.patch.object(match, 'sanitize_box', lambda box, root, book_, payload: swept.append(box.name) or dict(box=box.name, status='INCOMPLETE')), \
             mock.patch('relay_secrets.read_turn_config', return_value=dict(turn_key_id='unit-key', api_token='unit-token')), \
             mock.patch('edith_cross.harness', return_value=mock.MagicMock()), mock.patch('edith_cross.looped_input'), \
             contextlib.redirect_stdout(io.StringIO()):
            verdict = match.run_one(dict(name='mp-relay-cloudflare'), run, Path(folder), {'edith': FakeBox()}, 1201, SecretBook())
        self.assertEqual(swept, ['edith'])
        self.assertFalse(verdict['passed'])
        self.assertIn('synthetic', verdict['refused'])
        self.assertTrue(any(row.get('box') == 'here' for row in verdict['sanitize']), verdict['sanitize'])


if __name__ == '__main__':
    unittest.main()
