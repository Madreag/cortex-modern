"""Detect boundary and ownership defects in the shared peer interface, without engines."""
import base64
import json
from pathlib import Path
import tempfile
import unittest

import spread_peers as spread


class ContractTests(unittest.TestCase):
    def test_peer_cannot_hide_two_engines_on_one_machine(self):
        with self.assertRaisesRegex(ValueError, "exactly one"):
            spread.Peer("host", engines=2)

    def test_safe_peer_name(self):
        with self.assertRaises(ValueError):
            spread.Peer("../owner")

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
        self.assertEqual(json.loads(mapped)["path"].replace("\\", "/"), "/native/lane/host_probe/done.json")

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

    def test_credentials_and_tickets_do_not_travel_as_evidence(self):
        for name in (".env", "key.pem", "host.ticket", "id_ed25519"):
            self.assertFalse(spread.public_file(Path(name)))

    def test_default_proof_mark(self):
        self.assertEqual(spread.topology(count=4), "single-box: not proof")


if __name__ == "__main__":
    unittest.main()
