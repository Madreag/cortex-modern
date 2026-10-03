import json
from pathlib import Path
import unittest
from unittest.mock import patch

from acceptance_spectator import requirements, ownership_agreement, collect


class SpectatorScenario(unittest.TestCase):
    def test_tool_only_export_reports_missing_sources_without_crashing(self):
        scenario=dict(acceptance_row='spectator',peers=[],required_engine_levers=['dump_world_ownership'])
        faults=requirements(Path('/virtual/no-engine-source'),scenario)
        self.assertIn('engine receipt/lever missing: dump_world_ownership',faults)

    def test_promotion_takes_authorization_from_the_matching_host_receipt(self):
        native=dict(seat=1,actor=17,ticket_incarnation=2,activation_tick=7000,freed_seat=1)
        documents={
            'before-release.ownership.json':dict(configuration=dict(seats=3,world_max_spectators=1,persistent_world=True),seated_first_tick=1,lockstep_frame=2400),
            'after-promotion.ownership.json':dict(promotion={**native,'host_authorized':True}),
            'promoted-input.ownership.json':dict(promotion={**native,'input_tick':7002,'input_created_tick':7001,'applied_actor':17,'applied_seat':1,'applied_incarnation':2,'applied_input':{'right':True}}),
            'departing-seat.ownership.json':dict(ownership=dict(seat=1)),
            'spectator-image.ownership.json':dict(watch=dict(role='Spectator',image_received=True,first=1201,last=6061)),
            'crawl-complete.ownership.json':dict(watch=dict(last=6061),sim_cost=dict(process='spectator',sim_cost_us=500000,first_tick=6000,last_tick=6059,start_ms=5000,end_ms=35000))}
        with patch('acceptance_spectator.read',side_effect=lambda path:documents.get(Path(path).name,{})), \
             patch.object(Path,'is_file',return_value=True),patch.object(Path,'read_text',return_value='native log'), \
             patch('acceptance_spectator.peer_receipt',return_value={}), \
             patch('acceptance_spectator.live_hashes',return_value={}):
            result=collect(Path('/virtual/run'))
        self.assertIs(result['facts']['promotion'].get('host_authorized'),True)

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

    def test_one_watcher_three_seated_processes_and_a_dedicated_host(self):
        repo = Path(__file__).resolve().parents[1]
        scenario = json.loads((repo/"tools/e2e/world-spectator-promotion.json").read_text())
        self.assertEqual(len(scenario["peers"]), 5)
        self.assertEqual({p['name'] for p in scenario['peers']},{'host','seated-one','seated-two','seated-three','spectator'})
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
