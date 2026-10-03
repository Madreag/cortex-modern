"""Detect changes to the independent launch-config encoder without launching the engine."""
from dataclasses import replace
from pathlib import Path
import struct
import unittest

from net_activity_launch import encode_config, rules_for
from net_lobby_wire import Constant, read


class LaunchRelayLayoutTests(unittest.TestCase):
    def test_v8_carries_timing_relay_and_seat_roster(self):
        wire = read(Path(__file__).resolve().parents[1])
        self.assertEqual(wire.config_version.value, 8)
        rules = rules_for("rules")
        historical = encode_config(rules, replace(wire, config_version=Constant(4, "recorded v4")))
        current = encode_config(rules, wire)
        header = wire.header_bytes.value
        self.assertEqual(current[-44:], struct.pack('<HBB', 3, 1, 0) + b"\x02\x00{}" + bytes(36))
        self.assertEqual(len(current), len(historical) + 44)
        self.assertEqual(current[header + 2:-44], historical[header + 2:])
        self.assertEqual(struct.unpack_from("<I", current, 12)[0], len(current) - header)
        self.assertEqual(struct.unpack_from("<H", current, header)[0], 8)


if __name__ == "__main__":
    unittest.main()
