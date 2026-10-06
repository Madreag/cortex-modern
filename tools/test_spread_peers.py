"""Detect boundary and ownership defects in the shared peer interface, without engines."""
import base64
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
import subprocess
import sys
from unittest.mock import patch

import spread_peers as spread


class ContractTests(unittest.TestCase):
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
            case = SimpleNamespace(out=root, peer_ports={}, names=['host', 'client'], match=spread.Match(51580),
                                   directory=None, network='direct', host_address='100.64.1.2',
                                   members={'client': ({'name': 'SEAT', 'os': 'windows'}, claim, {}, backend)},
                                   peers=[spread.Peer('client')], signals=lambda: [],
                                   transport_module=SimpleNamespace(Transport=SimpleNamespace(native_claim=lambda value: value)))
            run = object.__new__(spread.Run)
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

    def test_credentials_and_tickets_do_not_travel_as_evidence(self):
        for name in (".env", "key.pem", "host.ticket", "id_ed25519"):
            self.assertFalse(spread.public_file(Path(name)))

    def test_default_proof_mark(self):
        self.assertEqual(spread.topology(count=4), "single-box: not proof")

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
        run.case = SimpleNamespace(members={"seat2": ({"name": "REMOTE", "os": os_name}, {"root": "/owned"}, {}, backend)},
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
