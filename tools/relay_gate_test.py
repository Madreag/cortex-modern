"""Counterexamples to the relay driver and its sweep, one unit row each: every row is a run, an artifact or an input the
judge or the sweep once read wrongly, built from synthetic values only (no issued login, no network, no engine)."""
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
from inventory_location import lead_script  # noqa: E402

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

    def test_a_windows_settings_file_is_read_by_its_login_shape(self):
        # The engine writes Settings.ini with CRLF line ends; the shape sweep must read a login there with no book at all.
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / 'Settings.ini').write_bytes(b'SettingsMan\r\n\tNetworkTurnServers = turn:relay.example:3479\r\n'
                                                        b'\tNetworkPlayerTurnUser = some-unknown-user\r\n\tNetworkPlayerTurnPass = some-unknown-pass\r\n')
            shapes = relay_login_sweep.sweep([Path(folder)], set())
        self.assertEqual(shapes['status'], 'LEAKED', shapes)
        self.assertEqual(shapes['files_with_logins'][0]['fields'], ['shape:ini-relay-login'])

    def test_a_settings_key_inside_a_json_spec_is_a_login_field(self):
        # The first pass's EDITH spec kept NetworkTurnUser as a JSON key (edith-set-5/coturn/edith-peers.json, 4:01 PM sweep).
        spec = plain({'peers': [{'settings': {'NetworkTurnUser': 'some-unknown-user', 'NetworkTurnPass': ''}}]})
        for name, data in (('peers.json', spec), ('wrapped.json', plain({'spec': spec.decode()}))):
            with self.subTest(name), tempfile.TemporaryDirectory() as folder:
                (Path(folder) / name).write_bytes(data)
                shapes = relay_login_sweep.sweep([Path(folder)], set())
                self.assertEqual(shapes['status'], 'LEAKED', shapes)

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

    def ssh(self, command, timeout=120, check=True):
        import time
        if command.rstrip().endswith(' data-digests'):
            return '{"Data/Base.rte": ["same-on-every-fake-box", 1]}'  # the game data preflight: every fake box holds the same
        return str(int(time.time() * 1000))

    def fetch_list(self, root, listing, local_root, tar_name):
        return 0

    def start_task(self, script, budget_s=900):
        pass

    def read_text(self, path, timeout=120):
        return None


class FakeBox:
    def __init__(self, name='edith'):
        self.name, self.tree, self.alias, self.task = name, 'D:/mx/lane/engine', name, 'cortex-session1'
        self.path_prepend, self.max_engines, self.computer = [], 2, name.upper()
        self.remote = FakeRemote()


class LocalGameBox(unittest.TestCase):
    """A game box that is this box (row c's EROL-PC): its run files are the driver's own run root."""

    def test_a_local_box_is_swept_in_its_own_run_root(self):
        secret = 'Zq8vN3pXw7LmT2rK5yHcRb4'
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder) / 'c-four-players'
            (root / 'client2').mkdir(parents=True)
            (root / 'client2' / 'stdout.log').write_text(f'[net-ice] relay login {secret}\n', encoding='utf-8')
            box = FakeBox('erol-pc')
            box.local = True
            secrets = SecretBook()
            secrets.add('minted-credential', secret)
            with mock.patch.object(match.subprocess, 'run', side_effect=AssertionError('a local box is never reached over ssh')):
                receipt = match.sanitize_box(box, root, secrets, Path(folder) / 'payload')
            text = (root / 'client2' / 'stdout.log').read_text(encoding='utf-8')
        self.assertEqual((receipt['box'], receipt['status'], receipt['hits_before'], receipt['hits_after']), ('erol-pc', 'CLEAN', 1, 0))
        self.assertNotIn(secret, text)


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
    # Every check any row names starts true, so a negative row isolates the one check it turns false.
    base = {key: True for key in ('identities', 'builds', 'exits', 'full_history', 'hashes_equal', 'holds', 'feel_bars', 'relay',
                                  'rtt_recorded', 'no_secret_in_files', 'provider_201', 'logins_revoked', 'relay_registrant',
                                  'renewal', 'migration', 'listing', 'seat_holds', 'offer_fresh', 'endpoint', 'route_receipts',
                                  'frames_past_expiry', 'tunnel', 'panel', 'menu_choice', 'secrets_observed', 'sanitizer_clean',
                                  'direct_expected', 'logins_short_lived', 'pair_blanked', 'pair_ttl', 'menu_entered')}
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

    def review(self, scenario_name: str, verdict: dict, passes: bool = False):
        whole = scenario(scenario_name)
        only = dict(whole, runs=[run for run in whole['runs'] if run['name'] == verdict['name']],
                    checklist=[item for item in whole['checklist'] if item['run'] == verdict['name']])
        with tempfile.TemporaryDirectory() as folder:
            document = match.review(only, [verdict], Path(folder))
        status = match.exit_status(document, [verdict]) if hasattr(match, 'exit_status') else 0
        if passes:
            self.assertTrue(document['passed'], [item for item in document['checklist'] if item['state'] != 'PASS'])
            self.assertEqual(status, 0)
            return
        self.assertFalse(document['passed'], f'{verdict["name"]}: review passed a failed run')
        self.assertNotEqual(status, 0)

    def test_the_same_verdict_with_every_check_true_passes(self):
        for scenario_name, row, kind, peers in (('mp-relay-cloudflare', 'cloudflare', 'cloudflare', ['host', 'client']),
                                                 ('mp-relay-hotspot', 'd-credential-expiry', 'cloudflare', ['host', 'client'])):
            with self.subTest(row):
                self.review(scenario_name, verdict_from(row, kind, peers), passes=True)

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


