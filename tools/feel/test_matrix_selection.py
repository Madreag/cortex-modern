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


if __name__ == '__main__':
    unittest.main()
