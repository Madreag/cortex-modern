"""Detect the humans_vs_cpu UI-probe comparer split.

Base tree routed the host-vs-client compare through a paused-tolerant script and
the host-vs-replay compare through the strict comparer. Both call sites must use
the same $rowCompare / $rowCompareArgs, and a UI-probe row must name this tree's
compare_sim_traces.py --min-ticks 1 (PAUSED_CORE, not a skipper).
"""
from pathlib import Path
import re
import unittest

SCRIPT = Path(__file__).resolve().parent / "run_interp_e2e.ps1"


class InterpE2ERouting(unittest.TestCase):
    def setUp(self):
        self.text = SCRIPT.read_text(encoding="utf-8")

    def test_both_compare_call_sites_use_the_same_script_and_argv(self):
        sites = [line.strip() for line in self.text.splitlines()
                 if re.search(r"python \$rowCompare\b", line)]
        self.assertEqual(sites, [
            "& python $rowCompare $hostTrace $clientTrace @rowCompareArgs 2>&1 |",
            "& python $rowCompare $hostTrace $replayTrace @rowCompareArgs 2>&1 |",
        ])
        self.assertIn('$rowCompare = Join-Path $repo "tools\\compare_sim_traces.py"', self.text)
        self.assertIn('$rowCompareArgs = @("--min-ticks", "1")', self.text)
        self.assertNotIn("compare_e2e_simgated_active.py", self.text)


if __name__ == "__main__":
    unittest.main()
