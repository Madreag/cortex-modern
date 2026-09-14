"""Replay retained audit items through gate() for the memory-perturb exception."""
from copy import deepcopy
from pathlib import Path
import json
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_audit

S44_UI = Path(r"D:/mx/s44-1/ui_perturb/result.json")
S44_PRIM = Path(r"D:/mx/s44-1/primitives/result.json")
S43_PRIM = Path(r"D:/mx/s43-1/primitives/result.json")

# gate() lists recorded 2026-09-14 01:41 MST before the memory-perturb exception.
BEFORE_S44_UI_TERRAIN = [
    "contract_mismatches:PRINT: [reference-contract-mismatch] perturbed UI.cart_items",
    "contract_mismatches:PRINT: [reference-contract-mismatch] perturbed UI.cart_cost",
    "contract_mismatches:PRINT: [reference-contract-mismatch] perturbed UI.order_cost",
    "contract_mismatches:PRINT: [reference-contract-mismatch] perturbed UI.order_mass",
    "contract_mismatches:PRINT: [reference-contract-mismatch] perturbed UI.order_passengers",
    "contract_mismatches:PRINT: [reference-contract-mismatch] perturbed UI.owned_items",
    "contract_mismatches:PRINT: [reference-contract-mismatch] perturbed UI.editor_mode",
    "contract_mismatches:PRINT: [reference-contract-mismatch] perturbed UI.background_manual",
    "contract_mismatches:PRINT: [reference-contract-mismatch] perturbed UI.background_duration",
    "contract_mismatches:PRINT: [reference-contract-mismatch] perturbed UI.background_scroll_axes",
    "contract_mismatches:PRINT: [reference-contract-mismatch] perturbed UI.background_scroll_interval",
    "contract_mismatches:PRINT: [reference-contract-mismatch] perturbed UI.background_scroll_step",
    "contract_mismatches:PRINT: [reference-contract-mismatch] perturbed UI.vector_value",
    "contract_mismatches:PRINT: [reference-contract-mismatch] perturbed UI.timer_sim",
    "contract_mismatches:PRINT: [reference-contract-mismatch] perturbed UI.timer_real",
    "contract_mismatches:PRINT: [reference-contract-mismatch] perturbed UI.alarm_value",
]
BEFORE_S44_UI_AHUMAN = list(BEFORE_S44_UI_TERRAIN)
BEFORE_S44_PRIM_PERTURB = [
    "contract_mismatches:PRINT: [reference-contract-mismatch] perturbed primitive.source_pixel",
    "contract_mismatches:PRINT: [reference-contract-mismatch] perturbed primitive.primitive_lua_instance",
    "contract_mismatches:PRINT: [reference-contract-mismatch] perturbed primitive.vertex_value",
]

NON_PERTURB_GATE_BEFORE = {
    str(S44_UI): {},
    str(S44_PRIM): {
        "observe_60": [],
        "save_60": [],
        "stage_60": [],
        "memory_60": [],
        "file_60": [],
        "hold_60": [],
        "preview_60": [],
    },
    str(S43_PRIM): {
        "observe_60": ["fixture_armed"],
        "save_60": ["fixture_armed"],
        "stage_60": ["fixture_armed"],
        "memory_60": ["fixture_armed"],
        "file_60": ["fixture_armed"],
        "hold_60": ["fixture_armed"],
        "preview_60": ["fixture_armed"],
    },
}


def load_results(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))["results"]


def by_case(path):
    return {item["case"]: item for item in load_results(path)}


def call_gate(item):
    return run_audit.gate(item, "any", item["binary"], None)


def perturb_failures(failures):
    return [
        name
        for name in failures
        if name.startswith("contract_mismatches:")
        or name in ("perturbation_not_detected", "restored_contract_checks_failed")
    ]


class PerturbGate(unittest.TestCase):
    def test_1_s44_perturb_items_ignore_perturbed_mismatches(self):
        cases = (
            (S44_UI, "memory-perturb_60_TerrainObject"),
            (S44_UI, "memory-perturb_60_AHuman"),
            (S44_PRIM, "memory-perturb_60"),
        )
        for path, name in cases:
            with self.subTest(case=name):
                failures = call_gate(by_case(path)[name])
                self.assertEqual(perturb_failures(failures), [])

    def test_2_undetected_perturb_is_red(self):
        item = deepcopy(by_case(S44_PRIM)["memory-perturb_60"])
        item["contract_mismatches"] = [
            line
            for line in item["contract_mismatches"]
            if " perturbed " not in line
        ]
        item["perturbation_detected"] = False
        failures = call_gate(item)
        self.assertIn("perturbation_not_detected", failures)

    def test_3_unrestored_after_is_red(self):
        item = deepcopy(by_case(S44_PRIM)["memory-perturb_60"])
        for check in item["contract_checks"]:
            if check.get("observation") == "after":
                check["mismatches"] = 1
        item["restored_contract_checks_passed"] = False
        failures = call_gate(item)
        self.assertIn("restored_contract_checks_failed", failures)

    def test_4_non_perturb_mismatch_stays_a_failure(self):
        item = deepcopy(by_case(S44_PRIM)["observe_60"])
        item["contract_mismatches"] = [
            "[reference-contract-mismatch] after UI.cart_items",
        ]
        failures = call_gate(item)
        self.assertIn(
            "contract_mismatches:[reference-contract-mismatch] after UI.cart_items",
            failures,
        )

    def test_5_non_perturb_items_match_frozen_before_lists(self):
        for path, expected_by_case in NON_PERTURB_GATE_BEFORE.items():
            items = load_results(path)
            non_perturb = [
                item
                for item in items
                if item["operation"].split(":", 1)[0] != "memory-perturb"
            ]
            self.assertEqual(
                {item["case"] for item in non_perturb},
                set(expected_by_case),
            )
            for item in non_perturb:
                with self.subTest(path=path, case=item["case"]):
                    self.assertEqual(call_gate(item), expected_by_case[item["case"]])


if __name__ == "__main__":
    unittest.main()