OPEN_REPORT = {'found': True, 'state': 3, 'relayed': True, 'remote_address': '', 'remote_identity': 'str:h-6d7c0768'}
CLOSED_REPORT = {'end_reason': 0, 'found': False, 'relay_pop': 0, 'relayed': False, 'remote_address': '', 'remote_identity': '', 'state': 0}


def three_peer_run(**changes):
    run = dict(mode='cloudflare', session_id=SESSION, connection={'host': 'RelayOnly', 'client': 'RelayOnly', 'client2': 'RelayOnly'},
               logs={peer: receipts(SESSION) for peer in ('host', 'client', 'client2')},
               signals=[('host', signal('str:h-6d7c0768', relay_line('104.30.150.145', 4001))),
                        ('client:aaaa1111bbbb2222cccc3333', signal('str:c-aaaa1111', relay_line('104.30.146.169', 4002))),
                        ('client:dddd4444eeee5555ffff6666', signal('str:c-dddd4444', relay_line('104.30.146.170', 4003)))],
               identities={'client': 'str:c-aaaa1111bbbb2222', 'client2': 'str:c-dddd4444eeee5555'},
               offers=[dict(session_id=SESSION, match_id=f'{SESSION}:1', provider='cloudflare', generation=1, expires_at=4102444800, server_count=2)],
               offer_urls=['turn:turn.cloudflare.com:3478?transport=udp'], client_connection={'found': True, 'relayed': True, 'remote_address': '104.30.150.145:4001'},
               reports={'client': OPEN_REPORT, 'client2': OPEN_REPORT}, overrides_cleared=True, relay_addresses=None, run_ends_at=4000000000)
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
        with tempfile.TemporaryDirectory() as folder, mock.patch.object(match, 'run_one', capture), contextlib.redirect_stdout(io.StringIO()), \
                mock.patch.object(match, 'LANE', 'unit-lane', create=True):
            # The scenario's game boxes as an inventory of the test's own, so no box's copy is read.
            boxes = Path(folder) / 'boxes.json'
            boxes.write_text(json.dumps(dict(boxes=[dict(name=name, kind='windows-task', ssh=name.lower(), repo=f'D:/Projects/{name}-build')
                                                    for name in ('EDITH', 'ALLY', 'Z13')])), encoding='utf-8')
            with mock.patch.dict(os.environ, {'CC_RELAY_BOXES_JSON': str(boxes)}):
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
                             client_connection=closed, reports={'client': closed}, connection={'host': 'Automatic', 'client': 'Automatic'},
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
        script = Path(os.environ.get('CC_RELAY_HOTSPOT_SCRIPT') or lead_script('hotspot_relay.sh'))
        self.assertTrue(script.is_file(), f'{script} is absent: the inventory copy this run reads does not carry the hotspot script')
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

    def test_the_retired_name_in_a_login_field_is_a_login(self):
        # EDITH's first-pass coturn run kept the retired account's name as NetworkTurnUser (2026-10-03 3:46 PM sweep).
        import relay_secrets
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / 'Settings.ini').write_text(f'\tNetworkTurnUser = {FIXED_USER}\n', encoding='utf-8')
            (Path(folder) / 'peers.json').write_text(json.dumps({'settings': {'NetworkTurnUser': FIXED_USER}, 'iceServers': [
                {'username': FIXED_USER}]}), encoding='utf-8')
            sweep = relay_secrets.sweep([folder], book().finder())
        self.assertEqual(sweep['status'], 'LEAKED')
        self.assertEqual(sorted(Path(row['path']).name for row in sweep['files_with_secrets']), ['Settings.ini', 'peers.json'])

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
             mock.patch.object(match, 'LANE', 'unit-lane', create=True), contextlib.redirect_stdout(io.StringIO()):
            verdict = match.run_one(dict(name='mp-relay-cloudflare'), run, Path(folder), {'edith': FakeBox()}, 1201, SecretBook())
        self.assertEqual(swept, ['edith'])
        self.assertFalse(verdict['passed'])
        self.assertIn('synthetic', verdict['refused'])
        self.assertTrue(any(row.get('box') == 'here' for row in verdict['sanitize']), verdict['sanitize'])



