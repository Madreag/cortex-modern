"""Detect a missing-trace offline compare that used to skip the failure.

Base tree left green_equal_reference unset on a missing trace.json and could
still exit 0 when the pie lists were empty. Empty input must fail with
'missing traces for offline compare'. A present red that equals the reference
must not be required to diverge.
"""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import compare_offline
from compare_offline import compare, main

CASES = (
    "SimBaseline", "TerrainStress", "ActorStress", "LuaBaseline", "LuaRandomStress", "LuaPairsStress",
    "LuaOsStubTest", "ModSmokeLoading", "TerrainCarveStress", "ThreadStress", "PerfBench", "Net_Lifecycle_Test",
    "p4_sp", "sp_identity", "ak47", "PreviewCompat", "PreviewModuleCompat", "F15Retirement_LateStateGain",
)
TRACE = json.dumps({"runs": [{"numeric": {}, "final_total_hash": "aa"}]})


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

    def test_retained_ak47_without_stage_traces_keeps_the_named_reason(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            retained = Path(tmp) / "ak47.json"
            retained.write_text(TRACE, encoding="utf-8")
            with patch.object(compare_offline, "RETAINED_PIE", Path(tmp) / "no-pie"), \
                    patch.object(compare_offline, "RETAINED_AK47", retained):
                result = compare(root)
            for row in result["rows"]:
                self.assertEqual(row["reason"], "missing traces for offline compare")

    def test_identical_present_red_does_not_force_exit_1(self):
        """A present red that equals the reference is not required to diverge."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            pie = Path(tmp) / "pie.txt"
            pie.write_bytes(b"pie")
            retained = Path(tmp) / "ak47.json"
            retained.write_text(TRACE, encoding="utf-8")
            for stage in ("reference", "red", "green"):
                (root / stage / "sp_identity").mkdir(parents=True)
                (root / stage / "sp_identity" / "trace.json.simdump.txt").write_bytes(b"pie")
                for name in CASES:
                    path = root / stage / name / "trace.json"
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(TRACE, encoding="utf-8")
            with patch.object(compare_offline, "RETAINED_PIE", pie), \
                    patch.object(compare_offline, "RETAINED_AK47", retained):
                result = compare(root)
                self.assertTrue(all(row["present"] and row["red_equal_reference"]
                                    and row["green_equal_reference"] for row in result["rows"]))
                self.assertEqual(main([str(root)]), 0)


if __name__ == "__main__":
    unittest.main()
