"""Controls for the audit's own gate: what a transition has to record before the run can be complete."""

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent / "contracts"))
import run_audit

BINARY = "a" * 64


def case(operation, **fields):
    item = {
        "case": operation,
        "operation": operation,
        "completed": True,
        "process_clean": True,
        "desktop_unchanged": True,
        "binary": BINARY,
        "errors": [],
        "graph_capture_problems": [],
        "graph_observations": [
            {"observation": "before", "serialized": True, "problem_count": 0},
            {"observation": "after", "serialized": True, "problem_count": 0},
        ],
        "prepared": True,
        "applied": True,
    }
    item.update(fields)
    return item


class Gate(unittest.TestCase):
    def gate(self, item, expect_load="any"):
        return run_audit.gate(item, expect_load, BINARY)

    def test_a_transition_expected_to_apply_must_report_applied(self):
        for operation in ("save", "memory", "file", "hold", "preview"):
            with self.subTest(operation=operation):
                self.assertEqual(self.gate(case(operation)), [])
                self.assertEqual(self.gate(case(operation, applied=False)), ["applied"])

    def test_observe_and_stage_answer_for_themselves(self):
        for operation in ("observe", "stage", "memory-perturb"):
            with self.subTest(operation=operation):
                self.assertEqual(self.gate(case(operation, applied=False)), [])

    def test_a_load_transaction_answers_to_expect_load(self):
        refused = case("ordinary-load:bad_closure", applied=False)
        self.assertEqual(self.gate(refused, "rejected"), [])
        self.assertEqual(self.gate(refused, "accepted"), ["applied"])
        self.assertEqual(self.gate(case("ordinary-load:valid_full"), "accepted"), [])

    def test_an_accepted_load_may_not_log_engine_errors(self):
        for line in (
            "ERROR: the saved script state did not restore: injected",
            "stack traceback: injected",
            "RTE Assert: injected",
        ):
            with self.subTest(line=line):
                accepted = case("ordinary-load:valid_full", errors=[line])
                self.assertEqual(self.gate(accepted, "accepted"), ["errors"])
                self.assertEqual(run_audit.unexpected_errors(accepted), [line])

    def test_a_refused_load_may_log_the_refusal_it_provoked(self):
        refused = case(
            "ordinary-load:missing_native_preset",
            applied=False,
            errors=[
                "[scriptgraph] restore failed: the AHuman Audit Missing Preset of Base.rte could not be rebuilt",
                'ERROR: There is no AHuman of the Preset name "Audit Missing Preset" defined in the "Base.rte" Data Module!',
            ],
        )
        self.assertEqual(self.gate(refused, "rejected"), [])
        self.assertEqual(run_audit.unexpected_errors(refused), [])

    def test_a_rejected_replacement_keeps_the_retained_candidate_running(self):
        staged = case(
            "stage-ordinary-reject:valid_full:bad_closure",
            staged_candidate={
                "valid_staged": True,
                "replacement_accepted": False,
                "retained_launched": True,
            },
            errors=[
                'ERROR: Could not load game "bad_closure": script graph validation failed'
            ],
        )
        self.assertEqual(self.gate(staged, "rejected"), [])

    def test_a_refusal_may_not_hide_a_crash(self):
        refused = case(
            "ordinary-load:bad_closure",
            applied=False,
            errors=[
                'ERROR: Could not load game "bad_closure": script graph validation failed',
                "RTE Abort: injected",
            ],
        )
        self.assertEqual(self.gate(refused, "rejected"), ["errors"])
        self.assertEqual(run_audit.unexpected_errors(refused), ["RTE Abort: injected"])

    def test_a_non_load_transition_is_strict_about_errors(self):
        held = case(
            "hold",
            applied=False,
            errors=[
                "[scriptgraph] reinstate failed: the script state names object 1, which is not in the world"
            ],
        )
        self.assertEqual(self.gate(held), ["applied", "errors"])

    def test_the_run_conditions_are_still_gated(self):
        self.assertEqual(self.gate(case("memory", completed=False)), ["completed"])
        self.assertEqual(
            self.gate(case("memory", process_clean=False)), ["process_clean"]
        )
        self.assertEqual(
            self.gate(case("memory", desktop_unchanged=False)), ["desktop_unchanged"]
        )
        self.assertEqual(self.gate(case("memory", binary="b" * 64)), ["binary"])

    def test_a_graph_that_did_not_serialise_is_a_failure(self):
        self.assertEqual(
            self.gate(case("memory", graph_capture_problems=["save snapshot refused"])),
            ["graph_capture"],
        )
        broken = case(
            "memory",
            graph_observations=[
                {"observation": "after", "serialized": True, "problem_count": 2}
            ],
        )
        self.assertEqual(self.gate(broken), ["graphs_serialized"])


if __name__ == "__main__":
    unittest.main()
