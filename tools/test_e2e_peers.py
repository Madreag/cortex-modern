"""A capture cannot claim separate boxes by relabeling two local players."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

import e2e_peers as peers


class SceneBoxesTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.capture = dict(topology="spread", source={"tip": "scene-commit"}, scene_sha256="scene-hash", runs=[])
        run = dict(name="run0", peer_boxes={"host": "Host box", "joiner": "Joiner box"}, peers=[])
        self.capture["runs"].append(run)
        for role, box, host in (("host", "Host box", "host-computer"), ("joiner", "Joiner box", "joiner-computer")):
            root = self.root / role
            root.mkdir()
            record = dict(pid=42, exe_sha256=role + "-binary", box=box, engine_hostname=host,
                          source_commit="scene-commit", scene_sha256="scene-hash")
            (root / "launch.json").write_text(json.dumps(record))
            run["peers"].append(dict(peer=role, box=box, box_hostname=host, root=str(root), record=record))

    def tearDown(self):
        self.temporary.cleanup()

    def test_separate_boxes(self):
        self.assertTrue(peers.spread_check(self.capture)["passed"])

    def test_two_labels_on_one_host(self):
        joiner = self.capture["runs"][0]["peers"][1]
        joiner["box_hostname"] = joiner["record"]["engine_hostname"] = "host-computer"
        (Path(joiner["root"]) / "launch.json").write_text(json.dumps(joiner["record"]))
        check = peers.spread_check(self.capture)
        self.assertFalse(check["passed"])
        self.assertIn("run0: peers ran on one engine host", check["reasons"])

    def test_single_box_capture(self):
        self.capture["topology"] = "single-box"
        self.assertIn("capture topology is not spread", peers.spread_check(self.capture)["reasons"])

    def test_changed_record(self):
        host = self.capture["runs"][0]["peers"][0]
        record = copy.deepcopy(host["record"])
        record["pid"] += 1
        (Path(host["root"]) / "launch.json").write_text(json.dumps(record))
        self.assertIn("run0/host: retained engine record differs at pid", peers.spread_check(self.capture)["reasons"])

    def test_missing_player(self):
        self.capture["runs"][0]["peers"].pop()
        self.assertFalse(peers.spread_check(self.capture)["passed"])

    def test_scene_change(self):
        self.capture["scene_sha256"] = "changed-scene"
        self.assertFalse(peers.spread_check(self.capture)["passed"])

    def test_foreground_probe_matches_the_game_task(self):
        script = (Path(peers.__file__).parent / "e2e/peer_box.ps1").read_text()
        self.assertIn('clear = (h == 0) or title in ("Program Manager", "") or state.value in (1, 5, 6, 7)', script)
        self.assertLess(script.index("if (`$fg -notmatch 'clear=True')"), script.index("`$env:CC_RUNNER_IGNORE_FULLSCREEN = '1'"))

    def test_fight_completion_file_reaches_the_other_player(self):
        document = peers.scene("fight15-s10")
        peers.check_scene(document)
        files = peers.player_files(document, peers.scene_runs(document)[0])
        requests = {}
        for role in ("host", "joiner"):
            out = self.root / ("native-" + role)
            (out / "run0" / (role + "-stage/probe")).mkdir(parents=True)
            (out / (role + "-started.json")).write_text('{}')
            request = out / "request.json"
            request.write_text(json.dumps(dict(out=str(out), role=role, run_name="run0", signal_files=files)))
            requests[role] = request
        marker = self.root / 'native-host/run0/host-stage/probe/fight-ticks-complete.json'
        marker.write_bytes(b'{')
        self.assertEqual(peers.read_player_files(requests['host'])['files'], {})
        content = b'{"frame":73000,"name":"fight-ticks-complete"}\n'
        marker.write_bytes(content)
        copied = peers.read_player_files(requests['host'])['files']
        peers.write_player_files(requests['joiner'], copied)
        received = self.root / 'native-joiner/run0/host-stage/probe/fight-ticks-complete.json'
        self.assertEqual(received.read_bytes(), content)
        with self.assertRaises(ValueError):
            peers.write_player_files(requests['host'], copied)


if __name__ == "__main__":
    unittest.main()
