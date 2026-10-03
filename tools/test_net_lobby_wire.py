"""Detect changes to the independent launch-config encoder without launching the engine."""
from dataclasses import replace
from pathlib import Path
import struct
import unittest

from net_activity_launch import encode_config, rules_for
from net_lobby_wire import Constant, read, session_protocol


class LaunchRelayLayoutTests(unittest.TestCase):
    def test_v8_carries_timing_relay_and_seat_roster(self):
        wire = read(Path(__file__).resolve().parents[1])
        self.assertEqual(wire.config_version.value, 8)
        rules = rules_for("rules")
        before_relay = replace(wire, config_version=Constant(wire.relay_layout_version.value - 1, "before the relay layout"))
        historical = encode_config(rules, before_relay)
        current = encode_config(rules, wire)
        header = wire.header_bytes.value
        self.assertEqual(current[-44:], struct.pack('<HBB', 3, 1, 0) + b"\x02\x00{}" + bytes(36))
        self.assertEqual(len(current), len(historical) + 44)
        self.assertEqual(current[header + 2:-44], historical[header + 2:])
        self.assertEqual(struct.unpack_from("<I", current, 12)[0], len(current) - header)
        self.assertEqual(struct.unpack_from("<H", current, header)[0], 8)

    def test_the_session_protocol_is_read_from_the_tree(self):
        repo = Path(__file__).resolve().parents[1]
        protocol = session_protocol(repo)
        header = (repo / "Source/Network/NetProtocol.h").read_text(encoding="utf-8").splitlines()
        line = header[int(protocol.site.rsplit(":", 1)[1]) - 1]
        self.assertIn("c_Version", line)
        self.assertIn(str(protocol.value), line)


if __name__ == "__main__":
    unittest.main()
