import os
from pathlib import Path
import tempfile
import unittest

from world_soak import configuration, census_receipts, journal_receipt


class WorldSoak(unittest.TestCase):
    def test_minute_fifty_join_and_full_cadences(self):
        plan = configuration()
        self.assertEqual(plan["late_join_elapsed_s"], 3000)
        self.assertGreaterEqual(plan["elapsed_s"], 3660)
        self.assertEqual(plan["journal_minutes"], [10,30,50,60])
        self.assertEqual(plan["memory"]["retained_bytes"], 128*1024**2)
        with self.assertRaises(ValueError): configuration(3599)

    def test_raw_memory_is_retained_beside_current_reducer(self):
        text = "[mem-census] tick=3600 uptime_ms=60000 private_mb=200 cow: entries=1 entry_mb=100 pixels=1 retired=0 retired_mb=0 last_image_mb=0\n"
        result = census_receipts(text)
        self.assertEqual(result["raw_series"][0]["process_bytes"], 200*1024**2)
        self.assertEqual(result["current_reduce_memory_census"]["series"][0]["net_mb"], 100)

    def test_missing_journal_is_not_zero_growth(self):
        retained = os.environ.get("CC_ACCEPTANCE_TEST_ROOT")
        if retained:
            root = Path(tempfile.mkdtemp(prefix="soak-test-", dir=retained))
        else:
            temporary = tempfile.TemporaryDirectory()
            self.addCleanup(temporary.cleanup)
            root = Path(temporary.name)
        result = journal_receipt(root, 10, 600)
        self.assertFalse(result["present"])
        self.assertIsNone(result["bytes"])
        with self.assertRaises(ValueError): journal_receipt(root, 10, 605)


if __name__ == "__main__":
    unittest.main(verbosity=2)
