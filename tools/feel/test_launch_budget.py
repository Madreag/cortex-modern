"""The feel driver's box reservation: its own, or one its caller already holds.

Acceptance run 1's feel shards ran under the chain's reservation and died with FileExistsError creating their own.
"""
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from feel import launch_budget


class ExclusiveMatrixTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.marker = self.root / 'FEEL-MATRIX-RUNNING'
        self.patches = [patch.object(launch_budget, 'MARKER', self.marker), patch.object(launch_budget, 'CROSS_GUARD', self.root / 'BOX-FREE-FOR-CROSS')]
        for active in self.patches:
            active.start()

    def tearDown(self):
        for active in self.patches:
            active.stop()

    def test_an_inherited_reservation_is_used_and_left_to_its_owner(self):
        self.marker.write_text(json.dumps(dict(pid=1, token='chain-token')), encoding='utf-8')
        with patch.dict(os.environ, {'CCCP_FEEL_MATRIX_RUN': 'chain-token'}):
            with launch_budget.exclusive_matrix():
                self.assertEqual(os.environ['CCCP_FEEL_MATRIX_RUN'], 'chain-token')
            self.assertEqual(json.loads(self.marker.read_text(encoding='utf-8'))['token'], 'chain-token')

    def test_another_owners_reservation_is_refused(self):
        self.marker.write_text(json.dumps(dict(pid=1, token='other-lane')), encoding='utf-8')
        with patch.dict(os.environ, {'CCCP_FEEL_MATRIX_RUN': 'chain-token'}):
            with self.assertRaises(FileExistsError):
                with launch_budget.exclusive_matrix():
                    pass

    def test_a_free_box_is_reserved_and_released(self):
        with patch.dict(os.environ, {}, clear=False):
            os.environ.pop('CCCP_FEEL_MATRIX_RUN', None)
            with launch_budget.exclusive_matrix():
                self.assertTrue(self.marker.is_file())
                self.assertEqual(json.loads(self.marker.read_text(encoding='utf-8'))['token'], os.environ['CCCP_FEEL_MATRIX_RUN'])
            self.assertFalse(self.marker.exists())
            self.assertNotIn('CCCP_FEEL_MATRIX_RUN', os.environ)


@unittest.skipUnless(sys.platform == 'win32', 'the launch guard wraps the Windows runner')
class CrossFlagTest(unittest.TestCase):
    """The cross match's reservation keeps strangers off the box, never the cross driver's own launch (r2-pc-match/payload.log:
    'PAYLOAD FAIL: engine launch refused: the box is reserved for the cross match')."""

    def setUp(self):
        import win32_test_runner
        self.root = Path(tempfile.mkdtemp())
        self.marker = self.root / 'FEEL-MATRIX-RUNNING'
        self.guard = self.root / 'BOX-FREE-FOR-CROSS'
        self.guard.write_text('cross', encoding='utf-8')
        self.patches = [patch.object(launch_budget, 'MARKER', self.marker), patch.object(launch_budget, 'CROSS_GUARD', self.guard),
                        patch.object(launch_budget, 'free_memory_bytes', lambda: 64 * 1024 ** 3),
                        patch.object(win32_test_runner.IsolatedRun, 'start', lambda run: 'launched')]
        for active in self.patches:
            active.start()
        launch_budget.install_memory_guard()
        self.start = win32_test_runner.IsolatedRun.start

    def tearDown(self):
        for active in reversed(self.patches):
            active.stop()

    def run_record(self):
        class Run:
            record = {}
            def _save(self):
                pass
        return Run()

    def test_a_marker_owner_launches_under_the_cross_flag(self):
        self.marker.write_text(json.dumps(dict(pid=1, token='cross-driver')), encoding='utf-8')
        with patch.dict(os.environ, {'CCCP_FEEL_MATRIX_RUN': 'cross-driver'}):
            self.assertEqual(self.start(self.run_record()), 'launched')

    def test_a_stranger_is_refused_under_the_cross_flag(self):
        with patch.dict(os.environ, {}, clear=False):
            os.environ.pop('CCCP_FEEL_MATRIX_RUN', None)
            with self.assertRaisesRegex(RuntimeError, 'reserved for the cross match'):
                self.start(self.run_record())
        self.marker.write_text(json.dumps(dict(pid=1, token='cross-driver')), encoding='utf-8')
        with patch.dict(os.environ, {'CCCP_FEEL_MATRIX_RUN': 'some-lane'}):
            with self.assertRaisesRegex(RuntimeError, 'refused'):
                self.start(self.run_record())


if __name__ == '__main__':
    unittest.main()
