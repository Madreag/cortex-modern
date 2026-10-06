"""Detect boundary and ownership defects in the shared peer interface, without engines."""
import base64
import contextlib
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
import subprocess
import sys
from unittest.mock import patch

import spread_peers as spread
from test_named_spread import NamedRoutingTests


class ContractTests(unittest.TestCase):
    def test_compatible_pending_release_never_reads_the_retired_queue(self):
        released = []
        case = object.__new__(spread.Case)
        case.pending = [(Path('host-ticket'), 'host'), (Path('seat-ticket'), 'seat')]
        case.pending_peers = dict(host='host', seat='seat')
        case.transport_module = SimpleNamespace(worker=SimpleNamespace(facts=SimpleNamespace(release_reservation=lambda *args:released.append(args))))
        case.release_pending('host')
        case.release_pending()
        self.assertEqual(released, [])
        self.assertEqual(case.pending, [(Path('host-ticket'), 'host'), (Path('seat-ticket'), 'seat')])

    def test_linux_host_with_loopback_hostname_uses_its_native_interface(self):
        with patch.object(spread.sys, 'platform', 'linux'), patch.object(spread.shutil, 'which', return_value=None), \
                patch.object(spread.socket, 'getaddrinfo', return_value=[(None, None, None, None, ('127.0.1.1', 0))]), \
                patch.object(spread.subprocess, 'run', side_effect=[SimpleNamespace(returncode=1, stdout=''),
                                                                  SimpleNamespace(returncode=0, stdout='192.168.50.128 127.0.0.1 ')]) as read:
            self.assertEqual(spread.native_address_probe(), '192.168.50.128')
        self.assertEqual(read.call_args.args[0], ['hostname', '-I'])

    def test_linux_route_source_wins_over_a_docker_bridge(self):
        with patch.object(spread.sys, 'platform', 'linux'), patch.object(spread.shutil, 'which', return_value=None), \
                patch.object(spread.socket, 'getaddrinfo', return_value=[(None, None, None, None, ('172.17.0.1', 0))]), \
                patch.object(spread.subprocess, 'run', return_value=SimpleNamespace(returncode=0, stdout='[{"prefsrc":"192.168.50.36"}]')) as read:
            self.assertEqual(spread.native_address_probe(), '192.168.50.36')
        self.assertEqual(read.call_args.args[0], ['ip', '-j', 'route', 'get', '192.0.2.1'])

    def place_fake(self, peers, pins=None):
        case = object.__new__(spread.Case)
        case.registry, case.out, case.control = Path('fake.json'), Path('fake'), Path('fake/control')
        case.peers, case.names, case.pins = peers, [peer.name for peer in peers], pins or {}
        case.id = 'fake-case'
        case.members, case.pending, case.match, case.lane, case.peer_ports = {}, [], spread.Match(51580), 'fake', {}
        case.stack = contextlib.ExitStack()
        self.addCleanup(case.stack.close)
        boxes = [dict(name=name, kind='local' if name == 'EROL-PC' else 'windows-task', os='windows',
                      hostname=name, engines_max=6) for name in ('EROL-PC', 'EDITH', 'Z13')]
        claims, requests = {}, []
        def rpc(box, action, value, **kwargs):
            self.assertEqual(action, 'claim')
            needs, request = SimpleNamespace(**value['needs']), value['request']
            name = box['name']
            busy = claims.get(name, [])
            if needs.alone and busy or any(item.alone for item in busy):
                raise RuntimeError('alone run needs an idle box')
            if len(busy) + needs.engines > box['engines_max']:
                raise RuntimeError('live engine capacity is in use')
            claims.setdefault(name, []).append(needs)
            requests.append(request)
            return dict(token=request['token'])
        case.backend = lambda: SimpleNamespace(rpc=rpc, control=lambda box:'fake/control',
                                               probe=lambda box,**kwargs:dict(hostname=box['hostname']))
        def retired(*args):
            raise AssertionError('retired placement was consulted')
        case.pool = SimpleNamespace(load_registry=lambda path:dict(boxes=boxes), static_reason=lambda *args:None,
                                    Needs=lambda **kwargs:SimpleNamespace(**(dict(alone=False, only_box=None, excluded=(), share_ok=False, reviewed=False, held=False) | kwargs)),
                                    queue_root=retired, write_ticket=retired, candidates=retired, priority_blocker=retired)
        facts = SimpleNamespace(machine_name=lambda:'controller', process_start=lambda pid:1, release_reservation=lambda *args:None)
        case.transport_module = SimpleNamespace(worker=SimpleNamespace(facts=facts, mutex=lambda *args,**kwargs:contextlib.nullcontext()))
        case.refuse = lambda name, box, reason:spread.SpreadRefusal(f'{name} on {box}: {reason}')
        case.allocate()
        self.assertTrue(all(request['case_id'] == case.id for request in requests))
        self.assertEqual({request['peer_id'] for request in requests}, set(case.names))
        return case, claims

    def test_shareable_screen_peers_use_one_fitting_box_without_reviewed_peer(self):
        case, claims = self.place_fake([spread.Peer('host', reviewed=True), spread.Peer('a'), spread.Peer('b'), spread.Peer('c')],
                                      dict(host='EDITH', a='EROL-PC', b='EROL-PC', c='Z13'))
        self.assertEqual([item[0]['name'] for item in case.members.values()], ['EDITH', 'EROL-PC', 'EROL-PC', 'Z13'])
        self.assertEqual(len(claims['EROL-PC']), 2)
        self.assertTrue(all(not needs.alone for needs in claims['EROL-PC']))
        self.assertTrue(all('enqueued_at' not in request for _, _, request, _ in case.members.values()))

    def test_only_the_lead_can_explicitly_share_all_eligible_peers(self):
        case, claims = self.place_fake([spread.Peer('a'), spread.Peer('b')], dict(a='EROL-PC', b='EROL-PC'))
        self.assertEqual({item[0]['name'] for item in case.members.values()}, {'EROL-PC'})
        self.assertEqual(len(claims['EROL-PC']), 2)

    def test_retained_runtime_keeps_settings_and_private_overlay_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            settings = root/'Userdata/Settings.ini'
            settings.parent.mkdir()
            settings.write_bytes(b'SettingsMan\n\tNetworkDisplayName = retained\n')
            overlay = root/'Data/Base.rte/GUIs/old.ini'
            overlay.parent.mkdir(parents=True)
            overlay.write_bytes(b'old GUI bytes\x00\xff')
            files = spread.retained_files(root)
            self.assertEqual(files[Path('Userdata/Settings.ini')], settings.read_bytes())
            self.assertEqual(files[Path('Data/Base.rte/GUIs/old.ini')], overlay.read_bytes())
            repo = root/'repo'
            (repo/'Data/Base.rte/GUIs').mkdir(parents=True)
            source = repo/'Data/Base.rte/GUIs/old.ini'
            source.write_bytes(b'current GUI')
            runtime = spread.native_retained_runtime(repo, root/'native/host', files, [])
            self.assertEqual((runtime/'Data/Base.rte/GUIs/old.ini').read_bytes(), overlay.read_bytes())
            self.assertEqual(source.read_bytes(), b'current GUI')

    def test_retained_incomplete_runtime_and_private_ticket_refuse(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with self.assertRaisesRegex(ValueError, 'retained runtime is incomplete'):
                spread.retained_files(root)
            (root/'Userdata').mkdir()
            (root/'Userdata/Settings.ini').write_text('SettingsMan\n')
            (root/'Userdata/host.ticket').write_bytes(b'private fixture')
            with self.assertRaisesRegex(spread.SpreadRefusal, 'existing credential delivery channel'):
                spread.retained_files(root)

    def test_real_menu_pairs_keep_every_original_page_assertion_and_capture(self):
        import test_menu_readback as menu
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for case in ('net-chat', 'lobby-name'):
                original, _ = menu.scripts(case, 51580, root, '1280x720')
                texts, probes = menu.spread_menu_scripts(case, original, 51580, root)
                self.assertEqual(original['host'].count('dump_'), texts['host'].count('dump_'))
                self.assertTrue(all(line in texts['host'] for line in original['host'].splitlines() if line.startswith('assert_')))
                self.assertIn('wait_connected 2 60', texts['client'])
                self.assertIn('wait_label LabelLobbyPlayer0 ', texts['client'])
                self.assertNotIn('dump_', texts['client'])
                self.assertEqual(probes['host']['steps'][0]['screen'], 'MultiplayerScreen')
                if case == 'net-chat':
                    self.assertIn('hello-from-client', texts['host'])
                    self.assertIn('hello-from-host', texts['client'])
                    self.assertIn('host-received-client', texts['client'])

    def test_only_matching_live_pool_share_admits_an_existing_lane_engine(self):
        import cross_peers as cross
        box = dict(name='EROL-PC', kind='windows-local', executable='D:/own/game.exe', scratch='D:/own')
        peer = dict(case_id='case', peer_id='seat')
        claim = dict(case_id='case', peer_id='seat', share_ok=True, engines=1)
        assignment = dict(claim='owned-claim', box=box, executable=box['executable'])
        module = SimpleNamespace(assignment_for_launch=lambda:assignment, box_facts=SimpleNamespace(read_reservation=lambda path:claim))
        with patch.dict(sys.modules, pool_run=module), patch.object(cross,'inventory_guard',return_value=None), \
                patch.object(cross,'box_load',return_value=[dict(Name='Cortex Command',ExecutablePath=box['executable'])]), \
                patch.object(cross,'scratch_bytes',return_value=0):
            with self.assertRaisesRegex(RuntimeError,'an engine of this lane is already running'):
                cross.assert_box_guard(box)
            cross.assert_box_guard(box,pool_peer=peer)
            for change in (dict(case_id='other'),dict(peer_id='other'),dict(share_ok=False),dict(reviewed=True),dict(held=True),dict(alone=True)):
                before = dict(claim)
                claim.update(change)
                with self.assertRaisesRegex(RuntimeError,'an engine of this lane is already running'):
                    cross.assert_box_guard(box,pool_peer=peer)
                claim.clear(); claim.update(before)

    def test_unshareable_and_quiet_peers_reserve_distinct_idle_boxes(self):
        case, claims = self.place_fake([spread.Peer('host', share_ok=False), spread.Peer('seat', quiet=True)],
                                      dict(host='EROL-PC', seat='EDITH'))
        self.assertEqual(len({item[0]['name'] for item in case.members.values()}), 2)
        self.assertTrue(claims[case.members['seat'][0]['name']][0].alone)
        self.assertTrue(all(not needs.share_ok for rows in claims.values() for needs in rows))
        self.assertFalse(case.peers[1].share_ok)

    def test_reviewed_and_held_peers_cannot_share_even_when_requested(self):
        peers = [spread.Peer('host', reviewed=True, share_ok=True), spread.Peer('held', held=True, share_ok=True)]
        self.assertTrue(all(not peer.share_ok for peer in peers))
        with self.assertRaisesRegex(spread.SpreadRefusal, 'TWO PEERS ON ONE BOX WITHOUT share_ok'):
            self.place_fake(peers, dict(host='EROL-PC', held='EROL-PC'))

    def test_reviewed_readback_uses_the_named_box_without_choosing(self):
        case, _ = self.place_fake([spread.Peer('host', reviewed=True, readback=True), spread.Peer('a'), spread.Peer('b')],
                                  dict(host='EDITH', a='EROL-PC', b='EROL-PC'))
        self.assertEqual(case.members['host'][0]['name'], 'EDITH')
        self.assertEqual(case.members['a'][0]['name'], 'EROL-PC')
        self.assertEqual(case.members['b'][0]['name'], 'EROL-PC')

    def test_worked_example_retains_wrong_peer_port_after_video_parses(self):
        import argparse
        import spread_example as example
        received = []
        def video_main():
            parser = argparse.ArgumentParser()
            spread.add_arguments(parser)
            options, _ = parser.parse_known_args()
            spread.configure(options)
            received.extend(options.peer_port)
            return 2
        with tempfile.TemporaryDirectory() as directory, patch.object(sys, 'argv', ['spread_example.py', '--out', directory,
                '--peer-boxes', 'host=EROL-PC,seat2=Linux', '--pool-dispatcher', 'dispatcher.py', '--port', '51580', '--port-block', '51580-51589',
                '--peer-port', 'seat2=51581']), patch.object(example.video, 'main', video_main), patch.object(example.video, 'run_one'):
            self.assertEqual(example.main(), 2)
        self.assertEqual(received, ['seat2=51581'])
        spread.configure(None)

    def test_worked_example_uses_one_shared_case_and_the_original_staging(self):
        import spread_example as example
        case = SimpleNamespace(result=lambda:dict(peer_boxes=dict(host='EROL-PC', client='Linux')))
        calls = []
        def run_case(repo, out, peers, match, **kwargs):
            calls.append(kwargs['peer_boxes'])
            self.assertEqual([peer.os for peer in peers], ['windows', 'any'])
            return dict(driver_result=kwargs['drive'](case))
        def video_main():
            value = example.video.run_one(SimpleNamespace(repo=Path('.'), size='960x540', port=51580),
                                          dict(size='960x540', timeout_s=170), dict(name='run0'), 0, Path('.'))
            self.assertEqual(value['peer_boxes'], dict(host='EROL-PC', client='Linux'))
            self.assertTrue(all(peer['record']['topology']=='spread' and peer['record']['box']==peer['box'] for peer in value['peers']))
            return 0
        with tempfile.TemporaryDirectory() as directory, patch.object(sys, 'argv', ['spread_example.py', '--out', directory,
                '--peer-boxes', 'host=EROL-PC,seat2=Linux', '--port', '51580', '--port-block', '51580-51589']), \
                patch.object(example, 'run_case', side_effect=run_case), patch.object(example.video, 'main', video_main), \
                patch.object(example.video, '_run_one', return_value=dict(peers=[dict(peer='host',record={}),dict(peer='client',record={})])) as staged, \
                patch.object(example.video, 'run_one'):
            self.assertEqual(example.main(), 0)
            staged.assert_called_once()
        self.assertEqual(calls, ['host=EROL-PC,seat2=Linux'])
        spread.configure(None)

    def test_pool_control_bootstrap_needs_no_caller_repository_imports(self):
        script = "import pathlib,sys,types; p=pathlib.Path(sys.argv[1]); m=types.ModuleType('spread_peers'); m.__file__=str(p); sys.modules[m.__name__]=m; exec(compile(p.read_text(encoding='utf-8'),str(p),'exec'),m.__dict__)"
        done = subprocess.run([sys.executable, '-I', '-c', script, str(Path(spread.__file__).resolve())], capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr)

    def test_peer_cannot_hide_two_engines_on_one_machine(self):
        with self.assertRaisesRegex(ValueError, "exactly one"):
            spread.Peer("host", engines=2)

    def test_safe_peer_name(self):
        with self.assertRaises(ValueError):
            spread.Peer("../owner")

    def test_declared_unicode_output_alias_preserves_the_logical_peer(self):
        with tempfile.TemporaryDirectory() as temporary:
            case = object.__new__(spread.Case)
            case.out = Path(temporary).resolve()
            alias = '\u00e9' * 64
            peer = spread.Peer('host', output_name=alias)
            case.output_names, case.members, case.runs = {peer.name: peer.output_name}, {'host': ()}, {}
            with patch.object(spread, 'Run') as factory:
                case.make_run(case.out, [], case.out / alias)
            self.assertEqual(factory.call_args.kwargs['role'], 'host')

    def test_output_alias_cannot_escape_the_case(self):
        with self.assertRaises(ValueError):
            spread.Peer('host', output_name='../owner')

    def test_legacy_script_bytes_are_not_transcoded(self):
        data = b'mark ' + bytes(range(128, 256)) + b'\nwait_file D:\\mx\\lane\\ready.json 5000\n'
        mapped = spread.map_script(data, [('D:\\mx\\lane', '/native/lane')])
        self.assertEqual(mapped, data.replace(b'D:\\mx\\lane\\ready.json', b'/native/lane/ready.json'))

    def test_control_port_is_not_game_port(self):
        with self.assertRaises(ValueError):
            spread.Match(46630, 46630)

    def test_box_aliases_preserve_actual_peer_names(self):
        values = spread.pairs("host=ONE,seat2=TWO,Dee=THREE")
        self.assertEqual(spread.role_value(values, ["Host", "Ana", "Dee"], "Ana"), "TWO")
        self.assertEqual(spread.role_value(values, ["Host", "Ana", "Dee"], "Dee"), "THREE")

    def test_case_alias_cannot_overwrite_box_assignment(self):
        with self.assertRaises(ValueError):
            spread.pairs("host=ONE,HOST=TWO")

    def test_windows_paths_inside_probe_json_map_to_native_root(self):
        text = json.dumps({"path": "D:\\mx\\lane\\host_probe\\done.json"})
        mapped = spread.map_text(text, [("D:\\mx\\lane", "/native/lane")])
        self.assertEqual(json.loads(mapped)["path"], "/native/lane/host_probe/done.json")

    def test_script_and_argument_paths_map_the_whole_windows_tail(self):
        mappings = [("D:\\mx\\lane", "/native/lane")]
        self.assertEqual(spread.map_text("D:\\mx\\lane\\host\\feel", mappings), "/native/lane/host/feel")
        self.assertEqual(spread.map_text("wait_file D:\\mx\\lane\\ready.json 5000\n", mappings),
                         "wait_file /native/lane/ready.json 5000\n")

    def test_probe_path_mapping_preserves_unrelated_regex_escapes(self):
        text = json.dumps({"path": "D:\\mx\\lane\\done.json", "regex": r"\d+\s+"})
        self.assertEqual(json.loads(spread.map_text(text, [("D:\\mx\\lane", "/native/lane")]))["regex"], r"\d+\s+")

    def test_mapping_prefers_case_input_over_case_root(self):
        result = spread.map_text("D:/mx/lane/input.txt", [("D:/mx/lane", "/root"), ("D:/mx/lane/input.txt", "/input/schedule.txt")])
        self.assertEqual(result, "/input/schedule.txt")

    def test_signals_include_probe_and_controller_rendezvous(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            directory = root/"Host_probe"
            directory.mkdir()
            (directory/"probe.json").write_text(json.dumps({"steps": [{"op": "signal", "name": "request-seen"},
                                                                          {"op": "wait_file", "path": str(root/"newcomer-frozen.json")}] }))
            signals = spread.declared_signals(root)
            self.assertIn("Host_probe/request-seen.json", signals)
            self.assertIn("newcomer-frozen.json", signals)
            self.assertIn("Host_probe/done.json", signals)

    def test_publish_rejects_undeclared_or_escaping_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            data = base64.b64encode(b"{}").decode()
            with self.assertRaisesRegex(ValueError, "declared"):
                spread.publish_signals(Path(temporary), {"owner.json": data}, {"done.json"})
            with self.assertRaises(ValueError):
                spread.publish_signals(Path(temporary), {"../owner.json": data}, {"../owner.json"})

    def test_partial_signal_is_not_published(self):
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(ValueError):
                spread.publish_signals(Path(temporary), {"done.json": base64.b64encode(b"{").decode()}, {"done.json"})
            self.assertFalse((Path(temporary)/"done.json").exists())

    def test_declared_empty_presence_marker_keeps_its_exact_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            spread.publish_signals(Path(temporary), {"ready.json": ""}, {"ready.json"})
            self.assertEqual((Path(temporary)/"ready.json").read_bytes(), b"")

    def test_presence_markers_are_declared_and_keep_opaque_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'menu.txt').write_text('touch_file client_done.mark\nwait_file client_done.mark 30\n')
            self.assertIn('client_done.mark', spread.declared_signals(root))
            data = b'ready\n'
            spread.publish_signals(root, {'client_done.mark': base64.b64encode(data).decode()}, {'client_done.mark'})
            self.assertEqual((root / 'client_done.mark').read_bytes(), data)

    def test_explicit_direct_mode_retains_its_host_address_without_directory(self):
        case = object.__new__(spread.Case)
        case.names = ['host']
        case.match = spread.Match(51580, parameters={'network': 'direct', 'host_address': '100.64.1.2'})
        case.members = {'host': ({'name': 'ONE'}, {}, {}, None)}
        case.connect_directory()
        self.assertIsNone(case.directory)
        self.assertEqual(case.host_address, '100.64.1.2')

    def test_direct_mode_rejects_a_loopback_host(self):
        case = object.__new__(spread.Case)
        case.names = ['host']
        case.match = spread.Match(51580, parameters={'network': 'direct', 'host_address': '127.0.0.1'})
        case.members = {'host': ({'name': 'ONE'}, {}, {}, None)}
        case.refuse = lambda name, box, reason: spread.SpreadRefusal(f'{name} on {box}: {reason}')
        with self.assertRaisesRegex(spread.SpreadRefusal, 'real machine'):
            case.connect_directory()

    def test_direct_mode_maps_only_loopback_inputs_and_preserves_ice_off(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root/'client-stage').mkdir()
            menu = root/'client-stage/menu.txt'
            menu.write_bytes(b'settext TextJoinAddress 127.0.0.1\nsettext TextJoinAddress invalid-seat\n')
            specs = []
            backend = SimpleNamespace(rpc=lambda box, action, body: specs.append(body['value']), launch=lambda *args: None)
            claim = dict(case_root='/native/case', repo='/native/repo', root='/owned', control='/control', exe_sha256='same')
            case = SimpleNamespace(id='fake-case', out=root, peer_ports={}, names=['host', 'client'], match=spread.Match(51580),
                                   directory=None, network='direct', host_address='100.64.1.2',
                                   members={'client': ({'name': 'SEAT', 'os': 'windows'}, claim, {}, backend)},
                                   peers=[spread.Peer('client')], signals=lambda: [], release_pending=lambda *args: None,
                                   transport_module=SimpleNamespace(Transport=SimpleNamespace(native_claim=lambda value: value)))
            run = object.__new__(spread.Run)
            run.retained = None
            run.case, run.role, run.output_name = case, 'client', 'client'
            run.repo, run.cwd, run.started = root/'repo', root/'client/runtime', False
            run.argv = ['engine', '-headless', '-net-ice', 'off', '-net-join', 'localhost', '-net-port', '51580']
            run.env, run.expected, run.fixtures, run.timeout = {}, [], [], 30
            run.start()
            spec = specs[0]
            self.assertEqual(spec['args'], ['-net-ice', 'off', '-net-join', '100.64.1.2', '-net-port', '51580'])
            self.assertEqual(base64.b64decode(spec['files']['client-stage/menu.txt']),
                             b'settext TextJoinAddress 100.64.1.2\nsettext TextJoinAddress invalid-seat\n')

    def test_blocked_port_is_a_declared_test_port(self):
        with self.assertRaisesRegex(ValueError, 'declared test ports'):
            spread.Peer('seat', block_udp=(80,))

    def test_declared_binary_match_config_keeps_every_byte(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = root / 'launch-config.bin'
            data = bytes(range(256)) + b'\x00\xff\n\r\x80'
            config.write_bytes(data)
            specs = []
            backend = SimpleNamespace(rpc=lambda box, action, body:specs.append(body['value']), launch=lambda *args:None)
            claim = dict(case_root='/native/case', repo='/native/repo', root='/owned', control='/control', exe_sha256='same')
            run = object.__new__(spread.Run)
            run.retained = None
            run.role, run.output_name, run.started = 'host', 'host', False
            run.repo, run.cwd = root/'repo', root/'host/runtime'
            run.argv = ['engine', '-headless', '-net-match-service-config', str(config)]
            run.env, run.expected, run.fixtures, run.timeout = {}, [], [], 30
            run.case = SimpleNamespace(id='fake-case', out=root, members={'host':({'name':'HOST'},claim,{},backend)}, names=['host'], peer_ports={},
                match=spread.Match(51580), directory=None, peers=[spread.Peer('host')], signals=lambda:[], release_pending=lambda *args:None,
                transport_module=SimpleNamespace(Transport=SimpleNamespace(native_claim=lambda value:value)))
            run.start()
            self.assertEqual(base64.b64decode(specs[0]['files']['launch-config.bin']), data)
            self.assertEqual(specs[0]['args'], ['-net-match-service-config', '/native/case/launch-config.bin'])

    def test_native_submit_refusal_names_the_peer_and_box_before_start(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            def launch(*args):
                raise RuntimeError('native memory 4.4 GB is below floor 4.5 GB')
            backend = SimpleNamespace(rpc=lambda *args:None, launch=launch)
            claim = dict(case_root='/native', repo='/repo', root='/owned', control='/control', exe_sha256='same')
            run = object.__new__(spread.Run)
            run.retained = None
            run.role, run.output_name, run.started, run.argv = 'seat', 'seat', False, ['engine','-headless']
            run.cwd, run.repo, run.env, run.expected, run.fixtures, run.timeout = root/'seat/runtime', root/'repo', {}, [], [], 30
            run.case = SimpleNamespace(id='fake-case', members={'seat':({'name':'REMOTE'},claim,{},backend)}, names=['seat'], peer_ports={},
                                       match=spread.Match(51580), directory=None, out=root, peers=[spread.Peer('seat')], signals=lambda:[],
                                       transport_module=SimpleNamespace(Transport=SimpleNamespace(native_claim=lambda value:value)),
                                       refuse=lambda peer,box,reason:spread.SpreadRefusal(f'spread peer {peer} on {box}: {reason}'))
            with self.assertRaisesRegex(spread.SpreadRefusal, 'spread peer seat on REMOTE: native memory 4.4 GB is below floor 4.5 GB'):
                run.start()
            self.assertFalse(run.started)
            self.assertTrue(run.launch_attempted)

    def test_native_holder_refusal_preserves_its_reason_without_other_log_bytes(self):
        log = 'private-value\n[box-hold] waiting: earlier owned job settles\n[box-hold] REFUSED: no turn within the wait\n'
        reason = spread.native_refusal(dict(reason='native launch refused', exit_code=3), log)
        self.assertEqual(reason, '[box-hold] waiting: earlier owned job settles\n[box-hold] REFUSED: no turn within the wait')
        self.assertNotIn('private-value', reason)

    def test_caller_directory_forwards_the_clients_own_proxy_without_replacing_it(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            descriptor = dict(DIRECTORY_URL='127.0.0.1:51581', DIRECTORY_PIN='a'*64, DIRECTORY_ROOT=str(root))
            case = object.__new__(spread.Case)
            case.match = spread.Match(51580, parameters=dict(directory=descriptor, peer_directory_urls={'client':'127.0.0.1:51582'}, join_by_session=False))
            case.names, case.peers, case.control, case.tunnels = ['host', 'client'], [spread.Peer('host'), spread.Peer('client')], root, []
            case.members = {'host': ({'name':'HOST', 'kind':'local'}, {'directory_port':51585}, {}, None),
                            'client': ({'name':'SEAT', 'kind':'windows-task', 'ssh':'native-seat'}, {'directory_port':51586}, {}, None)}
            with patch.object(spread.subprocess, 'Popen', return_value=SimpleNamespace(poll=lambda: None)) as popen, patch.object(spread.time, 'sleep'):
                case.connect_directory()
            for process, log in case.tunnels:
                log.close()
            self.assertEqual(case.members['host'][1]['signal_port'], 51581)
            self.assertEqual(case.members['client'][1]['signal_port'], 51586)
            self.assertIn('127.0.0.1:51586:127.0.0.1:51582', popen.call_args.args[0])
            self.assertEqual(case.directory['DIRECTORY_PIN'], descriptor['DIRECTORY_PIN'])
            self.assertEqual(descriptor, dict(DIRECTORY_URL='127.0.0.1:51581', DIRECTORY_PIN='a'*64, DIRECTORY_ROOT=str(root)))
            self.assertTrue(case.directory['preserve_settings'])

    def test_caller_directory_rejects_credentials_or_a_nonlocal_service(self):
        for endpoint in ['https://user:password@localhost:51581', 'https://remote.example:51581', 'http://localhost:51581']:
            with self.assertRaisesRegex(ValueError, 'loopback TLS test endpoint'):
                spread.directory_endpoint(endpoint)

    def test_named_native_refusal_does_not_probe_or_choose_another_box(self):
        events = []
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            facts = SimpleNamespace(machine_name=lambda:'controller', process_start=lambda pid:1,
                                    release_reservation=lambda path, token:events.append(('release',path,token)))
            case = object.__new__(spread.Case)
            case.registry, case.names, case.members, case.pending, case.control, case.out = root/'catalog.json', ['host'], {}, [], root, root
            case.peers, case.pins, case.match, case.lane, case.peer_ports = [spread.Peer('host', reviewed=True, quiet=True, lane='in-match')], dict(host='ONE'), spread.Match(51580), 'case', {}
            case.id = 'fake-case'
            case.stack = contextlib.ExitStack()
            def probe(box, **kwargs):
                events.append(('probe', box['name']))
                return {}
            def claim(box, action, value, **kwargs):
                events.append(('claim', box['name'], value['request']['label']))
                raise RuntimeError('alone run needs an idle box')
            case.pool = SimpleNamespace(load_registry=lambda path:{'boxes':[{'name':'ONE','kind':'local','os':'windows'},
                                                                           {'name':'FREE','kind':'local','os':'windows'}]},
                                        Needs=lambda **kwargs:SimpleNamespace(**kwargs), static_reason=lambda *args:None)
            case.transport_module = SimpleNamespace(worker=SimpleNamespace(facts=facts, mutex=lambda *args,**kwargs:contextlib.nullcontext()))
            case.backend = lambda:SimpleNamespace(probe=probe,rpc=claim)
            case.refuse = lambda peer, box, reason:spread.SpreadRefusal(f'{peer} on {box}: {reason}')
            with self.assertRaisesRegex(spread.SpreadRefusal, 'alone run needs an idle box'):
                case.allocate()
            case.stack.close()
            self.assertEqual(events, [('probe', 'ONE'), ('claim', 'ONE', 'in-match: spread')])
            self.assertEqual(case.pending, [])

    def test_credentials_and_tickets_do_not_travel_as_evidence(self):
        for name in (".env", "key.pem", "host.ticket", "id_ed25519"):
            self.assertFalse(spread.public_file(Path(name)))

    def test_default_proof_mark(self):
        self.assertEqual(spread.topology(count=4), "single-box: not proof")

    def test_declared_game_port_block_bounds_all_case_ports(self):
        self.assertEqual(spread.check_port_block(51580, '51580-51589', 10), (51580, 51589))
        for port, block, count in ((51580, '51580-51589', 11), (51579, '51580-51589', 1), (80, '80-89', 1),
                                   (65535, '65535-65536', 1), (51580, '51580:51589', 1)):
            with self.assertRaises(ValueError):
                spread.check_port_block(port, block, count)

    def cohort(self, client_hash="same"):
        class Transport:
            def __init__(self, digest):
                self.snapshot = None
                self.digest = digest
            def prepare(self, box, claim, request):
                if self.snapshot is None:
                    self.snapshot = {"head": "frozen"}
                claim.update(exe_sha256=self.digest, head=self.snapshot["head"])
            def committed_inputs(self):
                self.snapshot = {"head": "frozen"}
                return self.snapshot
        case = object.__new__(spread.Case)
        case.names = ["host", "client"]
        case.lane = "example"
        case.id = "unique"
        case.match = spread.Match(51580)
        case.guard = lambda: None
        case.members = {name: ({"name": name, "os": "windows", "scratch": "D:/mx"}, {"ports": [47660, 47664]}, {}, Transport(digest))
                        for name, digest in (("host", "same"), ("client", client_hash))}
        return case

    def test_every_peer_uses_one_frozen_snapshot(self):
        case = self.cohort()
        case.prepare()
        self.assertIs(case.members["host"][3].snapshot, case.members["client"][3].snapshot)

    def test_changed_binary_between_shipments_is_refused(self):
        with self.assertRaisesRegex(spread.SpreadRefusal, "between peer shipments"):
            self.cohort("changed").prepare()

    def lever_handle(self, os_name="windows"):
        class Transport:
            def rpc(self, box, action, body):
                self.target = box["name"]
                self.body = body
        backend = Transport()
        run = object.__new__(spread.Run)
        run.role = "seat2"
        run.started, run.finished = True, False
        run.native_progress = {"pid": 77}
        def synchronize(**kwargs):
            run.native_progress["action"] = {**backend.body["value"], "box": backend.target, "pid": 77}
        def refuse(peer, box, reason):
            return spread.SpreadRefusal(f"{peer} on {box}: {reason}")
        run.case = SimpleNamespace(peers=[spread.Peer("seat2", held=True)], members={"seat2": ({"name": "REMOTE", "os": os_name}, {"root": "/owned"}, {}, backend)},
                                   synchronize=synchronize, refuse=refuse)
        return run, backend

    def test_hold_lever_targets_the_held_peers_native_box(self):
        run, backend = self.lever_handle()
        receipt = run.suspend()
        self.assertEqual(backend.target, "REMOTE")
        self.assertEqual(backend.body["path"], "/owned/action.json")
        self.assertEqual((receipt["action"], receipt["box"], receipt["pid"]), ("suspend", "REMOTE", 77))

    def test_windows_hold_is_not_silently_replaced_on_posix(self):
        run, backend = self.lever_handle("linux")
        with self.assertRaisesRegex(spread.SpreadRefusal, "seat2 on REMOTE: Windows process suspension is unavailable"):
            run.suspend()
        self.assertFalse(hasattr(backend, "target"))


if __name__ == "__main__":
    unittest.main()
