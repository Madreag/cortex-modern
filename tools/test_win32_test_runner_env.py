"""Launch env carries CC_TEST_CRASH_DUMP unless the caller set it."""
from __future__ import annotations

import os
import tempfile
import unittest
from pathlib import Path

from posix_test_runner import IsolatedRun as PosixIsolatedRun
from win32_test_runner import IsolatedRun


def _construct(cls, extra=None):
    saved = os.environ.pop("CC_TEST_CRASH_DUMP", None)
    try:
        with tempfile.TemporaryDirectory() as tmp:
            cwd = Path(tmp) / "cwd"
            cwd.mkdir()
            run = cls(["dummy.exe", "-headless"], cwd, Path(tmp) / "out", env=extra)
            try:
                return dict(run.env), dict(run.record.get("env_set") or {})
            finally:
                run.close()
    finally:
        if saved is not None:
            os.environ["CC_TEST_CRASH_DUMP"] = saved


class TestCrashDumpEnv(unittest.TestCase):
    def test_default_exports_crash_dump(self):
        for cls in (IsolatedRun, PosixIsolatedRun):
            env, env_set = _construct(cls)
            self.assertEqual(env.get("CC_TEST_CRASH_DUMP"), "1", cls.__module__)
            self.assertEqual(env_set.get("CC_TEST_CRASH_DUMP"), "1", cls.__module__)

    def test_caller_value_is_kept(self):
        for cls in (IsolatedRun, PosixIsolatedRun):
            env, env_set = _construct(cls, {"CC_TEST_CRASH_DUMP": r"D:\mx\forced\crash.dmp"})
            self.assertEqual(env.get("CC_TEST_CRASH_DUMP"), r"D:\mx\forced\crash.dmp", cls.__module__)
            self.assertEqual(env_set.get("CC_TEST_CRASH_DUMP"), r"D:\mx\forced\crash.dmp", cls.__module__)


if __name__ == "__main__":
    unittest.main()
