"""The runner refuses an engine launch while the feel matrix's stream holds the box, unless the launch carries the
stream's token. The executable named here does not exist, so no engine can start whichever way a check goes."""

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import win32_test_runner as runner


class FeelMarker(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.root = Path(self.scratch.name)
        self.marker = self.root / "FEEL-MATRIX-RUNNING"
        self.saved = runner.FEEL_MARKER
        runner.FEEL_MARKER = self.marker

    def tearDown(self):
        runner.FEEL_MARKER = self.saved
        self.scratch.cleanup()

    def hold(self, pid, token="feel-token"):
        self.marker.write_text(json.dumps({"stream_root": "C:/scratch/x/S3", "stamp": "now", "pid": pid, "token": token}),
                               encoding="utf-8")

    def start(self, name, env=None):
        run = runner.IsolatedRun([str(self.root / "Cortex Command.exe"), "-script-graph-selftest"], self.root,
                                 self.root / name, timeout=5, env=env)
        try:
            with self.assertRaises(runner.StartupCheckError) as refused:
                run.start()
        finally:
            run.close()
        return str(refused.exception), run.record["startup_checks"]

    def test_a_live_marker_refuses_an_engine_launch(self):
        self.hold(os.getpid())
        message, checks = self.start("held")
        self.assertIn("feel_matrix_not_holding_box", message)
        self.assertFalse(any(c["check"] == "exe_exists" for c in checks), "refused before any other check")

    def test_the_streams_own_launch_passes_the_marker(self):
        self.hold(os.getpid())
        message, checks = self.start("own", env={runner.FEEL_TOKEN_ENV: "feel-token"})
        self.assertNotIn("feel_matrix_not_holding_box", message)
        self.assertIn({"check": "feel_matrix_not_holding_box", "ok": True, "detail": "no live marker"}, checks)

    def test_a_dead_streams_marker_holds_nothing(self):
        dead = int(subprocess.run([sys.executable, "-c", "import os; print(os.getpid())"], capture_output=True,
                                  text=True).stdout)
        self.hold(dead)
        message, _ = self.start("stale")
        self.assertNotIn("feel_matrix_not_holding_box", message)

    def test_an_unreadable_marker_holds_and_no_marker_does_not(self):
        self.marker.write_text("half-written", encoding="utf-8")
        self.assertEqual(runner.feel_matrix_hold({}), "half-written")
        self.marker.unlink()
        self.assertIsNone(runner.feel_matrix_hold({}))


if __name__ == "__main__":
    unittest.main()
