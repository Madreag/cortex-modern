"""One script drives every gameplay fixture case with the switches and inputs that case needs.

The expectations are the case rows and check calls of the battery's two gameplay wrappers, so a case
that loses its switches, its dump window, its input script, its check arguments or its -Only name
fails here with what the script holds now.
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE / "run_gameplay_fixtures.ps1"
FIXTURES = HERE / "fixtures"

# name -> (harness switches, sim dump window, input script under tools/fixtures)
EXPECTED = {
    "fire_reload": ([], "27:520", "fire_reload.txt"),
    "weapon_switch": ([], "27:210", "weapon_switch.txt"),
    "jetpack": ([], "27:145", "jetpack.txt"),
    "terrain_fire": ([], "27:225", "terrain_fire.txt"),
    "pie_reload": ([], "27:240", "pie_reload.txt"),
    "ai_orders": (["-AIOrderCommandHost", "-Ticks", "900"], "27:640", ""),
    "door_pass": (["-Ticks", "520", "-E2eSpawn", "ADoor:Door Slide Short:Base.rte:980:715:40:0"], "27:500", "door_pass.txt"),
    "crab_ai_order": (["-Ticks", "450", "-E2eSpawn", "ACrab:Crab:Base.rte:920:760:20"], "27:400", "crab_ai_order.txt"),
    "craft_cargo": (["-DeliverCommandHost", "-Ticks", "700"], "27:640", ""),
}
# The cases the wrappers kept in a second copy of the script.
DOOR_CRAB_CRAFT = ("door_pass", "crab_ai_order", "craft_cargo")
# Its switches carry the two editor-script paths, so only their variables are unchecked here.
HUMANS_VS_CPU_LITERALS = ("-Ticks", "900", "-MatchPreset", "Skirmish Defense", "-MatchMode", "coop-pve", "-HostUiScript", "-ClientUiScript")
HUMANS_VS_CPU_DUMP = "27:890"
# The extra arguments each case's semantic check is given, from the wrappers' Check-Case calls.
CHECK_EXTRAS = {
    "terrain_fire": ["--host-trace", "host_trace.json"],
    "humans_vs_cpu": ["--host-log", "host.out.txt", "--client-log", "client.out.txt"],
}

ROW = re.compile(
    r"@\{\s*name\s*=\s*'(?P<name>[^']+)'\s*;"
    r"\s*harness\s*=\s*@\((?P<harness>[^)]*)\)\s*;"
    r"\s*dump\s*=\s*'(?P<dump>[^']*)'\s*;"
    r"\s*script\s*=\s*'(?P<script>[^']*)'\s*;"
    r"\s*check\s*=\s*(?P<check>.*?)\s*\}\s*$",
    re.M,
)
QUOTED = re.compile(r"'([^']*)'")


def case_names(text: str) -> list[str]:
    return [match.group("name") for match in ROW.finditer(text)]


def parse_cases(text: str) -> dict[str, tuple[list[str], str, str]]:
    return {
        match.group("name"): (QUOTED.findall(match.group("harness")), match.group("dump"), match.group("script"))
        for match in ROW.finditer(text)
    }


def check_block(text: str, name: str) -> str:
    for match in ROW.finditer(text):
        if match.group("name") == name:
            return match.group("check")
    return ""


class ConsolidatedGameplayFixtures(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if not SCRIPT.is_file():
            raise AssertionError(f"the gameplay-fixture script is missing: {SCRIPT}")
        cls.text = SCRIPT.read_text(encoding="utf-8")
        cls.cases = parse_cases(cls.text)

    def test_every_case_row_is_the_one_its_wrapper_drove(self) -> None:
        for name, expected in EXPECTED.items():
            with self.subTest(case=name):
                observed = self.cases.get(name)
                self.assertIsNotNone(observed, f"case {name} is absent; the table holds {sorted(self.cases)}")
                self.assertEqual(expected, observed, f"case {name} row changed: {observed}")

    def test_the_door_crab_and_craft_cases_are_in_the_one_script(self) -> None:
        missing = [name for name in DOOR_CRAB_CRAFT if name not in self.cases]
        self.assertEqual([], missing, f"{missing} are not in {SCRIPT.name}; it holds {sorted(self.cases)}")

    def test_each_case_is_driven_once(self) -> None:
        names = case_names(self.text)
        duplicates = sorted({name for name in names if names.count(name) > 1})
        self.assertEqual([], duplicates, f"a case is driven twice per run: {duplicates}")

    def test_the_humans_vs_cpu_row_keeps_its_literals(self) -> None:
        observed = self.cases.get("humans_vs_cpu")
        self.assertIsNotNone(observed, f"humans_vs_cpu is absent; the table holds {sorted(self.cases)}")
        harness, dump, _ = observed
        for literal in HUMANS_VS_CPU_LITERALS:
            with self.subTest(literal=literal):
                self.assertIn(literal, harness, f"humans_vs_cpu lost {literal}: {harness}")
        self.assertEqual(HUMANS_VS_CPU_DUMP, dump, f"humans_vs_cpu dump window changed: {dump}")
        row = [match.group(0) for match in ROW.finditer(self.text) if match.group("name") == "humans_vs_cpu"][0]
        for variable in ("$hostUi", "$clientUi"):
            with self.subTest(variable=variable):
                self.assertIn(variable, row, f"humans_vs_cpu no longer passes {variable}: {row}")

    def test_each_check_keeps_its_extra_arguments(self) -> None:
        for name, extras in CHECK_EXTRAS.items():
            with self.subTest(case=name):
                block = check_block(self.text, name)
                self.assertTrue(block.startswith("{ param($dir)"), f"case {name} has no check block: {block!r}")
                for extra in extras:
                    self.assertIn(extra, block, f"case {name} check lost {extra}: {block}")
        for name in set(EXPECTED) - set(CHECK_EXTRAS):
            with self.subTest(case=name):
                self.assertEqual("$null", check_block(self.text, name), f"case {name} gained a check block")

    def test_run_case_takes_its_four_arguments_in_order(self) -> None:
        self.assertIn(
            "function Run-Case([string]$Name, [string[]]$HarnessArgs, [string]$Dump, [string]$Script) {",
            self.text,
            "Run-Case's parameter list changed, so the loop's positional arguments no longer line up",
        )

    def test_the_loop_passes_the_row_in_that_order(self) -> None:
        for line in ("$d = Run-Case $case.name $case.harness $case.dump $inputScript",
                     "$inputScript = $(if ($case.script) { \"$fixtures/$($case.script)\" } else { '' })",
                     "if ($case.check) { $extra = @(& $case.check $d) }",
                     "Check-Case $case.name $d $extra"):
            with self.subTest(line=line):
                self.assertIn(line, self.text, f"the loop no longer drives the table this way: {line!r}")

    def test_only_selects_by_case_name(self) -> None:
        self.assertIn(
            "if ($Only.Count -gt 0 -and $Name -notin $Only) { return $null }",
            self.text,
            "Run-Case no longer filters on -Only, so a driver's -Only argv would run every case",
        )

    def test_the_script_runs_out_of_the_repository(self) -> None:
        for default in ("[string]$Lane = $PSScriptRoot",
                        "[string]$HarnessCommon = (Join-Path $PSScriptRoot 'harness_common.ps1')",
                        "[string]$FixtureRoot = (Join-Path $PSScriptRoot 'fixtures')"):
            with self.subTest(default=default):
                self.assertIn(default, self.text, f"the script no longer defaults to the repository: {default!r}")
        for helper in ("check_fixture.py", "run_interp_e2e.ps1", "write_editor_scripts.py", "harness_common.ps1"):
            with self.subTest(helper=helper):
                self.assertTrue((HERE / helper).is_file(), f"{helper} is not in tools/, so a clone cannot run the script")
        for name, (_, _, script) in EXPECTED.items():
            if script:
                with self.subTest(case=name):
                    self.assertTrue((FIXTURES / script).is_file(), f"{script} is not in tools/fixtures/, so case {name} cannot run")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], "-v", *sys.argv[1:]])
