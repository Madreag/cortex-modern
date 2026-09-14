"""Offline detecting checks for the preview cost and machine gates."""
import copy
from pathlib import Path
import unittest
from unittest.mock import patch

import run_preview_write_barrier as driver


class CostChecks(unittest.TestCase):
    def setUp(self):
        self.red = dict(transport_ok=True, interference=[], previews=16, preview_ms=5.0, exit_code=5)
        self.green = dict(transport_ok=True, interference=[], previews=16, preview_ms=5.1, exit_code=0,
                          native=[dict(windows=16, capture_ms=1, write_ms=1, restore_ms=1, max_ms=0.2)])

    def score(self):
        return driver.assess_cost(self.red, self.green, '240', 240, 240)['pass_check']

    def test_observed_budget_and_census(self):
        self.assertTrue(self.score())
        self.assertFalse(driver.assess_cost(self.red, self.green, '240', 4, 4)['pass_check'])

    def test_exact_limit_is_not_under_budget(self):
        self.green['native'][0]['max_ms'] = 0.5
        self.assertFalse(self.score())

    def test_states_are_summed(self):
        self.green['native'] *= 3
        self.assertFalse(self.score())

    def test_full_preview_regression_is_rejected(self):
        self.green['preview_ms'] = 5.5
        self.assertFalse(self.score())

    def test_missing_nonfinite_or_contended_evidence(self):
        good = copy.deepcopy(self.green)
        for field, value in [('native', []), ('interference', [{'pid': 42}]), ('previews', 0),
                             ('transport_ok', False), ('preview_ms', float('nan')), ('exit_code', 5)]:
            self.green = copy.deepcopy(good)
            self.green[field] = value
            self.assertFalse(self.score(), field)

    def test_resume_is_required_before_process_gate(self):
        with patch.object(driver, 'gate') as gate:
            with self.assertRaisesRegex(RuntimeError, 'FAMILY ENDED'):
                driver.phase_b(False)
            gate.assert_not_called()

    def test_family_lock_blocks_even_an_authorized_resume(self):
        with patch.object(Path, 'exists', return_value=True):
            with self.assertRaisesRegex(RuntimeError, 'machine remains reserved'):
                driver.locks_clear()

    def test_selftest_summary_requires_every_named_row_and_identity(self):
        from run_selftests import SELFTESTS
        identity = 'a' * 64
        summary = dict(passed=13, total=13, exe_sha256=identity,
                       results={name: {'pass': True, 'binary': identity} for name in SELFTESTS})
        self.assertTrue(driver.assess_selftests(summary, identity))
        summary['results'][SELFTESTS[0]]['binary'] = 'b' * 64
        self.assertFalse(driver.assess_selftests(summary, identity))


if __name__ == '__main__':
    unittest.main()
