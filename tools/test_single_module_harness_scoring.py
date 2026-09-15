"""score_detect fails a nonzero exit, a timeout, or a FATAL after Index.ini exists."""
from __future__ import annotations

import unittest

from test_single_module_harness import score_detect


def _clean(**extra):
    case = {
        "error_line_present": False,
        "userdata_index_exists": True,
        "exit_code": 0,
        "timed_out": False,
        "log_text": "",
    }
    case.update(extra)
    return case


class TestScoreDetectProcess(unittest.TestCase):
    def test_exit_timeout_fatal_fail(self):
        self.assertFalse(score_detect(_clean(exit_code=1))["pass"])
        self.assertFalse(score_detect(_clean(timed_out=True))["pass"])
        self.assertFalse(score_detect(_clean(log_text="RTE Abort after Index.ini"))["pass"])

    def test_existing_checks_still_required(self):
        self.assertFalse(score_detect(_clean(error_line_present=True))["pass"])
        self.assertFalse(score_detect(_clean(userdata_index_exists=False))["pass"])
        self.assertTrue(score_detect(_clean())["pass"])


if __name__ == "__main__":
    unittest.main()
