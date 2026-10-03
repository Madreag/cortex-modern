import json
from pathlib import Path
import unittest

from acceptance_spectator import requirements, ownership_agreement


class SpectatorScenario(unittest.TestCase):
    def test_promoted_input_is_bound_to_the_authoritative_host_receipt(self):
        promoted = dict(seat=2, actor=17, ticket_incarnation=3, activation_tick=4000, freed_seat=2, host_authorized=True)
        departing = dict(seat=2)
        self.assertEqual(ownership_agreement(promoted, promoted, departing), [])
        for field in ("seat", "actor", "ticket_incarnation", "activation_tick", "freed_seat"):
            with self.subTest(field=field):
                changed = {**promoted, field:promoted[field]+1}
                self.assertTrue(ownership_agreement(promoted, changed, departing))
        self.assertTrue(ownership_agreement(promoted, promoted, {"seat":1}))
        self.assertTrue(ownership_agreement({**promoted, "host_authorized":False}, promoted, departing))

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