# --- the green-tip probes: each row was judged wrongly by the tip they were read on -----------------------------------

def supported(function, **kwargs):
    """The keyword arguments a function takes: a probe passes its evidence to whichever judge it meets, so a judge that
    ignores a piece of evidence is tested on the rest instead of failing on a signature."""
    import inspect
    parameters = inspect.signature(function).parameters
    return {key: value for key, value in kwargs.items() if key in parameters}


def synthetic(label: str, length: int = 64) -> str:
    import hashlib
    return hashlib.sha256(label.encode()).hexdigest()[:length]


def tracked_repo(folder: Path, text: str) -> Path:
    """A throwaway repository whose HEAD tracks the text: what 'a matching tracked value' looked like to the old sweep."""
    repo = folder / 'repo'
    repo.mkdir()
    (repo / 'tracked.txt').write_text(text, encoding='utf-8')
    for command in (['init', '-q'], ['add', 'tracked.txt'], ['-c', 'user.name=unit', '-c', 'user.email=unit@example.invalid', 'commit', '-q', '-m', 'unit']):
        subprocess.run(['git', '-C', str(repo), *command], capture_output=True, check=True)
    return repo


def lane_sweep(root: Path, folder: Path, secrets: SecretBook, repo: Path) -> tuple[int, dict]:
    import relay_lane_sweep
    out = folder / 'receipt.json'
    with mock.patch.object(relay_lane_sweep, 'lane_book', return_value=(secrets, ['synthetic'])), \
            mock.patch.object(relay_lane_sweep, 'REPO', repo, create=True), contextlib.redirect_stdout(io.StringIO()):
        status = relay_lane_sweep.main(['--root', str(root), '--out', str(out)])
    return status, json.loads(out.read_text(encoding='utf-8'))


def read_ticks(path) -> set:
    path = Path(path)
    return {json.loads(line)['tick'] for line in path.read_text(encoding='utf-8').splitlines() if line.strip()} if path.is_file() else set()


class FakeHarness:
    def __init__(self, names):
        pins = {bar: dict(status='PASS', value=1) for bar in match.FEEL_BARS}
        peer = dict(pins=pins, metrics={}, pass_check=True)
        self.records = mock.MagicMock()
        self.feel = mock.MagicMock()
        self.feel.reduce_timing_case.return_value = dict(peers={name: dict(peer) for name in names}, proof={'pass': True})
        self.feel.timing_peer.return_value = dict(peer)
        self.feel.compare_live_hashes.side_effect = lambda left, right, step: [
            dict(compared_ticks=len(read_ticks(left) & read_ticks(right)), mismatched_ticks=0)]


def judge_folder(root: Path, peers: dict, session: str = SESSION, ticks: int = 1202, missing=(), offers=True, provider='cloudflare'):
    """A finished run's folder: each peer's log with its route receipts, its live ticks, its record and its report."""
    for name, peer in peers.items():
        (root / name).mkdir(parents=True)
        opening = f'[net-ice] host session {session}' if name == 'host' else f'[net-ice] session {session} join_mode=ice resolved'
        lines = [opening, f'[net-ice] selected candidate={peer["route"]} connection={peer["connection"]}',
                 f'[net-route] RouteAllowed route={peer["route"]} allowed=1 connection={peer["connection"]}']
        if name == 'host':
            lines.append('[net-match] auto input delay: peer 2 rtt 5ms -> 3 frames (manual floor 0)')
        (root / name / 'stdout.log').write_text('\n'.join(lines) + '\n', encoding='utf-8')
        (root / f'{name}-live.jsonl').write_text(''.join(json.dumps(dict(tick=tick)) + '\n' for tick in range(1, ticks + 1)
                                                         if tick not in missing), encoding='utf-8')
        (root / f'{name}-record.json').write_text(json.dumps(dict(exit_code=0, evidence_complete=True, timed_out=False,
                                                                  exe_sha256='a' * 64)), encoding='utf-8')
        report = peer.get('report', CLOSED_REPORT if name == 'host' else OPEN_REPORT)
        (root / f'{name}_report.json').write_text(json.dumps(dict(service=dict(p2p=dict(connection=report, local_identity=peer.get('identity', ''))))),
                                                  encoding='utf-8')
    if offers:
        offer = dict(session_id=session, match_id=f'{session}:0', provider=provider, generation=1, expires_at=4102444800, server_count=2)
        (root / 'service.log').write_text(f'2026-10-03 INFO relay_offer_issued {json.dumps(offer)}\n', encoding='utf-8')
    return root


