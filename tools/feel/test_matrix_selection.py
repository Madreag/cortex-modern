import io
import json
from contextlib import redirect_stdout
from pathlib import Path
import tempfile
import unittest


class MatrixSelection(unittest.TestCase):
    def plan(self, *arguments):
        import feel_measure
        with tempfile.TemporaryDirectory() as folder, redirect_stdout(io.StringIO()) as out:
            self.assertEqual(feel_measure.main(['--out', str(Path(folder) / 'run'), '--dry-run', *arguments]), 0)
        return json.loads(out.getvalue())

    def test_lag_arms_with_cases_run_both_with_their_baselines(self):
        plan = self.plan('--lag-arms', '100ms-60hz', '--cases', '100ms-silent600')
        self.assertEqual(plan['path'], 'full matrix', 'the lag path keeps its gates; --cases alone does not')
        self.assertEqual([arm['arm'] for arm in plan['arms']], ['baseline-60hz', 'baseline-60hz-off', '100ms-60hz-on', '100ms-60hz-off',
                                                                'baseline-three-60hz', '100ms-silent600'])

    def test_cases_alone_keep_their_own_path(self):
        plan = self.plan('--cases', '100ms-silent600')
        self.assertEqual(plan['path'], '--cases')
        self.assertEqual([arm['arm'] for arm in plan['arms']], ['100ms-silent600'])

    def test_lag_arms_alone_are_unchanged(self):
        plan = self.plan('--lag-arms', '100ms-60hz')
        self.assertEqual([arm['arm'] for arm in plan['arms']], ['baseline-60hz', 'baseline-60hz-off', '100ms-60hz-on', '100ms-60hz-off'])

    def test_spread_keeps_the_original_arm_and_lag_plan(self):
        self.assertEqual(self.plan('--spread', '--lag-arms', '100ms-60hz'), self.plan('--lag-arms', '100ms-60hz'))

    def test_spread_keeps_the_original_loss_and_silent_levers(self):
        self.assertEqual(self.plan('--spread', '--cases', '100ms-loss5-silent600'), self.plan('--cases', '100ms-loss5-silent600'))

    def test_result_records_keep_peer_shape_and_mark_single_box(self):
        import feel_measure
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'manifest.json').write_text(json.dumps({'per_peer_lag_ms': {'host': 100, 'client': 100}}))
            feel_measure.write_json(root / 'run-result.json', {'host': {'exit_code': 0}, 'client': {'exit_code': 0}})
            result = json.loads((root / 'run-result.json').read_text())
        self.assertEqual(set(result), {'host', 'client'})
        self.assertTrue(all(row['topology'] == 'single-box: not proof' for row in result.values()))

    def test_analysis_keeps_recorded_spread_assignments(self):
        import feel_measure
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'manifest.json').write_text(json.dumps({'topology': 'spread', 'peer_boxes': {'host': 'ONE', 'client': 'TWO'}}))
            feel_measure.write_json(root / 'feel-report.json', {'name': '100ms-loss5', 'item9a_pass': False})
            result = json.loads((root / 'feel-report.json').read_text())
        self.assertEqual(result['topology'], 'spread')
        self.assertEqual(result['peer_boxes'], {'host': 'ONE', 'client': 'TWO'})
        self.assertIs(result['item9a_pass'], False)


if __name__ == '__main__':
    unittest.main()
