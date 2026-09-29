import json
import tempfile
import unittest
from pathlib import Path

from normalize_suite_defects import normalize


class SuiteDefectsTest(unittest.TestCase):
    def test_only_the_proven_intentional_assert_summary_is_an_observation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "suite").mkdir()
            row = {"selftest": "headless-assert-continues", "pass": True, "exit_code": 1, "timed_out": False,
                   "fault_line": "RTE Assert (headless, continued like Ignore): Assertion in file '../Source/Menus/MenuAutomation.cpp', line 357,",
                   "verdicts_after_assert": ["[menu-script] assert_screen expected=MainScreen actual=MainScreen PASS"],
                   "shutdown_line": "[assert] the run continued past an assert; see the RTE Assert line above",
                   "exe_sha256": "test-binary"}
            (root / "suite/result.json").write_text(json.dumps({"results": {"headless-assert-continues": row}}))
            log = root / "suite-stdout.log"
            log.write_text(json.dumps(row) + "\n" + json.dumps({**row, "pass": False}) + "\n" + row["fault_line"] + "\n")
            document = {"defects": [{"kind": "assert-dialog", "path": f"{log}:{line}"} for line in (1, 2, 3)],
                        "observations": [], "defect_count": 3, "hard_count": 3}
            result = normalize(document, root)
            self.assertEqual(result["defect_count"], 2)
            self.assertEqual(result["hard_count"], 2)
            self.assertEqual(len(result["observations"]), 1)
            row["pass"] = False
            (root / "suite/result.json").write_text(json.dumps({"results": {"headless-assert-continues": row}}))
            self.assertEqual(normalize(document, root)["defect_count"], 3)


if __name__ == "__main__":
    unittest.main()
