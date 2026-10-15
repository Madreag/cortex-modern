"""A process the runner kills at its timeout leaves where it waited beside its logs."""
import os
from pathlib import Path
import sys
import tempfile
import unittest

import posix_test_runner as runner


@unittest.skipIf(sys.platform == "win32", "the POSIX runner")
class TimeoutStacksTest(unittest.TestCase):
    def test_a_timed_out_group_records_each_threads_wait(self):
        saved = os.environ.get("CCCP_POSIX_HOP")
        os.environ["CCCP_POSIX_HOP"] = "off"
        try:
            with tempfile.TemporaryDirectory() as scratch:
                root = Path(scratch)
                record = runner.run(["/bin/sh", "-c", "sleep 300 & wait"], root, root / "hung", 3, startup_checks=False)
                self.assertTrue(record.get("timed_out"))
                self.assertEqual(record["exit_code"], 124)
                stacks = record.get("stacks") or {}
                self.assertTrue(stacks.get("written"), stacks)
                text = Path(stacks["path"]).read_text(errors="replace")
                if sys.platform == "darwin":
                    self.assertIn("Call graph", text)
                else:
                    self.assertIn(f"pid={stacks['pid']} threads=", text)
                    # The shell sleeps until its child ends: its thread names the kernel call it waits in.
                    self.assertRegex(text, r"comm=sh state=S wchan=(?!-\s|0\s)\S+")
        finally:
            if saved is None:
                os.environ.pop("CCCP_POSIX_HOP", None)
            else:
                os.environ["CCCP_POSIX_HOP"] = saved


if __name__ == "__main__":
    unittest.main()
