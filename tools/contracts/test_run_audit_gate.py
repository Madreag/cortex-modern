"""Detect when the audit gate demands ARMED from a job that never configured a contract fixture."""

from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_audit

BINARY = "a" * 64
ARMED_LINE = 'print("[native-contract-check] ARMED uid=1")\n'


def item(operation, **fields):
    row = {
        "case": operation,
        "operation": operation,
        "completed": True,
        "process_clean": True,
        "desktop_unchanged": True,
        "binary": BINARY,
        "errors": [],
        "graph_capture_problems": [],
        "graph_observations": [
            {"observation": "before", "serialized": True, "problem_count": 0},
            {"observation": "after", "serialized": True, "problem_count": 0},
        ],
        "prepared": True,
        "applied": True,
        "armed_fixtures": [],
        "native_checks": [],
        "contract_checks": [],
        "native_mismatches": [],
        "contract_mismatches": [],
    }
    row.update(fields)
    return row


class FixtureGate(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.root = Path(self.scratch.name)

    def tearDown(self):
        self.scratch.cleanup()

    def write_script(self, relative, text):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def test_checkpoint_only_applying_job_passes_without_armed_line(self):
        checkpoint = self.write_script(
            "tools/fixtures/mod_checkpoint.lua",
            "function Create(self) end\n",
        )
        row = item("save", configured_scripts=[checkpoint])
        self.assertEqual(run_audit.gate(row, "any", BINARY), [])
        self.assertFalse(run_audit.fixtures_expected(row, "any", [checkpoint]))
        self.assertEqual(row.get("contract_fixtures_configured"), [])

    def test_configured_contract_fixture_still_requires_arming(self):
        native = self.write_script(
            "tools/contracts/mod_native_contracts.lua",
            ARMED_LINE,
        )
        row = item("save", configured_scripts=[native])
        self.assertTrue(run_audit.fixtures_expected(row, "any"))
        self.assertEqual(run_audit.gate(row, "any", BINARY), ["fixture_armed"])

    def test_armed_contract_fixture_still_requires_named_checks(self):
        native = self.write_script(
            "tools/contracts/mod_native_contracts.lua",
            ARMED_LINE,
        )
        row = item(
            "save",
            configured_scripts=[native],
            armed_fixtures=[{"name": "native", "uid": "1"}],
            native_checks=[],
        )
        self.assertEqual(run_audit.gate(row, "any", BINARY), ["native_checks"])

    def test_refused_load_expects_no_fixtures(self):
        native = self.write_script(
            "tools/contracts/mod_native_contracts.lua",
            ARMED_LINE,
        )
        row = item(
            "ordinary-load:missing",
            applied=False,
            configured_scripts=[native],
        )
        self.assertFalse(run_audit.fixtures_expected(row, "refused"))
        self.assertEqual(run_audit.gate(row, "refused", BINARY), [])


if __name__ == "__main__":
    unittest.main()
