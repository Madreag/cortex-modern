"""Invoke each tightened oracle with empty or skipped input.

An oracle given nothing must fail by name: empty census lines are not [] == [], a brain-kill count is
not len(kills) >= human_seats, and a timed-out spectate process is not a completed one.
"""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from net_activity_launch import compare_census_lines
from test_net_brainless_spectate import net_arm, sp_arm


class OracleVacuity(unittest.TestCase):
    def test_empty_offline_census_names_the_missing_lines(self):
        result = compare_census_lines([], ["actor uid=1"])
        self.assertFalse(result["pass"])
        self.assertEqual(result["reason"], "offline census tick-1 lines missing")

    def test_empty_match_census_names_the_missing_lines(self):
        result = compare_census_lines(["actor uid=1"], [])
        self.assertFalse(result["pass"])
        self.assertEqual(result["reason"], "match census tick-1 lines missing")

    def test_empty_census_pair_is_not_a_vacuous_pass(self):
        """Base tree compared [] == [] and passed."""
        result = compare_census_lines([], [])
        self.assertFalse(result["pass"])
        self.assertEqual(result["reason"], "offline census tick-1 lines missing")

    def test_brainless_kill_count_quotes_the_production_detail(self):
        class DummyHarness:
            def run_isolated(self, *args, **kwargs):
                return None

            def lane(self, name, lane):
                return {"lane": name}

        extra = (
            "[spectate-probe] brain-kill simms=5000 team=0 uid=1\n"
            "[spectate-probe] brain-kill simms=5001 team=1 uid=2\n"
            "[spectate-probe] brain-kill simms=5002 team=2 uid=3\n"
        )
        with patch("test_net_brainless_spectate.load_harness", return_value=DummyHarness()):
            with tempfile.TemporaryDirectory() as tmp:
                out = Path(tmp)
                for peer in ("host", "client"):
                    peer_dir = out / "e2e" / "brainless_spectate" / peer
                    peer_dir.mkdir(parents=True)
                    (peer_dir / "stdout.log").write_text(extra, encoding="utf-8")
                result = net_arm(out, 48400, True, 1, 1, dedicated=False)
        host = next(row for row in result["spectate_checks"] if row["name"] == "host_brains_destroyed")
        self.assertEqual(host["status"], "fail")
        self.assertEqual(host["detail"], "brain-kill lines=3 human seats=2")

    def test_brainless_process_completed_quotes_exit_and_timeout(self):
        import run_sim_test
        import win32_test_runner

        class DummyRun:
            def __init__(self, *args, **kwargs):
                pass

            def start(self):
                return self

            def finish(self):
                return {"exit_code": 1, "timed_out": True}

            def close(self):
                return None

        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "sp"
            runtime = Path(tmp) / "runtime"
            runtime.mkdir()
            with patch.object(run_sim_test, "prepare_runtime", return_value=runtime), \
                    patch.object(win32_test_runner, "IsolatedRun", DummyRun), \
                    patch("test_net_brainless_spectate.stage_user_module"), \
                    patch("test_net_brainless_spectate.sha256", return_value="x"):
                result = sp_arm(out, True, False, 1, 1)
        completed = next(row for row in result["checks"] if row["name"] == "process_completed")
        self.assertEqual(completed["status"], "fail")
        self.assertEqual(completed["detail"], "exit_code=1 timed_out=True")


if __name__ == "__main__":
    unittest.main()
