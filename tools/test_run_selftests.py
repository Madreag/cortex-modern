"""The suite runner's sanitizer rule: a sanitizer build's declared wall-clock budget lines are reported, not judged,
and nothing else about the row is excused. Pure scoring; no engine."""

import tempfile
import unittest
from pathlib import Path

import run_selftests as runner

TAG = "[script-graph-selftest]"
TIMING = f"{TAG} FAIL threaded_synced_update_pass_timing registered=1024 states=32 before_us=30171 after_us=32740 added_us=2569 budget_us=150 delta_pct=8.51"
LOADED = f"{TAG} FAIL threaded_synced_update_pass_timing_under_load registered=5000 states=32 before_us=1219 after_us=5180 added_us=3961 budget_us=732"


def log(*lines):
    return "\n".join([f"{TAG} PASS same_tick_native_graph_capture serial_before=1", *lines]) + "\n"


class SanitizerBudgetRows(unittest.TestCase):
    def score(self, stdout, exit_code=1, timed_out=False):
        return runner.score_wall_clock_informational(stdout, {"exit_code": exit_code, "timed_out": timed_out}, "script-graph", "asan")

    def test_budget_lines_alone_are_reported_not_judged(self):
        scored = self.score(log(TIMING, LOADED, f"{TAG} PASS"))
        self.assertTrue(scored["pass"], scored)
        self.assertEqual(scored["informational_budget_lines"], [TIMING, LOADED])
        self.assertIn("asan build", scored["reason"])

    def test_any_other_named_fail_keeps_the_row_red(self):
        scored = self.score(log(TIMING, f"{TAG} FAIL threaded_synced_update_permitted_writes global_writes=0", f"{TAG} PASS"))
        self.assertFalse(scored["pass"], scored)

    def test_a_crash_or_a_missing_or_failed_closing_verdict_keeps_the_row_red(self):
        self.assertFalse(self.score(log(TIMING, LOADED), exit_code=3221225477)["pass"])
        self.assertFalse(self.score(log(TIMING, LOADED, f"{TAG} PASS"), exit_code=3221225477)["pass"])
        self.assertFalse(self.score(log(TIMING, LOADED), exit_code=1)["pass"])
        self.assertFalse(self.score(log(TIMING, LOADED, f"{TAG} FAIL"))["pass"])
        self.assertFalse(self.score(log(TIMING, LOADED, f"{TAG} PASS", f"{TAG} FAIL"))["pass"])

    def test_a_fatal_line_or_a_timeout_keeps_the_row_red(self):
        self.assertFalse(self.score(log(TIMING, "RTE Assert (headless) something", f"{TAG} PASS"))["pass"])
        self.assertFalse(self.score(log(TIMING, f"{TAG} PASS"), timed_out=True)["pass"])

    def test_a_row_without_budget_lines_is_scored_plainly(self):
        self.assertFalse(self.score(log(f"{TAG} PASS"))["pass"])

    def test_an_ordinary_build_judges_the_budget(self):
        self.assertFalse(runner.score_selftest(log(TIMING, f"{TAG} PASS"), 1, False, "script-graph-selftest")["pass"])

    def test_the_sanitizer_is_read_from_the_executable(self):
        with tempfile.TemporaryDirectory() as scratch:
            plain, asan, tsan = (Path(scratch) / name for name in ("plain.exe", "asan.exe", "tsan.bin"))
            plain.write_bytes(b"MZ" + b"\0" * (5 << 20) + b"ordinary engine")
            asan.write_bytes(b"MZ" + b"\0" * ((4 << 20) - 5) + b"clang_rt.asan_dynamic-x86_64.dll")
            tsan.write_bytes(b"\x7fELF" + b"__tsan_init")
            self.assertIsNone(runner.sanitizer_build(plain))
            self.assertEqual(runner.sanitizer_build(asan), "asan")
            self.assertEqual(runner.sanitizer_build(tsan), "tsan")


if __name__ == "__main__":
    unittest.main()
