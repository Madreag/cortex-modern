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
                          native=[dict(windows=16, capture_ms=1, write_ms=1, restore_ms=1, max_ms=0.2, p99_ms=0.1,
                                       tables=64, saves=8)])

    def score(self):
        return driver.assess_cost(self.red, self.green, '240', 240, 240)['pass_check']

    def test_observed_budget_and_census(self):
        self.assertTrue(self.score())
        self.assertFalse(driver.assess_cost(self.red, self.green, '240', 4, 4)['pass_check'])

    def test_exact_limit_is_not_under_budget(self):
        self.green['native'][0]['max_ms'] = 0.5
        self.assertFalse(self.score())

    def test_p99_over_limit_is_rejected(self):
        self.green['native'][0]['p99_ms'] = 0.5
        self.assertFalse(self.score())

    def test_cost_row_carries_journal_and_restore_fields(self):
        cost = driver.assess_cost(self.red, self.green, '240', 240, 240)
        self.assertEqual(cost['journal_entries'], 8)
        self.assertEqual(cost['tables_touched'], 64)
        self.assertEqual(cost['restore_us_per_window'], 1000.0/16)
        self.assertEqual(cost['native_p99_ms_sum'], 0.1)

    def test_fail_line_with_trailing_error_is_scored(self):
        text = '[script-graph-selftest] FAIL preview_depth_writes_undone depth 1 write leaked\n'
        failures = driver.FAIL.findall(text)
        self.assertEqual(failures, ['preview_depth_writes_undone'])
        self.assertTrue(driver.graph_failed(failures, 'preview_depth_writes_undone'))
        self.assertTrue(driver.graph_failed(['preview_depth_writes_undone depth 1 write leaked'],
                                            'preview_depth_writes_undone'))
        self.assertFalse(driver.graph_failed(failures, 'preview_window_modcompat'))

    def test_zero_journal_is_rejected(self):
        self.green['native'][0]['saves'] = 0
        self.assertFalse(self.score())
        self.green['native'][0]['saves'] = 8
        self.green['native'][0]['tables'] = 0
        self.assertFalse(self.score())

    def test_missing_or_nonfinite_restore_ms_is_rejected(self):
        good = copy.deepcopy(self.green)
        del self.green['native'][0]['restore_ms']
        cost = driver.assess_cost(self.red, self.green, '240', 240, 240)
        self.assertGreater(cost['green_windows'], 0)
        self.assertIsNone(cost['restore_us_per_window'])
        self.assertFalse(cost['pass_check'])
        self.green = copy.deepcopy(good)
        self.green['native'][0]['restore_ms'] = float('nan')
        self.assertFalse(self.score())

    def test_omitted_or_nan_p99_ms_is_rejected(self):
        good = copy.deepcopy(self.green)
        del self.green['native'][0]['p99_ms']
        self.assertFalse(self.score())
        self.green = copy.deepcopy(good)
        self.green['native'][0]['p99_ms'] = float('nan')
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
        summary = dict(passed=len(SELFTESTS), total=len(SELFTESTS), exe_sha256=identity,
                       results={name: {'pass': True, 'binary': identity} for name in SELFTESTS})
        self.assertTrue(driver.assess_selftests(summary, identity))
        summary['results'][SELFTESTS[0]]['binary'] = 'b' * 64
        self.assertFalse(driver.assess_selftests(summary, identity))

    def test_actor_census_accepts_binary_lua_graphs(self):
        data = b'153 actor uid=1 A\n153 att uid=2 B\n153 actor uid=3 C\nlua_graph\xb8\x00\n'
        self.assertEqual(driver.count_actors(data), 2)


if __name__ == '__main__':
    unittest.main()
