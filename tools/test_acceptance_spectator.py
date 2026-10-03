import json
from pathlib import Path
import unittest

from acceptance_spectator import requirements


class SpectatorScenario(unittest.TestCase):
    def test_one_watcher_and_three_seated_processes(self):
        repo = Path(__file__).resolve().parents[1]
        scenario = json.loads((repo/"tools/e2e/world-spectator-promotion.json").read_text())
        self.assertEqual(len(scenario["peers"]), 4)
        self.assertEqual([p["name"] for p in scenario["peers"] if "CCCP_TEST_SIM_COST_US" in p.get("env", {})], ["spectator"])
        self.assertTrue(scenario["preserve_evidence"])
        for peer in scenario["peers"]:
            for field in ("menu_script", "probe", "input_script"):
                if field in peer:
                    self.assertTrue((repo/"tools/e2e"/peer[field]).is_file(), peer[field])

    def test_cost_on_a_seated_process_is_refused(self):
        repo = Path(__file__).resolve().parents[1]
        scenario = json.loads((repo/"tools/e2e/world-spectator-promotion.json").read_text())
        scenario["peers"][0]["env"]["CCCP_TEST_SIM_COST_US"] = "500000"
        self.assertIn("simulation cost must affect only the watcher", requirements(repo, scenario))


if __name__ == "__main__":
    unittest.main(verbosity=2)