def judge_facts(signals, public=False, **changes):
    facts = dict(started='s', finished='f', states={}, identities={'edith': dict(status='PASS', head='a' * 40, executable_sha256='b' * 64)},
                 legs={}, ports={}, sessions=[SESSION], signals=signals,
                 offers_seen=None if public else [dict(session_id=SESSION, urls=['turn:turn.cloudflare.com:3478?transport=udp'], expires_at=4102444800)],
                 provider_calls=[] if public else [dict(at='2026-10-03 02:00:00 PM MST', epoch=1791066000.0, status=201)],
                 revokes=[204], minted=[dict(username='unit-minted', expires_at=4102444800, minted_at=1791066000)], public=public, ticks=1201,
                 ended_epoch=4000000000, overrides_cleared=True,
                 sanitize=[dict(box='edith', status='CLEAN', hits_before=0, hits_after=0), dict(box='here', status='CLEAN', hits_before=0, hits_after=0)],
                 fetched=['edith'], relay_hosts=None, coturn_address=None, fixed_pair=None, pair_ttl=None, cleanup=[], minted_at=None, clocks={})
    facts.update(changes)
    return facts


def full_judge(run, root, facts, directory_lines=()):
    import edith_cross
    names = [peer['name'] for peer in run['peers']]
    with mock.patch.object(edith_cross, 'pair_build_evidence', return_value=dict(passed=True)), \
            mock.patch.object(edith_cross, 'live_ticks', read_ticks), \
            mock.patch.object(match, 'registrant', lambda address: dict(address=address, cloudflare=True)), \
            mock.patch.object(match, 'public_directory_lines', return_value=list(directory_lines)), contextlib.redirect_stdout(io.StringIO()):
        return match.judge_run(FakeHarness(names), dict(name='unit'), run, root, facts, SecretBook())


CLOUDFLARE_SIGNALS = [('host', signal('str:h-6d7c0768', relay_line('104.30.150.145', 4001))),
                      ('client:aaaa1111bbbb2222cccc3333', signal('str:c-aaaa1111', relay_line('104.30.146.169', 4002)))]


def cloudflare_relay_row(folder: Path, missing=()):
    run = dict(name='cloudflare', relay='cloudflare', peers=[dict(name='host', box='edith', connection='RelayOnly'),
                                                             dict(name='client', box='edith', connection='RelayOnly')])
    root = judge_folder(folder / 'run', {'host': dict(route='relay', connection=7),
                                          'client': dict(route='relay', connection=8, identity='str:c-aaaa1111bbbb2222')}, missing=missing)
    return run, root, judge_facts(CLOUDFLARE_SIGNALS)


