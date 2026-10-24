"""The suite runner's sanitizer rule: a sanitizer build's declared wall-clock budget lines are reported, not judged,
and nothing else about the row is excused. Pure scoring; no engine."""

import contextlib
import json
import io
import tempfile
import types
import unittest
from pathlib import Path

import run_selftests as runner

TAG = "[script-graph-selftest]"


class ProofMarking(unittest.TestCase):
    def test_detector_collection_never_claims_play_proof(self):
        with tempfile.TemporaryDirectory() as temporary:
            count = len(runner.SELFTESTS)
            runner.write_result(Path(temporary), {'passed': count, 'total': count})
            result = json.loads((Path(temporary) / 'result.json').read_text())
        self.assertEqual(result['topology'], 'single-box: not proof')
        self.assertEqual((result['passed'], result['total']), (count, count))


class FreshFixedFailureReport(unittest.TestCase):
    STDOUT = ("[net-session-plane-selftest] FAIL saved_seed_7: seed=7 property=histories kind=return\n"
              "[net-session-plane-selftest] saved_schedule_group name=saved_seed_7 group=histories:return:2:direct\n"
              "[net-session-plane-selftest] saved_schedule_catalogue expected=108 ran=108\n"
              "[net-session-plane-selftest] FAIL\n")

    def options(self, root):
        from types import SimpleNamespace
        return SimpleNamespace(_suite_exe_sha256="current-binary", _suite_root=root)

    def record(self, **changes):
        return {"exit_code": 1, "timed_out": False, "exe_sha256": "current-binary", **changes}

    def test_driver_always_includes_fresh_fixed_prerequisite(self):
        self.assertEqual(runner.plan_rows(None, ["net-session-plane-hold-schedules"]),
                         (["net-session-plane", "net-session-plane-hold-schedules"], []))

    def test_red_fixed_collection_supplies_index_without_changing_its_result(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            options = self.options(root)
            proof = runner.save_fixed_failure_report(options, root, self.STDOUT, self.record())
            self.assertEqual(runner.current_fixed_failure_report(options), proof)
            self.assertEqual(proof["saved_cases_ran"], 108)
            self.assertIn("histories:return:2:direct\tsaved_seed_7\n", Path(proof["path"]).read_text())
            self.assertFalse(runner.score_selftest(self.STDOUT, 1, False, "net-session-plane-selftest")["pass"])

    def test_partial_timeout_wrong_binary_or_missing_closing_cannot_supply_index(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for stdout, record in [(self.STDOUT.replace("ran=108", "ran=107"), self.record()),
                                   (self.STDOUT, self.record(timed_out=True)),
                                   (self.STDOUT, self.record(exe_sha256="older-binary")),
                                   (self.STDOUT.rsplit("[net-session-plane-selftest] FAIL", 1)[0], self.record())]:
                with self.subTest(stdout=stdout, record=record), self.assertRaises(ValueError):
                    runner.save_fixed_failure_report(self.options(root), root, stdout, record)

    def test_missing_proof_changed_report_or_different_invocation_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            options = self.options(root)
            with self.assertRaises(ValueError):
                runner.current_fixed_failure_report(options)
            proof = runner.save_fixed_failure_report(options, root, self.STDOUT, self.record())
            options._suite_root = root / "other-invocation"
            with self.assertRaises(ValueError):
                runner.current_fixed_failure_report(options)
            options._suite_root = root
            options._suite_exe_sha256 = "new-binary"
            with self.assertRaises(ValueError):
                runner.current_fixed_failure_report(options)
            options._suite_exe_sha256 = "current-binary"
            Path(proof["path"]).write_text("hold-fixed-groups-v1\n")
            with self.assertRaises(ValueError):
                runner.current_fixed_failure_report(options)
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
        self.assertIn("sanitizer: budget not gated", scored["reason"])
        self.assertIn("added_us=2569 budget_us=150", scored["reason"])

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


class SanitizerRowTimeout(unittest.TestCase):
    def test_tsan_script_graph_gets_its_own_cap_and_an_ordinary_build_keeps_the_suite_cap(self):
        self.assertGreater(runner.SANITIZER_ROW_TIMEOUT["script-graph"]["tsan"], 1800)
        self.assertNotIn(None, runner.SANITIZER_ROW_TIMEOUT["script-graph"])


class SlowestBoxRowTimeout(unittest.TestCase):
    """The cap a row is launched with: twice its time on the slowest box where measured, else the suite's cap."""

    def launched_budget(self, name, timeout=300):
        budgets = []

        class Run:
            def start(self):
                return self

            def finish(self):
                return {"exit_code": 0, "timed_out": False, "exe_sha256": "0"}

            def close(self):
                pass

        def make_run(repo, args, case, budget, **kwargs):
            budgets.append(budget)
            return Run()

        options = types.SimpleNamespace(repo=Path("."), timeout=timeout, _planned_rows=())
        with tempfile.TemporaryDirectory() as directory:
            runner.run_row_in_mode(options, make_run, name, Path(directory), None)
        return budgets[0]

    def test_the_session_plane_row_gets_twice_its_slowest_box_time(self):
        self.assertGreaterEqual(self.launched_budget("net-session-plane"), 2 * 272)

    def test_an_unmeasured_row_and_a_longer_suite_cap_are_unchanged(self):
        self.assertEqual(self.launched_budget("net-lockstep"), 300)
        self.assertEqual(self.launched_budget("net-session-plane", timeout=7200), 7200)


class ScoreStdoutSanitizer(unittest.TestCase):
    def run_cli(self, stdout, *extra):
        import subprocess
        import sys
        with tempfile.TemporaryDirectory() as scratch:
            log_path, result = Path(scratch) / "stdout.log", Path(scratch) / "result.json"
            log_path.write_text(stdout, encoding="utf-8")
            done = subprocess.run([sys.executable, "-B", str(Path(runner.__file__)), "--score-stdout", str(log_path),
                                   "--exit-code", "1", "--name", "script-graph-selftest", "--result-json", str(result),
                                   *extra], capture_output=True, text=True, timeout=60)
            import json
            return done.returncode, json.loads(result.read_text(encoding="utf-8"))

    def test_a_sanitizer_log_is_measured_and_an_ordinary_log_is_judged(self):
        code, scored = self.run_cli(log(TIMING, LOADED, f"{TAG} PASS"), "--sanitizer", "tsan")
        self.assertEqual((code, scored["pass"]), (0, True), scored)
        self.assertIn("sanitizer: budget not gated (tsan build)", scored["reason"])
        code, scored = self.run_cli(log(TIMING, LOADED, f"{TAG} PASS"))
        self.assertEqual((code, scored["pass"]), (1, False), scored)

    def test_the_launch_record_decides_the_exit_and_the_timeout(self):
        import json
        with tempfile.TemporaryDirectory() as scratch:
            launch = Path(scratch) / "launch.json"
            launch.write_text(json.dumps({"exit_code": 1, "timed_out": True}), encoding="utf-8")
            code, scored = self.run_cli(log(TIMING, LOADED, f"{TAG} PASS"), "--sanitizer", "asan", "--launch-json", str(launch))
            self.assertEqual((code, scored["pass"]), (1, False), scored)
            launch.write_text(json.dumps({"exit_code": 1, "timed_out": False}), encoding="utf-8")
            code, scored = self.run_cli(log(TIMING, LOADED, f"{TAG} PASS"), "--sanitizer", "asan", "--launch-json", str(launch))
            self.assertEqual((code, scored["pass"]), (0, True), scored)


class RenderCapSanitizer(unittest.TestCase):
    CASE = {"exit_code": 0, "timed_out": False, "cap": "60hz", "frames": 11, "interval_s": 3.06, "presentations_per_second": 3.27}

    def score(self, sanitizer=None, **change):
        from test_headless_render_cap import score_detect  # noqa: PLC0415
        # The judge prints its verdict line; for these synthetic cases it stays out of the suite's log.
        with contextlib.redirect_stdout(io.StringIO()):
            return score_detect({**self.CASE, **change}, sanitizer)

    def test_an_ordinary_build_gates_the_frame_floor(self):
        self.assertFalse(self.score()["pass"])

    def test_a_sanitizer_build_measures_the_floor_and_still_gates_the_cap(self):
        scored = self.score("tsan")
        self.assertTrue(scored["pass"], scored)
        self.assertIn("sanitizer: budget not gated", scored["reason"])
        self.assertFalse(self.score("tsan", presentations_per_second=120.0)["pass"])
        self.assertFalse(self.score("tsan", cap=None)["pass"])
        self.assertFalse(self.score("tsan", frames=0)["pass"])
        self.assertFalse(self.score("tsan", exit_code=1)["pass"])


class SinglePlayerCanonicalDump(unittest.TestCase):
    WINDOWS = b"27 actor uid=1 Dummy pos=0x1.2c00000000000p+10,-0x0.0000000000000p+0 feet=9e98e75352970809 unknown=keep\r\n"
    POSIX = b"27 actor uid=1 Dummy pos=0x1.2cp+10,-0x0p+0 feet=9e98e75352970809 unknown=keep\n"

    def test_equivalent_float_spellings_and_record_terminators_match(self):
        self.assertEqual(runner.canonical_single_player_dump(self.WINDOWS), runner.canonical_single_player_dump(self.POSIX))
        self.assertEqual(runner.canonical_single_player_dump(self.WINDOWS),
                         b"27 actor uid=1 Dummy pos=0x1.2c00000000000p+10,-0x0.0p+0 feet=9e98e75352970809 unknown=keep\n")

    def test_every_state_field_and_record_order_stays_detectable(self):
        control = runner.canonical_single_player_dump(self.POSIX)
        for changed in (self.POSIX.replace(b"0x1.2c", b"0x1.2c00000000001"),
                        self.POSIX.replace(b"-0x0p+0", b"0x0p+0"),
                        self.POSIX.replace(b"feet=9", b"feet=8"),
                        self.POSIX.replace(b"unknown=keep", b"unknown=changed"),
                        self.POSIX.replace(b"uid=1", b"uid=2"),
                        self.POSIX.replace(b"unknown=keep", b"")):
            with self.subTest(changed=changed):
                self.assertNotEqual(control, runner.canonical_single_player_dump(changed))
        other = self.POSIX.replace(b"27 actor", b"28 actor")
        self.assertNotEqual(runner.canonical_single_player_dump(self.POSIX + other),
                            runner.canonical_single_player_dump(other + self.POSIX))

    def test_only_record_crlf_is_changed_and_binary64_low_bits_remain(self):
        self.assertEqual(runner.canonical_single_player_dump(b"27 actor uid=a\rb value=0x1.0000000000001p+0\r\n"),
                         b"27 actor uid=a\rb value=0x1.0000000000001p+0\n")


if __name__ == "__main__":
    unittest.main()
