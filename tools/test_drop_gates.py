"""A scene drops a peer when every part of an "all" gate is met; the resume scenes drop theirs only with a checkpoint on disk."""
import json
from pathlib import Path
import unittest

import e2e_video

SCENES = Path(__file__).resolve().parent / "e2e"
RETAINED = {"peer": "host", "log": "^\\[autosave\\] retained tick="}


def preflight(gate):
    scenario = {"peers": [{"name": "host"}, {"name": "client", "kill_when": gate}]}
    return e2e_video.run_preflight(scenario, {"name": "died"}, [], {})


class DropGateTests(unittest.TestCase):
    def test_an_all_gate_of_named_parts_is_accepted(self):
        self.assertIsNone(preflight({"all": [{"peer": "client", "sim_tick": 601}, RETAINED]}))

    def test_an_all_gate_with_a_part_that_waits_for_nothing_is_refused(self):
        refusal = preflight({"all": [{"peer": "client"}, RETAINED]})
        self.assertEqual((refusal or {}).get("class"), "harness", refusal)

    def test_an_empty_all_gate_is_refused(self):
        refusal = preflight({"all": []})
        self.assertEqual((refusal or {}).get("class"), "harness", refusal)

    def test_the_resume_scenes_drop_their_peers_only_after_the_host_retained_a_checkpoint(self):
        for name in ("mp-resume-from-disk.json", "mp-resume-from-disk-autosave.json"):
            scene = json.loads((SCENES / name).read_text(encoding="utf-8"))
            died = next(run for run in scene["runs"] if run["name"] == "died")
            for peer in died["peers"]:
                parts = e2e_video.gate_parts(peer.get("kill_when") or {})
                self.assertTrue(RETAINED in parts and {"peer": peer["name"], "sim_tick": 601} in parts,
                                f"{name} drops its {peer['name']} without waiting for the host's checkpoint on disk")
                self.assertIsNone(e2e_video.run_preflight({"peers": died["peers"]}, {"name": "died", "peers": [
                    {key: value for key, value in node.items() if key not in ("menu_script", "probe", "input_script")} for node in died["peers"]]}, [], {}))


if __name__ == "__main__":
    unittest.main()
