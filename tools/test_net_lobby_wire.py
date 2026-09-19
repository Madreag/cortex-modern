"""Detect changes to the independent launch-config encoder without launching the engine."""
from dataclasses import replace
from pathlib import Path
import struct
import unittest

from net_activity_launch import encode_config, rules_for
from net_lobby_wire import Constant, read


class LaunchRelayLayoutTests(unittest.TestCase):
    def test_v6_appends_the_empty_relay_offer(self):
        wire = read(Path(__file__).resolve().parents[1])
        self.assertEqual(wire.config_version.value, 6)
        rules = rules_for("rules")
        historical = encode_config(rules, replace(wire, config_version=Constant(4, "recorded v4")))
        current = encode_config(rules, wire)
        header = wire.header_bytes.value
        self.assertEqual(current[-4:], b"\x02\x00{}")
        self.assertEqual(len(current), len(historical) + 4)
        self.assertEqual(current[header + 2:-4], historical[header + 2:])
        self.assertEqual(struct.unpack_from("<I", current, 12)[0], len(current) - header)
        self.assertEqual(struct.unpack_from("<H", current, header)[0], 6)


if __name__ == "__main__":
    unittest.main()
