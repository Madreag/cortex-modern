"""Detect a missing-trace offline compare that used to skip the failure.

Base tree left green_equal_reference unset on a missing trace.json and could
still exit 0 when the pie lists were empty. Empty input must fail with
'missing traces for offline compare'. A present red that equals the reference
must not be required to diverge.
"""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import compare_offline
from compare_offline import compare, main


class CompareOfflineMissingTraces(unittest.TestCase):
    def test_missing_traces_fail_with_named_reason(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            absent = Path(tmp) / "no-retained"
            with patch.object(compare_offline, "RETAINED_PIE", absent / "pie"), \
                    patch.object(compare_offline, "RETAINED_AK47", absent / "ak47"):
                result = compare(root)
                self.assertTrue(result["rows"])
                for row in result["rows"]:
                    self.assertFalse(row["present"])
                    self.assertEqual(row["reason"], "missing traces for offline compare")
                code = main([str(root)])
            self.assertEqual(code, 1)


if __name__ == "__main__":
    unittest.main()
