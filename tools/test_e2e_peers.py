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

    def test_assigned_port_keeps_the_committed_scene(self):
        document = {"port_base": 49430}
        self.assertEqual(peers.scene_port(document), 49430)
        self.assertEqual(peers.scene_port(document, 49471), 49471)
        self.assertEqual(document, {"port_base": 49430})
        for invalid in (peers.video.PORT_LO - 1, peers.video.PORT_HI + 1):
            with self.assertRaises(SystemExit):
                peers.scene_port(document, invalid)

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
        (run / role).mkdir()
        subsystems = {name: "h-" + name for name in CORE | {"controller", "scene", "terrain", "sim_rng", "lua_state"}}
        rows = [dict(round=7, tick=tick, phase="live", paused=False, sim_gated=True,
                     subsystems={**subsystems, "actors": actors if tick == 4 else "a1"}) for tick in range(1, 6)]
        (probe / "ticks.jsonl").write_text("".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")
        (run / role / "stdout.log").write_text("[horizon-path] wait_us=59 tick=511 origin=106 expired=0 count=1 p99_us=59\n", encoding="utf-8")
        (run / role / "horizon_path_grid.json").write_text('{"count":11496,"request_stalls":0,"request_wait_max_us":0}\n', encoding="utf-8")
        # A half finishes its own player only; play() leaves the shared gates to the collector.
        peers.video.finish_run({"name": "fight", **self.GATE}, {}, {"name": "run0", "root": str(run), "peers": []}, {},
                               SimpleNamespace(fps=30, sheet_every=15, scratch_root=self.root, scratch_limit_bytes=1 << 40),
                               shared_gates=False)
        (run / "review.json").rename(run / (role + "-review.json"))
        archive = self.root / f"{role}.tgz"
        import tarfile
        with tarfile.open(archive, "w:gz") as output:
            for path in sorted(run.rglob("*")):
                if path.is_file():
                    output.add(path, arcname="run0/" + path.relative_to(run).as_posix(), recursive=False)
        return archive

    def collect(self, joiner_actors):
        out = self.root / "collected"
        out.mkdir()
        for archive in (self.half("host"), self.half("joiner", joiner_actors)):
            peers.unpack(archive, out)
        capture = {"root": str(out / "run0"), "peers": [{"peer": role, "root": str(out / "run0" / role)} for role in ("host", "joiner")]}
        peers.video.feel_probes(self.GATE, capture, {})
        return out, capture, json.loads((out / "run0" / "fight-hashes.json").read_text(encoding="utf-8"))

    def test_both_players_histories_are_kept_and_judged_once(self):
        out, capture, result = self.collect("a1")
        self.assertEqual(sorted(path.name for path in (out / "run0").glob("fight-hashes*.json")), ["fight-hashes.json"])
        self.assertEqual(result["status"], "PASS", result["errors"])
        self.assertEqual(sorted(result["coverage"]), ["host", "joiner"])
        self.assertTrue(all(peer["gates"]["fight-hashes"]["status"] == "PASS" for peer in capture["peers"]))

    def test_a_divergent_player_still_fails_the_fight(self):
        _, _, result = self.collect("a2")
        self.assertEqual(result["status"], "FAIL")
        self.assertEqual(result["first_difference"]["tick"], 4)

    def test_each_players_pathfinding_waits_are_quoted(self):
        _, _, result = self.collect("a1")
        for role in ("host", "joiner"):
            waits = result["pathfinding"][role]
            self.assertEqual(waits["horizon_report"]["request_stalls"], 0)
            self.assertEqual(waits["positive_waits"], 1)
            self.assertEqual(waits["worst_wait"]["wait_us"], 59)

    def test_one_players_history_alone_cannot_pass(self):
        out = self.root / "collected"
        out.mkdir()
        peers.unpack(self.half("host"), out)
        capture = {"root": str(out / "run0"), "peers": [{"peer": "host", "root": str(out / "run0" / "host")}]}
        peers.video.feel_probes(self.GATE, capture, {})
        self.assertEqual(capture["peers"][0]["gates"]["fight-hashes"]["status"], "FAIL")

    def test_a_second_copy_of_a_file_is_refused_by_name(self):
        out = self.root / "collected"
        out.mkdir()
        archive = self.half("host")
        peers.unpack(archive, out)
        with self.assertRaisesRegex(ValueError, "would overwrite another player's evidence: run0/host"):
            peers.unpack(archive, out)


class JoinerBoxTest(unittest.TestCase):
    """A Linux box joins exactly as the Mac does: its own checkout, its own build and a visible window."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.boxes = self.root / "boxes.json"
        self.boxes.write_text(json.dumps({"boxes": [
            dict(name="Ally", os="win32", hostname="Erol-Ally7", ssh="ally-ts", repo="C:/tree", scratch="C:/mx", python="python"),
            dict(name="Mac", os="darwin", hostname="Erol-Mac", ssh="bro", repo="/mac/tree", scratch="/mac/mx", python="/usr/bin/python3",
                 binary="build-gcc/CortexCommand"),
            dict(name="Linux", os="linux", hostname="ubuntu", ssh="3090-ts", repo="/linux/tree", scratch="/linux/mx",
                 python="/usr/bin/python3", binary="build-gcc/CortexCommand")]}))

    def tearDown(self):
        self.temporary.cleanup()

    def test_mac_and_linux_joiners(self):
        for joiner in ("Mac", "Linux"):
            self.assertEqual(peers.selected_boxes("host=Ally,joiner=" + joiner, self.boxes)["joiner"]["name"], joiner)

    def test_only_a_windows_host(self):
        with self.assertRaisesRegex(ValueError, "needs a Windows host"):
            peers.selected_boxes("host=Linux,joiner=Mac", self.boxes)
        with self.assertRaisesRegex(ValueError, "needs a Windows host"):
            peers.selected_boxes("host=Mac,joiner=Ally", self.boxes)

    def test_linux_player_script(self):
        box = peers.selected_boxes("host=Ally,joiner=Linux", self.boxes)["joiner"]
        request = self.root / "request.json"
        request.write_text(json.dumps(dict(out="/linux/mx/s10-run0-1")))
        with mock.patch.object(peers, "ssh"), mock.patch.object(peers, "copy_file"), \
                mock.patch.object(peers.subprocess, "Popen") as popen:
            peers.launch(box, request, None)
        command = popen.call_args.args[0][-1]
        self.assertEqual(command, "/bin/bash /linux/tree/tools/e2e/peer_linux.sh /linux/mx/s10-run0-1/request.json /linux/tree")
        script = (Path(peers.__file__).parent / "e2e/peer_linux.sh").read_text()
        self.assertIn("unset CCCP_HEADLESS", script)
        self.assertIn('export DISPLAY="${DISPLAY:-:0}"', script)

    def test_the_collectors_own_box_plays_without_ssh(self):
        box = dict(peers.selected_boxes("host=Ally,joiner=Linux", self.boxes)["joiner"], ssh="local")
        self.assertEqual(peers.remote_command(box, ["/usr/bin/python3", "tool.py", "--pack", Path("/linux/mx/request.json")]),
                         ["/usr/bin/python3", "tool.py", "--pack", "/linux/mx/request.json"])
        self.assertEqual(peers.remote_command(dict(box, ssh="3090-ts"), ["python3", "a b"])[-2:], ["3090-ts", "python3 'a b'"])
        source, copied, back = self.root / "source.json", self.root / "copied.json", self.root / "back.json"
        source.write_text("{}")
        peers.copy_file(box, source, copied)
        peers.copy_file(box, back, copied, download=True)
        self.assertEqual((copied.read_text(), back.read_text()), ("{}", "{}"))

    def test_engine_levers_are_test_levers_only(self):
        self.assertEqual(peers.engine_levers(["CC_TEST_LUA_RNG_TRACE=1"]), {"CC_TEST_LUA_RNG_TRACE": "1"})
        self.assertEqual(peers.engine_levers([]), {})
        for value in ("CCCP_HEADLESS=0", "CC_TEST_LUA_RNG_TRACE", "PATH=/tmp"):
            with self.assertRaises(ValueError):
                peers.engine_levers([value])

    def test_a_boxes_file_entry_places_only_its_own_engines(self):
        host = dict(name="Host box", os="win32", hostname="host-computer", runner_env={"CC_RUNNER_AFFINITY_MASK": "0x0000ffff"})
        joiner = dict(name="Joiner box", os="linux", hostname="joiner-computer")
        path = Path(self.temporary.name) / "boxes.json"
        path.write_text(json.dumps({"boxes": [host, joiner]}))
        boxes = peers.selected_boxes("host=Host box,joiner=Joiner box", path)
        self.assertEqual(peers.box_runner_env(boxes["host"]), {"CC_RUNNER_AFFINITY_MASK": "0x0000ffff"})
        self.assertEqual(peers.box_runner_env(boxes["joiner"]), {})
        for settings in ({"CC_TEST_TICK_COST_DETAILS": "1"}, {"CC_RUNNER_AFFINITY_MASK": 65535}, ["CC_RUNNER_AFFINITY_MASK"]):
            path.write_text(json.dumps({"boxes": [dict(host, runner_env=settings), joiner]}))
            with self.assertRaises(ValueError):
                peers.selected_boxes("host=Host box,joiner=Joiner box", path)

    def test_headed_player_environment(self):
        environment = peers.video.headed_environment({"CCCP_HEADLESS": "1", "CC_TEST_READBACK_TIMING": "1"})
        self.assertEqual(environment, {"CC_TEST_READBACK_TIMING": "1", "CC_TEST_VISIBLE_WINDOW": "1"})

    def test_windows_host_visibility_follows_its_assignment(self):
        self.assertFalse(peers.headed_box({"os": "win32"}))
        self.assertTrue(peers.headed_box({"os": "win32", "headed": True}))
        with mock.patch.object(peers.video.sys, "platform", "win32"):
            environment = peers.video.headed_environment({"CCCP_HEADLESS": "1"})
        self.assertEqual(environment, {"CC_TEST_VISIBLE_WINDOW": "1"})
        self.assertEqual(peers.video.HEADED_SETTINGS, {"EnableVSync": 1})

    def test_headed_players_keep_the_games_vsync_instead_of_the_headless_cap(self):
        scenario = peers.video.load_scenario("fight15-s10")
        options = SimpleNamespace(repo=Path(peers.__file__).resolve().parents[1], size=None, port=peers.video.PORT_LO, fps=30,
                                  setting=[], render_cap=60, fullstate_every=0, dry_run=True, tokens={})
        for headed, flag in ((False, True), (True, False)):
            options.headed = headed
            staged = peers.video._run_one(options, scenario, peers.scene_runs(scenario)[0], 0, self.root / str(headed))
            for peer in staged["peers"]:
                self.assertEqual("-feel-render-settings" in peer["args"], flag)
                self.assertEqual(peer["presentation"], "headed" if headed else "headless")
                self.assertEqual(peer["render_cap"], None if headed else 60)

    def test_collector_reads_a_players_early_pace_line(self):
        out = self.root / "native-joiner"
        (out / "run0" / "joiner").mkdir(parents=True)
        request = out / "request.json"
        request.write_text(json.dumps(dict(out=str(out), role="joiner", run_name="run0", signal_files=[])))
        self.assertIsNone(peers.read_player_files(request)["pace"])
        (out / "run0" / "joiner" / peers.video.PACE_REPORT).write_text(json.dumps({"line": "[fight-pace] RED"}))
        self.assertEqual(peers.read_player_files(request)["pace"]["line"], "[fight-pace] RED")


class WindowsJoinersTest(unittest.TestCase):
    """Four players on four Windows boxes: every joiner plays through its own box's game task, each on its own engine host."""

    BOXES = [("EROL-PC", "EROL-PC", "erol-pc"), ("Ally", "Erol-Ally7", "ally-ts"), ("EDITH", "EDITH", "edith-ts"), ("Z13", "Erol-Tablet", "z13-ts")]

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.boxes = self.root / "boxes.json"
        self.write_boxes(self.BOXES)

    def tearDown(self):
        self.temporary.cleanup()

    def write_boxes(self, boxes):
        self.boxes.write_text(json.dumps({"boxes": [
            dict(name=name, os="win32", hostname=host, ssh=alias, repo=f"C:/{alias}/tree", scratch=f"C:/{alias}/mx", python="python",
                 session_dir="C:/session1", task="cortex-session1") for name, host, alias in boxes]}))

    def test_windows_joiners_play_through_their_own_box(self):
        boxes = peers.selected_boxes("host=EROL-PC,joiner1=Ally,joiner2=EDITH,joiner3=Z13", self.boxes)
        self.assertEqual(list(boxes), ["host", "joiner1", "joiner2", "joiner3"])
        self.assertEqual([box["name"] for box in boxes.values()], ["EROL-PC", "Ally", "EDITH", "Z13"])
        request = self.root / "request.json"
        request.write_text(json.dumps(dict(out="C:/ally-ts/mx/s11-run0-1")))
        with mock.patch.object(peers, "ssh"), mock.patch.object(peers, "copy_file"), \
                mock.patch.object(peers.subprocess, "Popen") as popen:
            peers.launch(boxes["joiner1"], request, None)
        command = popen.call_args.args[0]
        self.assertEqual(command[-2:], ["ally-ts", "pwsh -NoProfile -File C:/ally-ts/tree/tools/e2e/peer_box.ps1 -Request C:/ally-ts/mx/s11-run0-1/request.json"])
        self.assertFalse(peers.headed_box(boxes["joiner2"]))

    def test_each_player_needs_its_own_engine_host(self):
        with self.assertRaisesRegex(ValueError, "a different box for each player"):
            peers.selected_boxes("host=EROL-PC,joiner1=Ally,joiner2=Ally", self.boxes)
        with self.assertRaisesRegex(ValueError, "a different box for each player"):
            peers.selected_boxes("joiner1=Ally,joiner2=EDITH", self.boxes)
        self.write_boxes([*self.BOXES[:3], ("Z13", "EDITH", "z13-ts")])
        with self.assertRaisesRegex(ValueError, "two assignments name one engine host"):
            peers.selected_boxes("host=EROL-PC,joiner1=Ally,joiner2=EDITH,joiner3=Z13", self.boxes)

    def capture(self, hosts=None):
        hosts = hosts or {role: role + "-computer" for role in ("host", "joiner1", "joiner2", "joiner3")}
        run = dict(name="run0", peer_boxes={role: role + " box" for role in hosts}, peers=[])
        for role, host in hosts.items():
            root = self.root / role
            root.mkdir(exist_ok=True)
            record = dict(pid=42, exe_sha256=role + "-binary", box=role + " box", engine_hostname=host,
                          source_commit="scene-commit", scene_sha256="scene-hash")
            (root / "launch.json").write_text(json.dumps(record))
            run["peers"].append(dict(peer=role, box=role + " box", box_hostname=host, root=str(root), record=record))
        return dict(topology="spread", source={"tip": "scene-commit"}, scene_sha256="scene-hash", runs=[run])

    def test_four_players_on_four_hosts(self):
        self.assertTrue(peers.spread_check(self.capture())["passed"])
        capture = self.capture()
        capture["runs"][0]["peers"].pop()
        self.assertIn("run0: a player's record is absent", peers.spread_check(capture)["reasons"])
        hosts = {role: role + "-computer" for role in ("host", "joiner1", "joiner2")}
        hosts["joiner3"] = "joiner1-computer"
        self.assertIn("run0: peers ran on one engine host", peers.spread_check(self.capture(hosts))["reasons"])

    def test_every_players_marker_reaches_every_other_player(self):
        document = peers.scene("fight15-s11")
        peers.check_scene(document)
        run = peers.scene_runs(document)[0]
        roles = peers.scene_roles(document, run)
        self.assertEqual(roles, ["host", "joiner1", "joiner2", "joiner3"])
        files = peers.player_files(document, run)
        for marker in ("host-stage/probe/countdown.mark", "joiner1-stage/probe/joined.mark", "joiner2-stage/probe/joined.mark",
                       *(role + "-stage/probe/fight-ticks-complete.json" for role in roles)):
            self.assertIn(marker, files)
        requests = {}
        for role in roles:
            out = self.root / ("native-" + role)
            (out / "run0" / (role + "-stage/probe")).mkdir(parents=True)
            (out / (role + "-started.json")).write_text("{}")
            (out / "run0" / (role + "-stage/probe/fight-ticks-complete.json")).write_text('{"name":"fight-ticks-complete"}')
            request = out / "request.json"
            request.write_text(json.dumps(dict(out=str(out), role=role, run_name="run0", signal_files=files)))
            requests[role] = request
        (self.root / "native-joiner1/run0/joiner1-stage/probe/joined.mark").write_text("")
        states = {role: peers.read_player_files(requests[role])["files"] for role in roles}
        for role in roles:
            incoming = {path: data for other, state in states.items() if other != role for path, data in state.items()}
            peers.write_player_files(requests[role], incoming)
            received = self.root / ("native-" + role) / "run0"
            for other in roles:
                self.assertTrue((received / (other + "-stage/probe/fight-ticks-complete.json")).is_file(), (role, other))
            self.assertEqual((received / "joiner1-stage/probe/joined.mark").is_file(), True)

    def test_a_player_packs_only_its_own_files(self):
        out = self.root / "native-joiner2"
        for role in ("host", "joiner1", "joiner2", "joiner3"):
            (out / "run0" / (role + "-stage/probe")).mkdir(parents=True)
            (out / "run0" / (role + "-stage/probe/fight-ticks-complete.json")).write_text("{}")
        (out / "run0" / "joiner2").mkdir()
        (out / "run0" / "joiner2" / "stdout.log").write_text("joiner2\n")
        request = out / "request.json"
        request.write_text(json.dumps(dict(out=str(out), role="joiner2", run_index=0, scene_relative="tools/e2e/fight15-s11.json",
                                           box=dict(repo=str(Path(peers.__file__).resolve().parents[1])))))
        with mock.patch("builtins.print"):
            peers.pack(request)
        import tarfile
        with tarfile.open(out.with_suffix(".tgz")) as archive:
            names = sorted(archive.getnames())
        self.assertEqual(names, ["run0/joiner2-stage/probe/fight-ticks-complete.json", "run0/joiner2/stdout.log"])

    def test_a_player_can_name_every_other_players_probe_directory(self):
        repo = Path(peers.__file__).resolve().parents[1]
        request = self.root / "request.json"
        request.write_text(json.dumps(dict(box=dict(repo=str(repo), scratch=str(self.root), os="win32"), role="joiner2", run_index=0,
                                           out=str(self.root / "native-joiner2"), commit="tip", scene_relative="tools/e2e/fight15-s11.json",
                                           scene_sha256="scene", port=peers.video.PORT_LO, fps=30, sheet_every=15)))
        seen = {}

        def run_one(options, *args):
            seen.update(options.tokens)
            raise SceneAccepted
        with mock.patch.object(peers, "check_build", return_value=({}, {}, {})), \
                mock.patch.object(peers, "committed_scene_sha256", return_value="scene"), \
                mock.patch.object(peers.video, "requirement_findings", return_value=[]), \
                mock.patch.object(peers.video, "check_scratch_budget"), mock.patch.object(peers.video, "_run_one", side_effect=run_one):
            with self.assertRaises(SceneAccepted):
                peers.play(request)
        self.assertEqual(sorted(seen), ["PROBE_DIR_host", "PROBE_DIR_joiner1", "PROBE_DIR_joiner2", "PROBE_DIR_joiner3"])
        self.assertEqual(seen["PROBE_DIR_joiner1"], self.root / "native-joiner2" / "run0" / "joiner1-stage/probe")

    def test_a_run_name_that_puts_windows_game_files_past_the_path_limit_is_refused(self):
        boxes = peers.selected_boxes("host=EROL-PC,joiner1=Ally,joiner2=EDITH,joiner3=Z13", self.boxes)
        roots = {"EROL-PC": "C:/cortex-project/mx/scene-peers-desync", "Ally": "C:/cortex-project/mx/two-box-harness",
                 "EDITH": "D:/mx/two-box-harness", "Z13": "C:/cortex-project/mx/scene-peers-desync"}
        # The Z13's joiner of fe5db-erolpc-ally-mac-z13-4p1 could not read this 125-character Data file at 260 characters.
        longest = len("Data/Browncoats.rte/Scenes/Objects/BunkerSystems/RefineryAuthorizationConsole/RefineryAuthorizationConsolePieSliceActions.lua")
        for run in ("fe5db-erolpc-ally-mac-z13-4p1", "fe5db-4p2"):
            for box in boxes.values():
                box["scratch"] = roots[box["name"]] + "/" + run
            if run == "fe5db-4p2":
                peers.check_windows_paths(boxes, run, "run0", Path("."), longest)
                continue
            with self.assertRaisesRegex(ValueError, "^Z13: the joiner3's game files would reach 260 characters"):
                peers.check_windows_paths(boxes, run, "run0", Path("."), longest)
        self.assertEqual(peers.longest_data_path(Path(peers.__file__).resolve().parents[1]), longest)

    def test_a_scene_names_one_host_and_lowercase_players(self):
        document = peers.scene("fight15-s11")
        for names in (("host", "host", "joiner1"), ("host",), ("host", "Joiner1"), ("host", "joiner1", "joiner1")):
            changed = copy.deepcopy(document)
            changed["peers"] = [dict(document["peers"][1], name=name) for name in names]
            with self.assertRaisesRegex(ValueError, "one host and at least one joiner"):
                peers.check_scene(changed)


if __name__ == "__main__":
    unittest.main()