class GreenTipProbes(unittest.TestCase):
    """The probes the read of the green tip ran, as rows: G1-G7 plus the Cloudflare positive control."""

    # G1: no shape hit is excused because a repository tracks the same text.
    def test_g1_a_tracked_64_hex_login_is_a_secret_hit(self):
        login = json.dumps(dict(username=synthetic('g1-user'), credential=synthetic('g1-credential')))
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            (folder / 'run').mkdir()
            (folder / 'run' / 'offer.json').write_text(login, encoding='utf-8')
            unrelated = SecretBook()
            unrelated.add('backend-api_token', synthetic('unrelated-token'))
            status, receipt = lane_sweep(folder / 'run', folder, unrelated, tracked_repo(folder, login))
        self.assertEqual(status, 1, receipt['roots'][0])
        self.assertGreaterEqual(receipt['roots'][0]['hits'], 1)

    def test_g1_a_booked_username_in_a_menu_echo_is_found(self):
        user = synthetic('g1-menu-user', 12)
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            (folder / 'run' / 'host').mkdir(parents=True)
            (folder / 'run' / 'host' / 'stdout.log').write_text(f'[menu-script] set_text TextHostRelayUser {user} PASS\n', encoding='utf-8')
            secrets = SecretBook()
            secrets.add('fixed-username', user, **supported(secrets.add, scope='login-field'))
            status, receipt = lane_sweep(folder / 'run', folder, secrets, tracked_repo(folder, f'the {user} project\n'))
        self.assertEqual(status, 1, receipt['roots'][0])

    def test_g1_a_unicode_escaped_credential_is_found(self):
        credential = synthetic('g1-escaped-credential', 24)
        escaped = ''.join(f'\\u{ord(character):04x}' for character in credential)
        text = '{"username": "' + synthetic('g1-escaped-user', 16) + '", "credential": "' + escaped + '"}'
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            (folder / 'run').mkdir()
            (folder / 'run' / 'capture.json').write_text(text, encoding='utf-8')
            secrets = SecretBook()
            secrets.add('minted-credential', credential)
            status, receipt = lane_sweep(folder / 'run', folder, secrets, tracked_repo(folder, text))
        self.assertEqual(status, 1, receipt['roots'][0])

    # G2: no secret in any exception text; every exit path sanitizes.
    def run_patches(self, folder: Path, secret_store: list, start=None):
        real_init = match.LaneCoturn.__init__

        def init(coturn, book_):
            real_init(coturn, book_)
            secret_store.append(coturn.secret)
        patches = [mock.patch.object(match, 'LANE', 'unit-lane', create=True),
                   mock.patch.object(match, 'load_boxes', lambda trees, aliases=None: {'edith': FakeBox(), 'ally': FakeBox('ally')}),
                   mock.patch.object(match, 'identity', return_value=dict(status='PASS', head='a' * 40, executable_sha256='b' * 64, errors=[])),
                   mock.patch.object(match, 'choose_ports', return_value=list(range(48700, 48706))),
                   mock.patch.object(match, 'RETIRED_FIXED_CONF', folder / 'no-such.conf'),
                   mock.patch.object(match, 'sanitize_box', lambda box, root, book_, payload: dict(box=box.name, status='CLEAN', hits_before=0, hits_after=0)),
                   mock.patch.object(match, 'put_remote_text', lambda box, path, text: None),
                   mock.patch.object(match.LaneCoturn, '__init__', init),
                   mock.patch('edith_cross.harness', return_value=mock.MagicMock()), mock.patch('edith_cross.looped_input')]
        if start is not None:
            patches.append(mock.patch.object(match.LaneCoturn, 'start', start))
        return patches

    def test_g2_a_timeout_carrying_the_secret_leaves_no_copy(self):
        secrets_made = []

        def start(coturn):
            raise subprocess.TimeoutExpired(cmd=f'ssh Erol-Mac turnserver --static-auth-secret={coturn.secret}', timeout=60)
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            printed = io.StringIO()
            with contextlib.ExitStack() as stack:
                for patch in self.run_patches(folder, secrets_made, start):
                    stack.enter_context(patch)
                stack.enter_context(contextlib.redirect_stdout(printed))
                match.main(['--scenario', 'mp-relay-cloudflare', '--run', 'coturn', '--out', str(folder / 'out'),
                            '--box', 'host=edith', '--box', 'client=edith'])
            written = [path.read_text(encoding='utf-8', errors='replace') for path in (folder / 'out').rglob('*') if path.is_file()]
        self.assertTrue(secrets_made)
        self.assertNotIn(secrets_made[0], printed.getvalue())
        self.assertFalse([text for text in written if secrets_made[0] in text])

    def test_g2_an_interrupt_still_sweeps_here_and_revokes(self):
        swept, revoked = [], []

        class Observer:
            def __init__(self, host_name, book_):
                self.minted, self.session = [dict(username='unit-public-login', expires_at=4102444800)], SESSION

            def start(self):
                pass

            def stop(self):
                pass
        run = copy.deepcopy(next(run for run in scenario('mp-relay-hotspot')['runs'] if run['name'] == 'a-automatic-fallback'))
        with tempfile.TemporaryDirectory() as folder, contextlib.ExitStack() as stack:
            folder = Path(folder)
            for patch in self.run_patches(folder, []):
                stack.enter_context(patch)
            stack.enter_context(mock.patch.object(match, 'PublicObserver', Observer))
            stack.enter_context(mock.patch.object(match, 'wait_done', side_effect=KeyboardInterrupt()))
            stack.enter_context(mock.patch.object(match, 'sanitize_local', lambda root, book_: swept.append(root) or dict(box='here', status='CLEAN')))
            stack.enter_context(mock.patch.object(match, 'revoke_cloudflare', lambda config, names, *args, **kwargs: revoked.extend(names) or [204]))
            stack.enter_context(mock.patch('relay_secrets.read_turn_config', return_value=dict(turn_key_id='unit', api_token='unit')))
            stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
            boxes = {'edith': FakeBox(), 'ally': FakeBox('ally')}
            for box in boxes.values():
                box.remote.fail_on = '<every copy succeeds>'
            with self.assertRaises(KeyboardInterrupt):
                match.run_one(dict(name='mp-relay-hotspot'), run, folder / 'out', boxes, 1201, SecretBook())
        self.assertTrue(swept, 'the local sweep did not run')
        self.assertEqual(revoked, ['unit-public-login'])

    def test_g2_the_expanded_menu_is_blanked_after_a_failure_before_load(self):
        writes, pair = {}, ('1791066329:' + synthetic('g2-tag', 24), synthetic('g2-credential', 28))

        def start(coturn):
            coturn.port, coturn.pid = 3490, None

        class Quiet:
            def __init__(self, *args, **kwargs):
                self.pin, self.revokes, self.minted, self.signals, self.offers, self.provider_calls, self.box = 'a' * 64, [], [], [], [], [], args[0] if args else None

            def open(self):
                pass

            def close(self):
                pass

            def stop(self):
                pass

            def sessions(self):
                return []
        run = copy.deepcopy(next(run for run in scenario('mp-relay-cloudflare')['runs'] if run['name'] == 'fixed'))
        for peer in run['peers']:
            peer['box'] = 'edith'
        box = FakeBox()
        box.remote.fail_on = '<every copy succeeds>'
        box.remote.start_task = mock.MagicMock(side_effect=RuntimeError('the task refused to start (synthetic)'))
        with tempfile.TemporaryDirectory() as folder, contextlib.ExitStack() as stack:
            folder = Path(folder)
            for patch in self.run_patches(folder, [], start):
                stack.enter_context(patch)
            stack.enter_context(mock.patch.object(match, 'put_remote_text', lambda box_, path, text: writes.__setitem__(str(path), text)))
            stack.enter_context(mock.patch.object(match, 'coturn_rest_login', return_value=pair))
            stack.enter_context(mock.patch.object(match, 'box_lan_address', return_value='10.0.0.5'))
            for name in ('Bridge', 'Directory', 'Tunnel'):
                stack.enter_context(mock.patch.object(match, name, Quiet))
            stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
            verdict = match.run_one(dict(name='mp-relay-cloudflare'), run, folder / 'out', {'edith': box}, 1201, SecretBook())
        self.assertFalse(verdict['passed'])
        menus = {path: text for path, text in writes.items() if path.endswith('.menu.txt')}
        self.assertTrue(menus, 'no menu script was shipped')
        self.assertFalse([path for path, text in menus.items() if pair[0] in text or pair[1] in text], 'a shipped menu still holds the pair')

    # G3: exactly the row's table; contiguous history.
    def test_g3_a_correct_automatic_direct_row_passes(self):
        run = dict(name='automatic', relay='automatic', peers=[dict(name='host', box='edith', connection='Automatic'),
                                                               dict(name='client', box='edith', connection='Automatic')])
        srflx = 'candidate:2 1 udp 1694498815 203.0.113.20 51000 typ srflx'
        with tempfile.TemporaryDirectory() as folder:
            root = judge_folder(Path(folder) / 'run', {'host': dict(route='direct', connection=7),
                                                       'client': dict(route='direct', connection=8, identity='str:c-aaaa1111bbbb2222',
                                                                      report=dict(OPEN_REPORT, relayed=False))})
            verdict = full_judge(run, root, judge_facts([('host', signal('str:h-6d7c0768', srflx)),
                                                         ('client:aaaa1111bbbb2222cccc3333', signal('str:c-aaaa1111', srflx))]))
        self.assertTrue(verdict['passed'], {key: value for key, value in verdict['checks'].items() if not value})

    def test_g3_hotspot_a_with_every_required_check_true_passes(self):
        run = copy.deepcopy(next(run for run in scenario('mp-relay-hotspot')['runs'] if run['name'] == 'a-automatic-fallback'))
        at = lambda second: f'2026-10-03 02:{second // 60:02d}:{second % 60:02d} PM MST'
        offer = dict(session_id=SESSION, match_id=f'{SESSION}:0', provider='cloudflare', generation=1, expires_at=4102444800, server_count=2)
        bound = dict(OPEN_REPORT, remote_address='104.30.146.169:5000')
        with tempfile.TemporaryDirectory() as folder:
            root = judge_folder(Path(folder) / 'run', {'host': dict(route='relay', connection=7, report=bound),
                                                       'client': dict(route='relay', connection=8, identity='str:c-aaaa1111bbbb2222', report=bound)},
                                offers=False)
            (root / 'ally-tailscale.json').write_text(json.dumps([
                dict(step='before', at=at(0), backend_state='Running'), dict(step='down', at=at(1), exit_code=0, backend_state='Stopped'),
                dict(step='engine-started', at=at(2), peer='client', backend_state='Stopped'),
                dict(step='engine-ended', at=at(40), peer='client', backend_state='Stopped'),
                dict(step='up', at=at(41), exit_code=0, backend_state='Running')]), encoding='utf-8')
            (root / 'client-panel').mkdir()
            (root / 'client-panel' / 'net-ui-result.json').write_text(json.dumps({'pass': True, 'complete': True}), encoding='utf-8')
            # The host's match summary, where the service report writes it: the hotspot seat may be held, no other.
            report = json.loads((root / 'host_report.json').read_text(encoding='utf-8'))
            report['service']['last_match'] = dict(peers=[dict(name='host', holds=0), dict(name='client', holds=1)])
            (root / 'host_report.json').write_text(json.dumps(report), encoding='utf-8')
            verdict = full_judge(run, root, judge_facts([], public=True), [f'INFO relay_offer_issued {json.dumps(offer)}'])
        self.assertTrue(verdict['passed'], {key: value for key, value in verdict['checks'].items() if not value})

    def test_g3_the_cloudflare_relay_row_passes(self):
        with tempfile.TemporaryDirectory() as folder:
            verdict = full_judge(*cloudflare_relay_row(Path(folder)))
        self.assertTrue(verdict['passed'], {key: value for key, value in verdict['checks'].items() if not value})

    def test_g3_a_pair_missing_tick_600_fails(self):
        with tempfile.TemporaryDirectory() as folder:
            verdict = full_judge(*cloudflare_relay_row(Path(folder), missing=(600,)))
        self.assertFalse(verdict['passed'])
        self.assertFalse(verdict['checks']['full_history'])

    # G4: a closed report fails; one report and one retained sender entry per peer.
    def test_g4_a_closed_report_fails_a_three_peer_run(self):
        verdict = match.judge_relay(three_peer_run(client_connection=CLOSED_REPORT, reports={'client': CLOSED_REPORT, 'client2': OPEN_REPORT},
                                                   overrides_cleared=True))
        self.assertFalse(verdict['passed'])

    def test_g4_two_joiners_keep_two_reports_and_two_sender_entries(self):
        run = dict(name='unit-three', relay='cloudflare', peers=[dict(name='host', box='edith', connection='RelayOnly'),
                                                                 dict(name='client', box='edith', connection='RelayOnly'),
                                                                 dict(name='client2', box='ally', connection='RelayOnly')])
        with tempfile.TemporaryDirectory() as folder:
            root = judge_folder(Path(folder) / 'run', {'host': dict(route='relay', connection=7),
                                                       'client': dict(route='relay', connection=8, identity='str:c-aaaa1111bbbb2222'),
                                                       'client2': dict(route='relay', connection=9, identity='str:c-dddd4444eeee5555')})
            verdict = full_judge(run, root, judge_facts(three_peer_run()['signals']))
            retained = json.loads((root / 'signals-candidates.json').read_text(encoding='utf-8'))
        self.assertEqual(sorted(verdict['relay_evidence'].get('reports', {})), ['client', 'client2'])
        self.assertEqual(sorted({row.get('peer', row.get('sender')) for row in retained}), ['client', 'client2', 'host'])

    # G5: renewal, migration, the tunnel and the preconditions judged by time and identity.
    def test_g5_a_mint_before_the_half_life_fails(self):
        renewed = '[net-relay] relay login renewed on 1 live connection(s)'
        logs = {peer: receipts(SESSION) + '\n' + renewed + '\n' for peer in ('host', 'client')}
        first, ttl = 1791066000.0, 300

        def evidence(second_mint):
            calls = [dict(status=201, epoch=first, at='2026-10-03 02:00:00 PM MST'),
                     dict(status=201, epoch=second_mint, at='2026-10-03 02:02:25 PM MST')]
            timed = {peer: [[second_mint + 5, renewed]] for peer in logs}
            samples = {peer: [[first + 10, 600], [first + ttl + 20, 19000]] for peer in logs}
            return match.renewal_evidence(logs, calls, list(logs), ttl_s=ttl, **supported(
                match.renewal_evidence, frames_past_expiry=True, line_times=timed, samples=samples, first_expiry=first + ttl,
                clocks={peer: (0.0, 0.5) for peer in logs}, session=SESSION))
        self.assertFalse(evidence(first + 145)['passed'], 'a second mint five seconds before the half-life passed')
        self.assertTrue(evidence(first + 150)['passed'], evidence(first + 150)['reasons'])

    def test_g5_a_repeated_line_on_the_same_connection_is_no_migration(self):
        declared = '[net-match] Host left - relayproof-client is now hosting; boundary=600 round=1'
        seats = {'host': 'relayproof-host', 'client': 'relayproof-client', 'client2': 'relayproof-client2'}
        stale = {peer: receipts(SESSION) + '\n' + declared + '\n[net-ice] selected candidate=relay connection=7\n' for peer in ('client', 'client2')}
        fresh = {peer: receipts(SESSION) + '\n' + declared + '\n[net-ice] selected candidate=relay connection=9\n'
                       '[net-route] RouteAllowed route=relay allowed=1 connection=9\n' for peer in ('client', 'client2')}
        ticks = {'client': 1801, 'client2': 1801}
        self.assertFalse(match.migration_declarations(stale, ['client', 'client2'], SESSION, seats, ticks)['passed'])
        self.assertTrue(match.migration_declarations(fresh, ['client', 'client2'], SESSION, seats, ticks)['passed'])

    def test_g5_a_bracket_shorter_than_the_run_fails(self):
        rows = tunnel()
        up = next(row for row in rows if row['step'] == 'up')
        up['at'] = '2026-10-03 14:00:30 MST'  # back up 35 s before the engine ended, though listed after it
        self.assertTrue(match.tunnel_receipt(tunnel(), ['client'])['passed'])
        self.assertFalse(match.tunnel_receipt(rows, ['client'])['passed'])

    def test_g5_malformed_preconditions_are_refused(self):
        for state, gateway, mapped in (('Running: error reading status', '172.20.10.1', '100.64.1.2:5000'), ('Running', 'None', 'None'),
                                       ('Running', '172.20.10.1', 'mapped=None')):
            with self.subTest(state=state, gateway=gateway, mapped=mapped):
                self.assertEqual(match.hotspot_preflight(state, gateway, mapped)['verdict'], 'REFUSED')
        with mock.patch.dict(match.HOME, gateway='192.0.2.1', public='192.0.2.2'):
            self.assertEqual(match.hotspot_preflight('Running', '172.20.10.1', '100.64.1.2:5000')['verdict'], 'AWAY')
    def test_home_baseline_is_required_and_addresses_stay_private(self):
        with mock.patch.dict(match.HOME, gateway='', public=''):
            self.assertEqual(match.hotspot_preflight('Running', '198.51.100.1', '198.51.100.2:5000')['verdict'], 'REFUSED')
        with mock.patch.dict(match.HOME, gateway='192.0.2.1', public='192.0.2.2'):
            for gateway, mapped in [('192.0.2.1', '198.51.100.2:5000'), ('198.51.100.1', '192.0.2.2:5000')]:
                result = match.hotspot_preflight('Running', gateway, mapped)
                self.assertEqual(result['verdict'], 'HOME')
                self.assertNotIn(gateway, json.dumps(result))
                self.assertNotIn(mapped.split(':')[0], json.dumps(result))

    def test_process_log_redacts_a_private_address_before_retention(self):
        book = SecretBook()
        address = '192.0.2.28'
        book.add('owner-address', address)
        with tempfile.TemporaryDirectory() as folder, mock.patch.object(match, 'REDACTOR', book):
            path = Path(folder) / 'process.log'
            match.retain_redacted_process_log(io.StringIO(f'path uses {address}\nready\n'), path)
            self.assertEqual(path.read_text(), 'path uses <owner-address>\nready\n')


    # G6: limits are incomplete, never clean; a scrub keeps the span's representation.
    def test_g6_four_nested_gzips_are_incomplete(self):
        data = plain(LOGIN)
        for _ in range(4):
            data = gzip.compress(data)
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / 'nested.gz').write_bytes(data)
            scanned = book().scan([folder])
        self.assertEqual(scanned.get('status'), 'INCOMPLETE', scanned)

    def test_g6_a_raw_64_hex_login_is_scrubbed_and_verified_clean(self):
        import relay_secrets
        login = synthetic('g6-raw-hex-login')
        secrets = SecretBook()
        secrets.add('minted-username', login)
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / 'offer.json').write_text(json.dumps(dict(username=login)), encoding='utf-8')
            scrubbed = relay_secrets.sweep([folder], secrets.finder(), scrub=True)
            verify = relay_secrets.sweep([folder], secrets.finder())
            left = (Path(folder) / 'offer.json').read_text(encoding='utf-8')
        self.assertEqual(scrubbed['status'], 'CLEAN', scrubbed['scrubbed'])
        self.assertEqual(verify['status'], 'CLEAN', verify['files_with_secrets'])
        self.assertNotIn(login, left)

    # G7: no lane, model, reader or tool name in the code (the names are assembled so this row does not name them).
    def test_g7_no_names_in_the_code(self):
        import re
        names = ['as' + 'tra', 'op' + 'us', 'cl' + 'aude', 'anth' + 'ropic', 'fa' + 'ble', 'gr' + 'ok', 'ki' + 'mi', 'gp' + 't-', 'co' + 'dex',
                 'cur' + 'sor', 'de' + 'vin', 'swe' + '-2', 'son' + 'net', 'hai' + 'ku', 'independent ' + 'read']
        pattern = re.compile('|'.join(re.escape(name) for name in names), re.IGNORECASE)
        files = sorted(TOOLS.glob('relay_*.py')) + sorted(TOOLS.glob('relay_*.json')) + sorted((TOOLS / 'e2e').glob('mp-relay-*'))
        found = [f'{path.name}:{number}' for path in files for number, line in enumerate(path.read_text(encoding='utf-8').splitlines(), 1)
                 if pattern.search(line)]
        self.assertEqual(found, [])

if __name__ == '__main__':
    unittest.main()
