import json
import os
from pathlib import Path
import tempfile
import unittest

from acceptance_evidence import live_hashes, peer_receipt, tagged_receipts
from compare_sim_traces import CORE


class NativeReceipts(unittest.TestCase):
    def setUp(self):
        retained = os.environ.get("CC_ACCEPTANCE_TEST_ROOT")
        if retained:
            self.root = Path(tempfile.mkdtemp(prefix="receipt-test-", dir=retained))
        else:
            temporary = tempfile.TemporaryDirectory()
            self.addCleanup(temporary.cleanup)
            self.root = Path(temporary.name)
        self.base = [dict(session=1, match=2, history_branch=0, source_round=1, instance="peer", execution=0,
                          incarnation=0, tick=t, phase="live", wall_ms=t*1000/60, sim_gated="a"*64,
                          subsystems=dict.fromkeys(CORE | {"controller"}, "b"*64)) for t in range(1, 62)]

    def write(self, name, rows):
        path = self.root/name
        path.write_text("".join(json.dumps({**r, "instance": path.stem})+"\n" for r in rows), encoding="utf-8")
        return path

    def test_missing_activation_tick_is_not_trimmed(self):
        a = self.write("a.jsonl", self.base)
        b = self.write("b.jsonl", self.base[1:])
        result = live_hashes(dict(a=a, b=b), 1, 61)
        self.assertEqual(result["missing"], 1)
        self.assertEqual(result["compared"], 60)

    def test_wrong_history_or_duplicate_is_not_hidden(self):
        a = self.write("a.jsonl", self.base)
        b = self.write("b.jsonl", [{**r, "match": 3} for r in self.base])
        self.assertEqual(live_hashes(dict(a=a, b=b), 1, 61)["unequal"], 61)
        c = self.write("c.jsonl", self.base+self.base[:1])
        self.assertEqual(live_hashes(dict(a=a, c=c), 1, 61)["duplicates"], 1)

    def test_waits_and_rate_read_native_rows(self):
        path = self.write("a.jsonl", self.base)
        result = peer_receipt("pc", path, "[net-frame-wait] frame=4 wait_ms=51\n", dict(exit_code=0), 1, 61)
        self.assertEqual(result["timing"][0]["max_wait_ms"], 51)
        self.assertAlmostEqual(result["timing"][0]["elapsed_ms"], 1000)

    def test_wait_on_first_required_tick_is_not_trimmed(self):
        path = self.write("a.jsonl", self.base)
        result = peer_receipt("pc", path, "[net-frame-wait] frame=1 wait_ms=51\n", dict(exit_code=0), 1, 61)
        self.assertEqual(result["timing"][0]["max_wait_ms"], 51)
        self.assertEqual(result["timing"][0]["wait_ms"], 51)

    def test_a_short_tail_is_judged_with_the_window_before_it(self):
        # One tick 2.4 ms late and the next as early: the pacer's own correction, with the corrected tick past the bracket's end.
        rows = [dict(self.base[0], tick=t, wall_ms=t*1000/60 + (2.4 if t == 72 else 0)) for t in range(1, 74)]
        path = self.write("a.jsonl", rows)
        timing = peer_receipt("pc", path, "", dict(exit_code=0), 1, 72)["timing"]
        self.assertEqual([(w["first"], w["last"]) for w in timing], [(1, 72)])
        self.assertTrue(all(w["last"] - w["first"] >= 60 for w in timing))
        self.assertGreaterEqual((timing[-1]["last"] - timing[-1]["first"]) * 1000 / timing[-1]["elapsed_ms"], 59.5)

    def test_malformed_receipt_fails(self):
        with self.assertRaises(ValueError): tagged_receipts('[transfer] {bad}', 'transfer')


if __name__ == "__main__":
    unittest.main(verbosity=2)
