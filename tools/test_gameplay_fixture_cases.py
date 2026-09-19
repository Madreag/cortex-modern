"""The consolidated gameplay-fixture script drives every case the two wrappers drove.

The expectations are the case rows of the pre-consolidation wrappers
`fg6d-battery/test_gameplay_fixtures.ps1` (sha256 53C406114AE5B34014DDB827CFC4863D464BB67CB89D7722676B277C0DC4CE5D)
and `fg6d-battery/test_gameplay_fixtures_crab.ps1` (sha256 D84721CE50DAB9794234B723430E35EDBF12BEE0C10F6054D1A4536E4D7064BE):
a case that loses its switches, its dump window, its input script or its -Only name fails here with the
row it now has.
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE / "run_gameplay_fixtures.ps1"

FIXTURES = "D:/Projects/stage2_p4/fixtures"
# name -> (harness switches, sim dump window, input script)
EXPECTED = {
    "fire_reload": ([], "27:520", f"{FIXTURES}/fire_reload.txt"),
    "weapon_switch": ([], "27:210", f"{FIXTURES}/weapon_switch.txt"),
    "jetpack": ([], "27:145", f"{FIXTURES}/jetpack.txt"),
    "terrain_fire": ([], "27:225", f"{FIXTURES}/terrain_fire.txt"),
    "pie_reload": ([], "27:240", f"{FIXTURES}/pie_reload.txt"),
    "ai_orders": (["-AIOrderCommandHost", "-Ticks", "900"], "27:640", ""),
    "door_pass": (["-Ticks", "520", "-E2eSpawn", "ADoor:Door Slide Short:Base.rte:980:715:40:0"], "27:500", f"{FIXTURES}/door_pass.txt"),
    "crab_ai_order": (["-Ticks", "450", "-E2eSpawn", "ACrab:Crab:Base.rte:920:760:20"], "27:400", f"{FIXTURES}/crab_ai_order.txt"),
    "craft_cargo": (["-DeliverCommandHost", "-Ticks", "700"], "27:640", ""),
}
# The three cases the item-3b work drove through a second copy of the wrapper.
ITEM_3B = ("door_pass", "crab_ai_order", "craft_cargo")
# Its switches are built from $hostUi/$clientUi, so its row holds variables where the others hold literals.
VARIABLE_CASES = ("humans_vs_cpu",)

ROW = re.compile(
    r"@\{\s*name\s*=\s*'(?P<name>[^']+)'\s*;"
    r"\s*harness\s*=\s*@\((?P<harness>[^)]*)\)\s*;"
    r"\s*dump\s*=\s*'(?P<dump>[^']*)'\s*;"
    r"\s*script\s*=\s*'(?P<script>[^']*)'"
)
QUOTED = re.compile(r"'([^']*)'")


def case_names(text: str) -> list[str]:
    return [match.group("name") for match in ROW.finditer(text)]


def parse_cases(text: str) -> dict[str, tuple[list[str], str, str]]:
    cases = {}
    for match in ROW.finditer(text):
        cases[match.group("name")] = (QUOTED.findall(match.group("harness")), match.group("dump"), match.group("script"))
    return cases


class ConsolidatedGameplayFixtures(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if not SCRIPT.is_file():
            raise AssertionError(f"the consolidated gameplay-fixture script is missing: {SCRIPT}")
        cls.text = SCRIPT.read_text(encoding="utf-8")
        cls.cases = parse_cases(cls.text)

    def test_every_case_row_survived_the_consolidation(self) -> None:
        for name, expected in EXPECTED.items():
            with self.subTest(case=name):
                observed = self.cases.get(name)
                self.assertIsNotNone(observed, f"case {name} is absent; the table holds {sorted(self.cases)}")
                self.assertEqual(expected, observed, f"case {name} row changed: {observed}")

    def test_the_three_item_3b_cases_are_in_the_one_script(self) -> None:
        missing = [name for name in ITEM_3B if name not in self.cases]
        self.assertEqual([], missing, f"the 3b cases {missing} are not in {SCRIPT.name}; it holds {sorted(self.cases)}")

    def test_each_case_is_driven_once(self) -> None:
        names = case_names(self.text)
        duplicates = sorted({name for name in names if names.count(name) > 1})
        self.assertEqual([], duplicates, f"a case is driven twice per run: {duplicates}")

    def test_the_variable_case_is_still_driven(self) -> None:
        for name in VARIABLE_CASES:
            with self.subTest(case=name):
                self.assertIn(f"name = '{name}'", self.text, f"case {name} is absent from {SCRIPT.name}")

    def test_only_selects_by_case_name(self) -> None:
        self.assertIn(
            "if ($Only.Count -gt 0 -and $Name -notin $Only) { return $null }",
            self.text,
            "Run-Case no longer filters on -Only, so a driver's -Only argv would run every case",
        )

    def test_the_loop_drives_the_table(self) -> None:
        self.assertIn("foreach ($case in $cases) {", self.text, "the script no longer drives its case table in one loop")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], "-v", *sys.argv[1:]])
