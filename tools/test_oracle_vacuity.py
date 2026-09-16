"""Invoke each tightened oracle with empty or skipped input.

Base tree compared empty census lines as [] == [], skipped missing pair
saves, treated None brains as equal, skipped missing offline traces, accepted
len(kills) >= human_seats, and ignored a timed-out spectate process.
"""
from __future__ import annotations

import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE / "seat_facts"))

import compare_offline as offline_mod
from check_world import compare as compare_world
from compare_offline import compare as compare_offline
from compare_offline import main as offline_main
from net_activity_launch import compare_census_lines
from phase_b import pair_arms
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

    def test_missing_pair_saves_name_the_missing_snapshot(self):
        with patch("phase_b.invoke", return_value=0):
            with tempfile.TemporaryDirectory() as tmp:
                with self.assertRaisesRegex(RuntimeError, "missing pair snapshot for world compare"):
                    pair_arms(Path(tmp))

    def test_failed_world_compare_stops_the_arm(self):
        """Base tree invoked check_world without require_zero and discarded a failed compare."""
        from phase_b import invoke as real_invoke

        def run_pair_ok(script, argv, out, require_zero=False):
            if Path(script).name == "run_pair.py":
                Path(out).write_text("ok\n", encoding="utf-8")
                return {"command": [], "exit": 0, "log": str(out)}
            return real_invoke(script, argv, out, require_zero=require_zero)

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            pair = root / "reseat" / "e2e" / "snapshot_p5"
            for number, peer in enumerate(("host", "client"), 1):
                dest = pair / peer / "runtime" / "Userdata" / "UserSavedGames.rte"
                dest.mkdir(parents=True)
                save = dest / f"p5snap_p{number}.ccsave"
                with zipfile.ZipFile(save, "w") as archive:
                    archive.writestr("Index.ini", "x")
            with patch("phase_b.invoke", side_effect=run_pair_ok):
                with self.assertRaisesRegex(RuntimeError, r"check_world\.py exited 1; comparison did not complete"):
                    pair_arms(root)

    def test_none_brains_cannot_pass_brain_records_equal(self):
        with patch("check_world.world", side_effect=[
            (b"a", {"brains": None, "alarm_bits": []}),
            (b"a", {"brains": None, "alarm_bits": []}),
        ]):
            result = compare_world(Path("host"), Path("client"))
        self.assertFalse(result["checks"]["brain_records_present"])
        self.assertFalse(result["checks"]["brain_records_equal"])
        self.assertFalse(result["pass"])

    def test_missing_offline_traces_name_the_missing_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            absent = Path(tmp) / "no-retained"
            with patch.object(offline_mod, "RETAINED_PIE", absent / "pie"), \
                    patch.object(offline_mod, "RETAINED_AK47", absent / "ak47"):
                result = compare_offline(Path(tmp))
                self.assertTrue(result["rows"])
                for row in result["rows"]:
                    self.assertFalse(row["present"])
                    self.assertEqual(row["reason"], "missing traces for offline compare")
                self.assertEqual(offline_main([str(tmp)]), 1)

    def test_brainless_kill_count_quotes_the_production_detail(self):
        class DummyHarness:
            def lane(self, name, lane):
                return {"lane": name}

        with patch("test_net_brainless_spectate.load_harness", return_value=DummyHarness()):
            with tempfile.TemporaryDirectory() as tmp:
                out = Path(tmp)
                for peer in ("host", "client"):
                    (out / "e2e" / "brainless_spectate" / peer).mkdir(parents=True)
                result = net_arm(out, 48400, True, 1, 1, dedicated=False)
        host = next(row for row in result["spectate_checks"] if row["name"] == "host_brains_destroyed")
        self.assertEqual(host["status"], "fail")
        self.assertEqual(host["detail"], "brain-kill lines=0 human seats=2")

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
