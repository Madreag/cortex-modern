"""A missing or differing running tick must fail the AI fight's hash check."""

import json
from pathlib import Path
import tempfile
import unittest

from compare_sim_traces import CORE
from e2e.fight_hashes import compare_fight_hashes


class FightHashesTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.spec = {"peers": ["host", "joiner"], "running_ticks": 2}
        self.rows = [{"round": 7, "tick": tick, "phase": "live", "paused": tick == 1,
                      "sim_gated": str(tick), "subsystems": {name: str(tick) for name in CORE | {"controller"}}}
                     for tick in range(1, 4)]
        for peer in self.spec["peers"]:
            (self.root / f"{peer}-stage/probe").mkdir(parents=True)
            self.write(peer, self.rows)

    def write(self, peer, rows):
        (self.root / f"{peer}-stage/probe/ticks.jsonl").write_text(
            "".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")

    def test_complete_shared_history(self):
        rows = json.loads(json.dumps(self.rows))
        for row in rows:
            row["subsystems"]["controller_route"] = "this player's route"
        self.write("joiner", rows)
        self.assertEqual(compare_fight_hashes(self.root, self.spec)["status"], "PASS")

    def test_first_shared_difference(self):
        rows = json.loads(json.dumps(self.rows))
        rows[1]["subsystems"]["actors"] = "another position"
        self.write("joiner", rows)
        result = compare_fight_hashes(self.root, self.spec)
        self.assertEqual(result["status"], "FAIL")
        self.assertEqual(result["first_difference"]["tick"], 2)
        self.assertEqual(result["first_difference"]["components"], ["actors"])

    def test_missing_peer_tail(self):
        self.write("joiner", self.rows[:-1])
        self.assertEqual(compare_fight_hashes(self.root, self.spec)["status"], "FAIL")

    def test_gap_in_both_histories(self):
        self.spec["running_ticks"] = 1
        for peer in self.spec["peers"]:
            self.write(peer, [self.rows[0], self.rows[2]])
        self.assertEqual(compare_fight_hashes(self.root, self.spec)["status"], "FAIL")

    def test_duplicate_tick(self):
        self.write("joiner", [*self.rows, self.rows[-1]])
        self.assertEqual(compare_fight_hashes(self.root, self.spec)["status"], "FAIL")

    def test_paused_ticks_do_not_complete_a_fight(self):
        self.spec["running_ticks"] = 3
        self.assertEqual(compare_fight_hashes(self.root, self.spec)["status"], "FAIL")

    def test_missing_component(self):
        rows = json.loads(json.dumps(self.rows))
        del rows[1]["subsystems"]["actors"]
        self.write("joiner", rows)
        self.assertEqual(compare_fight_hashes(self.root, self.spec)["status"], "FAIL")


if __name__ == "__main__":
    unittest.main()
