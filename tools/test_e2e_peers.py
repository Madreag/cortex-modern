"""A capture cannot claim separate boxes by relabeling two local players."""
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

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


class SceneAccepted(Exception):
    pass


class CommittedSceneTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        self.git("init", "-q")
        self.git("config", "core.autocrlf", "input")
        self.path = self.repo / "scene.json"
        self.content = b'{\n  "schema": 1,\n  "name": "committed scene"\n}\n'
        self.path.write_bytes(self.content)
        self.git("add", "scene.json")
        self.git("-c", "user.name=Scene check", "-c", "user.email=scene@example.invalid", "commit", "-qm", "Store the scene")
        self.commit = self.git("rev-parse", "HEAD").strip()
        self.digest = hashlib.sha256(self.content).hexdigest()
        self.request = self.root / "request.json"
        self.request.write_text(json.dumps(dict(box={"repo": str(self.repo)}, scene_relative="scene.json",
                                                commit=self.commit, scene_sha256=self.digest)), encoding="utf-8")

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.repo), *args], text=True)

    def tearDown(self):
        self.temporary.cleanup()

    def crlf_working_copy(self):
        self.path.write_bytes(self.content.replace(b"\n", b"\r\n"))
        self.git("add", "scene.json")
        self.assertEqual(self.git("status", "--porcelain"), "")
        self.assertEqual(self.git("show", self.commit + ":scene.json").encode(), self.content)

    def test_player_accepts_crlf_working_copy(self):
        self.crlf_working_copy()
        with mock.patch.object(peers, "check_build", return_value=({}, {}, {})), \
                mock.patch.object(peers, "check_scene", side_effect=SceneAccepted):
            with self.assertRaises(SceneAccepted):
                peers.play(self.request)

    def test_orchestrator_hashes_committed_blob(self):
        self.crlf_working_copy()
        options = SimpleNamespace(scenario=str(self.path), repo=self.repo, out=self.root / "capture",
                                  peer_boxes="host=Host,joiner=Joiner", boxes_file=self.root / "boxes.json")
        with mock.patch.object(peers, "check_scene"), mock.patch.object(peers, "selected_boxes", return_value={}), \
                mock.patch.object(peers.video, "write_json", side_effect=SceneAccepted) as write:
            with self.assertRaises(SceneAccepted):
                peers.capture_scene(options)
        self.assertEqual(write.call_args.args[1]["scene_sha256"], self.digest)

    def test_player_rejects_real_content_change(self):
        self.path.write_bytes(self.content.replace(b"committed scene", b"different scene").replace(b"\n", b"\r\n"))
        with mock.patch.object(peers, "check_build", return_value=({}, {}, {})), mock.patch.object(peers, "check_scene") as check:
            with self.assertRaises(ValueError):
                peers.play(self.request)
        check.assert_not_called()

    def test_player_rejects_different_committed_scene(self):
        request = json.loads(self.request.read_text(encoding="utf-8"))
        request["scene_sha256"] = hashlib.sha256(b"another committed scene").hexdigest()
        self.request.write_text(json.dumps(request), encoding="utf-8")
        with mock.patch.object(peers, "check_build", return_value=({}, {}, {})), mock.patch.object(peers, "check_scene") as check:
            with self.assertRaisesRegex(ValueError, "player scene differs from the original scene"):
                peers.play(self.request)
        check.assert_not_called()


class SpreadFightTest(unittest.TestCase):
    """Two boxes each bring one player's fight history; the collector keeps both and judges the fight once."""

    GATE = {"fight_hash_gate": {"peers": ["host", "joiner"], "running_ticks": 3}}

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def half(self, role, actors="a1"):
        from compare_sim_traces import CORE
        run = self.root / role / "run0"
        probe = run / f"{role}-stage" / "probe"
        probe.mkdir(parents=True)
        subsystems = {name: "h-" + name for name in CORE | {"controller", "scene", "terrain", "sim_rng", "lua_state"}}
        rows = [dict(round=7, tick=tick, phase="live", paused=False, sim_gated=True,
                     subsystems={**subsystems, "actors": actors if tick == 4 else "a1"}) for tick in range(1, 6)]
        (probe / "ticks.jsonl").write_text("".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")
        peers.video.feel_probes(self.GATE, {"root": str(run), "peers": [{"peer": role}]}, {})
        archive = self.root / f"{role}.tar"
        import tarfile
        with tarfile.open(archive, "w") as output:
            for path in sorted(run.rglob("*")):
                if path.is_file():
                    output.add(path, arcname="run0/" + path.relative_to(run).as_posix(), recursive=False)
        return archive

    def collect(self, joiner_actors):
        out = self.root / "collected"
        out.mkdir()
        for archive in (self.half("host"), self.half("joiner", joiner_actors)):
            peers.unpack(archive, out)
        capture = {"root": str(out / "run0"), "peers": [{"peer": "host"}, {"peer": "joiner"}]}
        return out, capture, peers.video.fight_hash_gate(self.GATE, capture)

    def test_both_players_histories_are_kept_and_judged_once(self):
        out, capture, result = self.collect("a1")
        self.assertTrue((out / "run0" / "fight-hashes-host.json").is_file() and (out / "run0" / "fight-hashes-joiner.json").is_file())
        self.assertEqual(result["status"], "PASS", result["errors"])
        self.assertEqual(sorted(result["coverage"]), ["host", "joiner"])
        self.assertTrue(all(peer["gates"]["fight-hashes"]["status"] == "PASS" for peer in capture["peers"]))

    def test_a_divergent_player_still_fails_the_fight(self):
        _, _, result = self.collect("a2")
        self.assertEqual(result["status"], "FAIL")
        self.assertEqual(result["first_difference"]["tick"], 4)


if __name__ == "__main__":
    unittest.main()
