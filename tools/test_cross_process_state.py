"""Controls for the cross-process real-clock projection of the raw contract-audit documents."""

import gzip
import json
from pathlib import Path
import sys
import tempfile
import unittest

if __package__:
    from . import cross_process_state as projection
else:
    import cross_process_state as projection

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent / "contracts"))
import run_audit


TIMER = "mo[1048625].MovableObject.m_RestTimer.Timer.m_StartRealTime"
SIMULATION = "mo[1048625].MovableObject.m_Vel.Vector.m_X"


def document(path, lines):
    with gzip.open(path, "wb") as stream:
        stream.write("".join(line + "\n" for line in lines).encode())
    return path


def records(path):
    with gzip.open(path, "rt", encoding="utf-8") as stream:
        return [json.loads(line) for line in stream]


class ProjectionRules(unittest.TestCase):
    def test_only_the_named_real_clock_fields_are_projected(self):
        for field in (
            TIMER,
            "activity.Activity.m_DeathTimer[0].Timer.m_StartRealTime",
            "scene.Scene.m_PartialPathUpdateTimer.Timer.m_StartRealTime",
            "frame.text_timer[0].Timer.m_StartRealTime",
            "clock.real_time",
            "clock.sim_accumulator",
        ):
            with self.subTest(field=field):
                self.assertIsNotNone(projection.real_clock_reason(field))
        for field in (
            SIMULATION,
            "clock.sim_time",
            "clock.sim_count",
            "clock.dt",
            "clock.ticks_per_second",
            "mo[3].MovableObject.m_RestTimer.Timer.m_StartSimTime",
            "clock.real_time_scale",
            "mo[3].MovableObject.m_RestTimer.Timer.m_StartRealTimeMS",
            "rng.sim",
        ):
            with self.subTest(field=field):
                self.assertIsNone(projection.real_clock_reason(field))

    def test_every_rule_names_its_writer(self):
        for kind, name, reason in projection.REAL_CLOCK_FIELDS:
            with self.subTest(name=name):
                self.assertIn(kind, ("exact", "suffix"))
                self.assertTrue(
                    len(reason) > 80 and (".cpp" in reason or ".h" in reason)
                )

    def test_family_keeps_the_field_and_drops_the_indices(self):
        self.assertEqual(
            projection.family(
                "mo[7].AHuman.m_Paths[0][3].LimbPath.m_SegTimer.Timer.m_StartRealTime"
            ),
            "mo[].AHuman.m_Paths[][].LimbPath.m_SegTimer.Timer.m_StartRealTime",
        )
        self.assertEqual(
            projection.field_of("clock.real_time = 89713"), "clock.real_time"
        )
        self.assertIsNone(projection.field_of("not a field"))


class CrossProcessComparison(unittest.TestCase):
    def setUp(self):
        self.directory = Path(tempfile.mkdtemp())
        self.artifact = self.directory / "differences.jsonl.gz"

    def compare(self, first, second, cross_process=True):
        return run_audit.compare_state_files(
            document(self.directory / "a.gz", first),
            document(self.directory / "b.gz", second),
            self.artifact,
            cross_process=cross_process,
        )

    def test_real_clock_anchors_are_projected_per_family_with_their_reason(self):
        result = self.compare(
            [
                TIMER + " = 89713",
                "clock.real_time = 89713",
                "clock.sim_accumulator = 4",
                "clock.sim_time = 100",
                SIMULATION + " = 1.5",
            ],
            [
                TIMER + " = 98497",
                "clock.real_time = 98497",
                "clock.sim_accumulator = 9",
                "clock.sim_time = 100",
                SIMULATION + " = 1.5",
            ],
        )
        self.assertTrue(result["equal"])
        self.assertEqual(result["changed_lines"], 0)
        self.assertEqual(result["projected_lines"], 3)
        self.assertEqual(
            sorted(result["projected"]),
            [
                "clock.real_time",
                "clock.sim_accumulator",
                "mo[].MovableObject.m_RestTimer.Timer.m_StartRealTime",
            ],
        )
        for family in result["projected"].values():
            self.assertEqual(family["count"], 1)
            self.assertTrue(family["reason"])
        self.assertEqual(
            [record["projected"] for record in records(self.artifact)],
            [True, True, True],
        )

    def test_a_simulation_field_is_still_reported(self):
        result = self.compare(
            [SIMULATION + " = 1.5", TIMER + " = 1", "clock.sim_time = 100"],
            [SIMULATION + " = 1.6", TIMER + " = 2", "clock.sim_time = 101"],
        )
        self.assertFalse(result["equal"])
        self.assertEqual(result["changed_lines"], 2)
        self.assertEqual(result["projected_lines"], 1)
        retained = [
            record["field"]
            for record in records(self.artifact)
            if not record["projected"]
        ]
        self.assertEqual(retained, [SIMULATION, "clock.sim_time"])

    def test_a_shifted_document_is_not_projected(self):
        result = self.compare(
            [TIMER + " = 1", "clock.real_time = 1"],
            ["clock.real_time = 2", TIMER + " = 2", "clock.sim_accumulator = 3"],
        )
        self.assertFalse(result["equal"])
        self.assertEqual(result["changed_lines"], 3)
        self.assertEqual(result["projected_lines"], 0)
        self.assertEqual((result["first_lines"], result["second_lines"]), (2, 3))

    def test_same_process_comparison_keeps_every_byte(self):
        result = self.compare([TIMER + " = 1"], [TIMER + " = 2"], cross_process=False)
        self.assertFalse(result["equal"])
        self.assertEqual(result["changed_blocks"], 1)
        self.assertNotIn("projected", result)

    def test_identical_documents_compare_equal_either_way(self):
        for cross_process in (True, False):
            with self.subTest(cross_process=cross_process):
                result = self.compare(
                    [TIMER + " = 1", SIMULATION + " = 2"],
                    [TIMER + " = 1", SIMULATION + " = 2"],
                    cross_process=cross_process,
                )
                self.assertTrue(result["equal"])
                self.assertEqual(result["first_sha256"], result["second_sha256"])


if __name__ == "__main__":
    unittest.main()
