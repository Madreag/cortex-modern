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

    def test_a_windows_box_puts_its_named_tool_folders_first_on_the_game_tasks_path(self):
        script = (Path(peers.__file__).parent / "e2e/peer_box.ps1").read_text()
        self.assertIn("$prepend = @($requestData.box.path_prepend)", script)
        self.assertLess(script.index("`$env:CC_RUNNER_IGNORE_FULLSCREEN = '1'"), script.index("$pathLine\n"))
        self.assertLess(script.index("$pathLine\n"), script.index("& python -u '$tree\\tools\\e2e_peers.py' --play"))

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
            request.write_text(json.dumps(dict(out=str(out), scene_out=str(out), role=role, run_name="run0", signal_files=files)))
            requests[role] = request
        marker = self.root / 'native-host/run0/host-stage/probe/fight-ticks-complete.json'
        marker.write_bytes(b'{')
        self.assertEqual(peers.read_player_files(requests['host'])['files'], {})
        content = b'{"frame":73000,"name":"fight-ticks-complete"}\n'
        marker.write_bytes(content)
        copied = peers.read_player_files(requests['host'])['files']
        peers.write_player_files(requests['joiner'], {"files": copied})
        received = self.root / 'native-joiner/run0/host-stage/probe/fight-ticks-complete.json'
        self.assertEqual(received.read_bytes(), content)
        with self.assertRaises(ValueError):
            peers.write_player_files(requests['host'], {"files": copied})


class CommittedScenesTest(unittest.TestCase):
    def test_every_public_scene_passes_the_launchers_own_check(self):
        # A scene the two-machine launcher would refuse is caught here, before any box is spent on it.
        refused = {}
        for path in sorted((Path(__file__).resolve().parent / "e2e").glob("*.json")):
            if path.name.endswith(".probe.json"):
                continue
            document = peers.read(path)
            if isinstance(document, dict) and document.get("public_directory"):
                try:
                    peers.check_scene(document)
                except ValueError as error:
                    refused[path.name] = str(error)
        self.assertEqual(refused, {})


class FightEndCheckTest(unittest.TestCase):
    def test_the_fights_end_reads_the_view_state_not_the_absence_of_text(self):
        # A unit lost in the fight's last seconds puts the game's own "Lost control of remote body!" on screen; the end check asks
        # whether the pan's body selection is over, which no passing message changes.
        for name in ("fight15-s10", "fight15-s11", "fight15-s12"):
            document = peers.scene(name)
            for script, text in document["scripts"].items():
                if not script.endswith(".probe"):
                    continue
                checks = [step for step in json.loads(text)["steps"] if step.get("op") == "assert_scene"]
                self.assertFalse([step for step in checks if step.get("equals", {}).get("screen_text") == ""], (name, script))
                self.assertEqual(checks[-1]["equals"].get("actor_selection"), False, (name, script, checks[-1]))

    def test_the_host_ends_the_match_only_after_every_player_checked_the_end(self):
        # The end check waits 90 ticks; a host that paused to end the match first left a joiner's ticks stopped (f-s10h-1, step 356).
        for name in ("fight15-s10", "fight15-s10h", "fight15-s11", "fight15-s12"):
            document = peers.scene(name)
            probes = {script[:-len(".probe")]: json.loads(text)["steps"] for script, text in document["scripts"].items() if script.endswith(".probe")}
            joiners = [peer for peer in probes if peer != "host"]
            for peer, steps in probes.items():
                end = max(index for index, step in enumerate(steps) if step.get("op") == "assert_scene" and step.get("equals", {}).get("actor_selection") is False)
                if peer == "host":
                    escape = next(index for index in range(end, len(steps)) if steps[index] == {"op": "key_down", "key": "Escape"})
                    waited = [steps[index]["path"] for index in range(end, escape) if steps[index]["op"] == "wait_file"]
                    self.assertEqual(waited, [f"{{PROBE_DIR_{joiner}}}/end-checked.json" for joiner in joiners], (name, peer))
                else:
                    self.assertEqual(steps[end + 1], {"op": "signal", "name": "end-checked"}, (name, peer))


class HeldHostSceneTest(unittest.TestCase):
    def test_the_held_host_scene_holds_the_hosts_own_seat_on_every_run(self):
        import fight15_held_host
        committed = json.loads((fight15_held_host.ROOT / "fight15-s10h.json").read_text(encoding="utf-8"))
        self.assertEqual(committed, fight15_held_host.held_host(json.loads((fight15_held_host.ROOT / "fight15-s10.json").read_text(encoding="utf-8"))))
        document = peers.scene("fight15-s10h")
        peers.check_scene(document)
        host, joiner = (next(peer for peer in document["peers"] if peer["name"] == name) for name in ("host", "joiner"))
        tick, milliseconds = map(int, host["args"][host["args"].index("-net-test-live-stall") + 1].split(":"))
        # The host alone stalls, past the slow player bound (3 ticks) and inside the fight's twenty minutes.
        self.assertGreater(milliseconds, 3 * 1000 / 60)
        self.assertLess(tick, document["fight_hash_gate"]["running_ticks"])
        self.assertNotIn("-net-test-live-stall", joiner["args"])
        item = next(row for row in document["checklist"] if row["id"] == "host-seat-held-host")
        self.assertEqual(item["peer"], "host")
        self.assertTrue(any("peer=1 held=1" in pattern for pattern in item["log_regex"]), item)
        self.assertTrue(any("seat-reclaimed peer=1" in pattern for pattern in item["log_regex"]), item)
        # The hold is the stall's: one the host earns later on its own (f-s10h-2, frame 28558) does not count.
        import re
        held = next(pattern for pattern in item["log_regex"] if "held=1" in pattern)
        self.assertTrue(re.search(held, "[net-seat-ai] peer=1 held=1 frame=6045 tick=6045 cpu_teams=13"))
        self.assertFalse(re.search(held, "[net-seat-ai] peer=1 held=1 frame=28558 tick=28558 cpu_teams=13"))
        # And the joiner never waits on the stalled host past A1's 50 ms (f-s10h-1/-2: 1,656 and 1,639 ms at frame 6022-6023).
        (never,) = next(row for row in document["checklist"] if row["id"] == "joiner-never-waited-joiner")["forbidden_log_regex"]
        self.assertTrue(re.search(never, "[net-frame-wait] frame=6023 wait_ms=1639 on=Captain blocked=remote-input duplicates=0"))
        self.assertFalse(re.search(never, "[net-frame-wait] frame=6030 wait_ms=11 on=Captain"))
        self.assertFalse(re.search(never, "[net-frame-wait] frame=8049 wait_ms=500 on=Captain"))
        self.assertEqual(host["env"].get("CC_TEST_RETURN_INPUT_DETAILS"), "1")
        # The lever's own freeze of the host's screen is named and left out of the host's drawn frames and stills, nothing else.
        (excluded,) = document["drawn_frames_exclude"]
        self.assertEqual(excluded["peer"], "host")
        self.assertTrue(excluded["first_tick"] < tick < excluded["last_tick"] and excluded["last_tick"] - excluded["first_tick"] <= 180, excluded)
        (still,) = document["allowed_stills"]
        self.assertEqual(still["peer"], "host")
        self.assertTrue(still.get("first_tick", 0) < tick < still.get("last_tick", 0) and still["last_tick"] - still["first_tick"] <= 180, still)

    def test_a_scenes_still_window_of_ticks_allows_that_still_alone(self):
        # The recorder's pictures, 30 a second: the host's screen freezes 2 s at tick 6000 and again, on its own, at tick 7500.
        rows, wall = [], 0.0
        for tick in range(5000, 9001, 2):
            wall += 2000.0 if tick in (6002, 7502) else 1000.0 / 30
            rows.append(dict(frame=len(rows), wall_ms=wall, sim_tick=tick, saved=True, screen="game", service_state="Running"))
        stalls = [(next(row["wall_ms"] for row in rows if row["sim_tick"] == tick - 2) - rows[0]["wall_ms"],
                   next(row["wall_ms"] for row in rows if row["sim_tick"] == tick) - rows[0]["wall_ms"]) for tick in (6002, 7502)]
        spans = [(begin / 1000.0, end / 1000.0) for begin, end in stalls]
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / "frames.jsonl").write_text("".join(json.dumps(row) + "\n" for row in rows))
            (Path(folder) / "events.jsonl").write_text("".join(json.dumps(dict(message=f"video_mark minute-0{minute}-host", wall_ms=rows[0]["wall_ms"] + 60000.0 * minute - 50000.0)) + "\n" for minute in (1, 2, 3)))
            import fight15_held_host
            scene = fight15_held_host.held_host(json.loads((fight15_held_host.ROOT / "fight15-s10.json").read_text(encoding="utf-8")))
            allowed = []
            stills = peers.video.running_stills(spans, rows, allowed=allowed, named=peers.video.named_still_windows(scene, "run0", "host", folder))
        self.assertEqual([still["frames"][0] for still in allowed], [rows.index(next(row for row in rows if row["sim_tick"] == 6000))])
        self.assertEqual(len(stills), 1, stills)
        self.assertGreater(stills[0]["start_s"], spans[0][1])

    def test_a_scenes_named_window_is_left_out_of_that_peers_drawn_frames(self):
        def window(tick, max_ms):
            return (f"[frame-pace] mode=lockstep recorder=1 recorder_hz=30 target_hz=60.00 first_tick={tick - 60} tick={tick} frames=60 wall_ms=1000.0 "
                    f"fps=59.80 interval_p50_ms=16.67 interval_p95_ms=17.50 interval_max_ms={max_ms:.2f}")
        log = "\n".join(window(60 * index, 2035.0 if 60 * index == 6000 else 18.0) for index in range(1, 201)) + "\n"
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / "stdout.log").write_text(log)
            peer = dict(peer="host", root=folder)
            judged = {row["id"]: row for row in peers.video.capture_evidence_items({}, dict(name="run0"), peer)}
            self.assertEqual(judged["drawn-frames-host"]["probe"], "fail")
            scene = dict(drawn_frames_exclude=[dict(peer="host", first_tick=5940, last_tick=6120, reason="the stall")])
            judged = {row["id"]: row for row in peers.video.capture_evidence_items(scene, dict(name="run0"), peer)}
            self.assertEqual(judged["drawn-frames-host"]["probe"], "pass", judged["drawn-frames-host"])
            self.assertEqual(judged["drawn-frames-host"]["excluded_windows"][0]["reason"], "the stall")
            other = dict(drawn_frames_exclude=[dict(peer="joiner", first_tick=5940, last_tick=6120, reason="the stall")])
            self.assertEqual({row["id"]: row for row in peers.video.capture_evidence_items(other, dict(name="run0"), peer)}["drawn-frames-host"]["probe"], "fail")


class PacedRunTest(unittest.TestCase):
    """A run whose saves are the stress lever judges its players' frame rate in the run it names at a real interval."""

    def rows(self, paced_probe="pass", paced=True):
        stress = dict(id="drawn-frames-host", run="died", peer="host", probe="fail", drawn_frames=dict(status="FAIL"),
                      finding=dict(**{"class": "harness"}, reason="4 of 6 windows uneven", launch=None, errors=[]))
        rows = [stress, dict(id="recording-rate-host", run="died", peer="host", probe="pass")]
        if paced:
            judged = dict(id="drawn-frames-host", run="died-paced", peer="host", probe=paced_probe, drawn_frames=dict(status="PASS" if paced_probe == "pass" else "FAIL"))
            if paced_probe != "pass":
                judged["finding"] = dict(**{"class": "harness"}, reason="1 with an interval over 4", launch=None, errors=[])
            rows.append(judged)
        definition = dict(runs=[dict(name="died", pacing_judged_in=dict(run="died-paced", reason="stress")), dict(name="died-paced")])
        return {(row["id"], row["run"]): row for row in peers.video.judge_pacing_where_named(definition, rows)}

    def test_the_stress_runs_frame_rate_takes_the_paced_runs_verdict(self):
        rows = self.rows()
        stress = rows[("drawn-frames-host", "died")]
        self.assertEqual(stress["probe"], "pass")
        self.assertNotIn("finding", stress)
        self.assertEqual(stress["at_stress"]["finding"]["reason"], "4 of 6 windows uneven")
        self.assertEqual(stress["judged_in"]["run"], "died-paced")

    def test_a_paced_run_whose_frame_rate_fails_fails_the_stress_row_too(self):
        stress = self.rows(paced_probe="fail")[("drawn-frames-host", "died")]
        self.assertEqual(stress["probe"], "fail")
        self.assertEqual(stress["finding"]["reason"], "died-paced: 1 with an interval over 4")

    def test_a_paced_run_that_never_drew_fails_the_stress_row(self):
        stress = self.rows(paced=False)[("drawn-frames-host", "died")]
        self.assertEqual(stress["probe"], "fail")
        self.assertIn("died-paced", stress["finding"]["reason"])

    def test_only_the_frame_rate_row_moves(self):
        self.assertEqual(self.rows()[("recording-rate-host", "died")], dict(id="recording-rate-host", run="died", peer="host", probe="pass"))


class LostSoldierTest(unittest.TestCase):
    def test_a_soldier_lost_before_its_player_takes_it_is_bought_again(self):
        # 1 of 5 turns on 10-14: the fight killed a bought soldier during its player's held turn and the run failed on it.
        for name in ("fight15-s11", "fight15-s12", "fight15-s13"):
            document = peers.scene(name)
            for script, text in document["scripts"].items():
                if not script.endswith(".probe"):
                    continue
                steps = json.loads(text)["steps"]
                selects = [(index, step) for index, step in enumerate(steps) if step.get("op") == "actor_next_until" and step["preset"] != "Base.rte/Brain Case"]
                self.assertTrue(selects, (name, script))
                for index, step in selects:
                    start = steps[index - step["lost_rebuy_back"]]
                    # Back to the press that opens the pie for the purchase, a bounded number of times.
                    self.assertEqual((start["op"], start.get("button"), start.get("down")), ("game_mouse", "right", True), (name, script, index))
                    self.assertEqual(steps[index - step["lost_rebuy_back"] + 2], {"op": "wait_scene", "input_player": 0, "equals": {"pie_visible": True}})
                    self.assertEqual(step["lost_rebuy_attempts"], 3)


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
        # The collector works in real paths, as capture_scene resolves its --out; a Mac reaches its temporary folder through a link.
        self.root = Path(self.temporary.name).resolve()

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
            dict(name="Node", os="win32", hostname="Test-Node7", ssh="node-ts", repo="C:/tree", scratch="C:/mx", python="python"),
            dict(name="Mac", os="darwin", hostname="Test-Mac", ssh="mac", repo="/mac/tree", scratch="/mac/mx", python="/usr/bin/python3",
                 binary="build-gcc/CortexCommand"),
            dict(name="Linux", os="linux", hostname="ubuntu", ssh="unix-ts", repo="/linux/tree", scratch="/linux/mx",
                 python="/usr/bin/python3", binary="build-gcc/CortexCommand")]}))

    def tearDown(self):
        self.temporary.cleanup()

    def test_mac_and_linux_joiners(self):
        for joiner in ("Mac", "Linux"):
            self.assertEqual(peers.selected_boxes("host=Node,joiner=" + joiner, self.boxes)["joiner"]["name"], joiner)

    def test_only_a_windows_host(self):
        with self.assertRaisesRegex(ValueError, "needs a Windows host"):
            peers.selected_boxes("host=Linux,joiner=Mac", self.boxes)
        with self.assertRaisesRegex(ValueError, "needs a Windows host"):
            peers.selected_boxes("host=Mac,joiner=Node", self.boxes)

    def test_linux_player_script(self):
        box = peers.selected_boxes("host=Node,joiner=Linux", self.boxes)["joiner"]
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
        box = dict(peers.selected_boxes("host=Node,joiner=Linux", self.boxes)["joiner"], ssh="local")
        self.assertEqual(peers.remote_command(box, ["/usr/bin/python3", "tool.py", "--pack", Path("/linux/mx/request.json")]),
                         ["/usr/bin/python3", "tool.py", "--pack", "/linux/mx/request.json"])
        self.assertEqual(peers.remote_command(dict(box, ssh="unix-ts"), ["python3", "a b"])[-2:], ["unix-ts", "python3 'a b'"])
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
        request.write_text(json.dumps(dict(out=str(out), scene_out=str(out), role="joiner", run_name="run0", signal_files=[])))
        self.assertIsNone(peers.read_player_files(request)["pace"])
        (out / "run0" / "joiner" / peers.video.PACE_REPORT).write_text(json.dumps({"line": "[fight-pace] RED"}))
        self.assertEqual(peers.read_player_files(request)["pace"]["line"], "[fight-pace] RED")


class WindowsJoinersTest(unittest.TestCase):
    """Four players on four Windows boxes: every joiner plays through its own box's game task, each on its own engine host."""

    BOXES = [("TEST-PC", "TEST-PC", "test-pc"), ("Node", "Test-Node7", "node-ts"), ("BOX-C", "BOX-C", "box-c-ts"), ("PAD", "Test-Tablet", "pad-ts")]

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
        boxes = peers.selected_boxes("host=TEST-PC,joiner1=Node,joiner2=BOX-C,joiner3=PAD", self.boxes)
        self.assertEqual(list(boxes), ["host", "joiner1", "joiner2", "joiner3"])
        self.assertEqual([box["name"] for box in boxes.values()], ["TEST-PC", "Node", "BOX-C", "PAD"])
        request = self.root / "request.json"
        request.write_text(json.dumps(dict(out="C:/node-ts/mx/s11-run0-1")))
        with mock.patch.object(peers, "ssh"), mock.patch.object(peers, "copy_file"), \
                mock.patch.object(peers.subprocess, "Popen") as popen:
            peers.launch(boxes["joiner1"], request, None)
        command = popen.call_args.args[0]
        self.assertEqual(command[-2:], ["node-ts", "pwsh -NoProfile -File C:/node-ts/tree/tools/e2e/peer_box.ps1 -Request C:/node-ts/mx/s11-run0-1/request.json"])
        self.assertFalse(peers.headed_box(boxes["joiner2"]))

    def test_each_player_needs_its_own_engine_host(self):
        with self.assertRaisesRegex(ValueError, "a different box for each player"):
            peers.selected_boxes("host=TEST-PC,joiner1=Node,joiner2=Node", self.boxes)
        with self.assertRaisesRegex(ValueError, "a different box for each player"):
            peers.selected_boxes("joiner1=Node,joiner2=BOX-C", self.boxes)
        self.write_boxes([*self.BOXES[:3], ("PAD", "BOX-C", "pad-ts")])
        with self.assertRaisesRegex(ValueError, "two assignments name one engine host"):
            peers.selected_boxes("host=TEST-PC,joiner1=Node,joiner2=BOX-C,joiner3=PAD", self.boxes)

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
            request.write_text(json.dumps(dict(out=str(out), scene_out=str(out), role=role, run_name="run0", signal_files=files)))
            requests[role] = request
        (self.root / "native-joiner1/run0/joiner1-stage/probe/joined.mark").write_text("")
        states = {role: peers.read_player_files(requests[role])["files"] for role in roles}
        for role in roles:
            incoming = {path: data for other, state in states.items() if other != role for path, data in state.items()}
            peers.write_player_files(requests[role], {"files": incoming})
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
        request.write_text(json.dumps(dict(out=str(out), scene_out=str(out), role="joiner2", run_index=0, scene_relative="tools/e2e/fight15-s11.json",
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
                                           out=str(self.root / "native-joiner2" / "run0.runner"), scene_out=str(self.root / "native-joiner2"),
                                           commit="tip", scene_relative="tools/e2e/fight15-s11.json",
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
        boxes = peers.selected_boxes("host=TEST-PC,joiner1=Node,joiner2=BOX-C,joiner3=PAD", self.boxes)
        roots = {"TEST-PC": "C:/sample-project/mx/scene-peers-desync", "Node": "C:/sample-project/mx/two-box-harness",
                 "BOX-C": "W:/mx/two-box-harness", "PAD": "C:/sample-project/mx/scene-peers-desync"}
        # The PAD's joiner of fe5db-testpc-node-mac-pad-4p1 could not read this 125-character Data file at 260 characters, when each
        # run had its own folder; one folder for the scene's runs brings that name to 255, and a name three characters longer to 261.
        longest = len("Data/Browncoats.rte/Scenes/Objects/BunkerSystems/RefineryAuthorizationConsole/RefineryAuthorizationConsolePieSliceActions.lua")
        for run in ("fe5db-testpc-node-mac-pad-4p1-ab", "fe5db-testpc-node-mac-pad-4p1", "fe5db-4p2"):
            for box in boxes.values():
                box["scratch"] = roots[box["name"]] + "/" + run
            if run != "fe5db-testpc-node-mac-pad-4p1-ab":
                peers.check_windows_paths(boxes, run, "run0", Path("."), longest)
                continue
            with self.assertRaisesRegex(ValueError, "^PAD: the joiner3's game files would reach 261 characters"):
                peers.check_windows_paths(boxes, run, "run0", Path("."), longest)
        self.assertEqual(peers.longest_data_path(Path(peers.__file__).resolve().parents[1]), longest)

    def test_a_newcomer_scene_picks_relay_only_by_hand_before_multiplayer(self):
        document = peers.scene("fight15-s23")
        self.assertNotIn("NetworkConnectionMode", document["peers"][0]["settings"])
        peers.check_scene(document)
        for change in ("drop", "late"):
            changed = copy.deepcopy(document)
            menu = changed["scripts"]["host.menu"].replace("combo_select ComboNetworkConnection Relay only\n", "")
            if change == "late":
                menu += "combo_select ComboNetworkConnection Relay only\n"
            changed["scripts"]["host.menu"] = menu
            with self.subTest(change=change), self.assertRaisesRegex(ValueError, "must select RelayOnly"):
                peers.check_scene(changed)

    def test_a_scene_names_one_host_and_lowercase_players(self):
        document = peers.scene("fight15-s11")
        for names in (("host", "host", "joiner1"), ("host",), ("host", "Joiner1"), ("host", "joiner1", "joiner1")):
            changed = copy.deepcopy(document)
            changed["peers"] = [dict(document["peers"][1], name=name) for name in names]
            with self.assertRaisesRegex(ValueError, "one host and at least one joiner"):
                peers.check_scene(changed)


class LobbyIdentityTest(unittest.TestCase):
    """A joiner joins the lobby its own run's host opened, and a run never starts on a port another lobby lists."""

    SCENES = sorted(path.stem for path in (Path(peers.__file__).parent / "e2e").glob("fight15-s[0-9][0-9].json"))
    LOBBY = "{PROBE_DIR_host}/lobby.json"

    def test_a_host_with_no_menu_names_the_lobby_it_listed(self):
        with tempfile.TemporaryDirectory() as folder:
            log, lobby = Path(folder) / "stdout.log", Path(folder) / "probe" / "lobby.json"
            log.write_text("[net-directory] registering\n", encoding="utf-8")
            peers.video.write_listed_lobby(log, lobby)
            self.assertFalse(lobby.exists())
            log.write_text("[net-directory] registered session_id=s-1 heartbeat_s=20\n", encoding="utf-8")
            peers.video.write_listed_lobby(log, lobby)
            self.assertEqual(json.loads(lobby.read_text(encoding="utf-8")), {"session_id": "s-1"})
            # A host that lists again after it went quiet is joined under its new listing.
            with log.open("a", encoding="utf-8") as text:
                text.write("[net-directory] registered session_id=s-2 heartbeat_s=20\n")
            peers.video.write_listed_lobby(log, lobby)
            self.assertEqual(json.loads(lobby.read_text(encoding="utf-8")), {"session_id": "s-2"})
        document = copy.deepcopy(peers.scene("mp-host-hang"))
        world = document["runs"][0]["peers"][0]
        peers.check_scene(document)
        world.pop("lobby_file")
        with self.assertRaisesRegex(ValueError, "the host's menu never writes it"):
            peers.check_scene(document)

    def test_every_two_box_joiner_joins_its_own_hosts_lobby(self):
        for name in self.SCENES:
            scripts = peers.scene(name)["scripts"]
            joiners = [text for text in scripts.values() if "click_row Public" in text or '"wait_public_row"' in text]
            with self.subTest(scene=name):
                self.assertTrue(joiners)
                self.assertFalse([key for key, text in scripts.items() if "GameRowPort" in text], "a joiner picks its lobby by port alone")
                for text in joiners:
                    self.assertIn("click_row PublicGameRowLobby:" + self.LOBBY, text)
                for text in (text for text in scripts.values() if "activate ButtonMultiplayerCreate" in text):
                    self.assertIn("write_lobby {PROBE_DIR}/lobby.json", text)
                for text in (text for text in scripts.values() if "wait_row PublicGameRowLobby:" in text):
                    self.assertLess(text.index("wait_file " + self.LOBBY), text.index("wait_row PublicGameRowLobby:" + self.LOBBY))

    def test_a_scene_whose_joiner_picks_a_row_by_port_alone_is_refused(self):
        document = copy.deepcopy(peers.scene("fight15-s13"))
        document["scripts"]["joiner1.menu"] = document["scripts"]["joiner1.menu"].replace("PublicGameRowLobby:" + self.LOBBY, "PublicGameRowPort{PORT}")
        with self.assertRaisesRegex(ValueError, "by port alone"):
            peers.check_scene(document)

    def test_a_run_never_starts_on_a_port_another_lobby_lists(self):
        # Another run's lobby on this run's port: its joiners could take that row for their host's.
        listing = {"sessions": [{"session_id": "0123456789abcdef", "name": "Captain", "activity": "Skirmish Defense", "listen_port": 49436}]}
        urls = []
        def fetch(url):
            urls.append(url)
            return listing
        with self.assertRaisesRegex(ValueError, r"^port 49436 is held by another lobby in directory\.broserver\.com: Captain \(Skirmish Defense, session 0123456789abcdef\)"):
            peers.require_free_port("directory.broserver.com", 49436, fetch=fetch, wait_s=0)
        self.assertTrue(urls[0].startswith("https://directory.broserver.com/v1/sessions?limit="))
        peers.require_free_port("directory.broserver.com", 49437, fetch=fetch, wait_s=0)

    def test_a_lobby_that_is_closing_is_waited_out(self):
        pages = [{"sessions": [{"session_id": "a", "name": "Captain", "listen_port": 49436}]}, {"sessions": []}]
        now = [0.0]
        def sleep(seconds):
            now[0] += seconds
        peers.require_free_port("directory.broserver.com", 49436, fetch=lambda url: pages.pop(0), wait_s=90, sleep=sleep, clock=lambda: now[0])
        self.assertEqual(pages, [])

    def test_every_page_of_the_listing_is_read(self):
        pages = {"": {"sessions": [{"session_id": "a", "listen_port": 1}], "next_cursor": "c1"},
                 "c1": {"sessions": [{"session_id": "b", "name": "Captain", "listen_port": 49436}]}}
        fetch = lambda url: pages["c1" if "cursor=c1" in url else ""]
        with self.assertRaisesRegex(ValueError, "session b"):
            peers.require_free_port("directory.broserver.com", 49436, fetch=fetch, wait_s=0)

    def test_a_held_port_stops_the_run_before_any_player_starts(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        boxes = root / "boxes.json"
        boxes.write_text(json.dumps({"boxes": [
            dict(name="TEST-PC", os="win32", hostname="TEST-PC", ssh="test-pc", repo="C:/t", scratch="C:/mx", python="python", session_dir="C:/s", task="t"),
            dict(name="Node", os="win32", hostname="Test-Node7", ssh="node-ts", repo="C:/t", scratch="C:/mx", python="python", session_dir="C:/s", task="t")]}))
        args = SimpleNamespace(scenario="fight15-s13", peer_boxes="host=TEST-PC,joiner1=Node", boxes_file=boxes, port=49436, engine_env=[],
                               out=root / "s13-held", repo=Path(peers.__file__).resolve().parents[1])
        held = ValueError("port 49436 is held by another lobby in directory.broserver.com: Captain (Skirmish Defense, session x)")
        with mock.patch.object(peers.video, "source_evidence", return_value={"dirty": False, "tip": "0" * 40}), \
                mock.patch.object(peers, "committed_scene_sha256", return_value="0" * 64), \
                mock.patch.object(peers, "require_free_port", side_effect=held) as checked, \
                mock.patch.object(peers, "launch") as launched, mock.patch("builtins.print"):
            peers.capture_scene(args)
        checked.assert_called_with("directory.broserver.com", 49436)
        launched.assert_not_called()
        capture = json.loads((root / "s13-held" / "capture.json").read_text())
        self.assertIn("is held by another lobby", capture["interrupted"])


class KillAndRestartTest(unittest.TestCase):
    """A peer is dropped, hung and restarted on its own box; only facts and its run's files cross to the collector."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name).resolve()
        self.document = peers.scene("mp-resume-from-disk-relay")

    def tearDown(self):
        self.temporary.cleanup()

    def edited(self, run_name, peer_name, **values):
        document = copy.deepcopy(self.document)
        run = next(run for run in document["runs"] if run["name"] == run_name)
        next(peer for peer in run["peers"] if peer["name"] == peer_name).update(values)
        return document

    def test_a_scene_that_kills_and_restarts_its_players_is_accepted(self):
        peers.check_scene(self.document)
        peers.check_scene(peers.scene("mp-host-hang"))

    def test_a_player_restarts_only_from_its_own_earlier_runtime(self):
        with self.assertRaisesRegex(ValueError, "its own runtime of an earlier run"):
            peers.check_scene(self.edited("resumed", "client", retain_runtime_from={"run": "died", "peer": "host"}))
        with self.assertRaisesRegex(ValueError, "an earlier player of its run only once that player has ended"):
            peers.check_scene(self.edited("resumed", "client", retain_runtime_from={"run": "resumed", "peer": "client"}))

    def test_the_host_never_waits_to_start_on_another_player(self):
        with self.assertRaisesRegex(ValueError, "the host starts first"):
            peers.check_scene(self.edited("died", "host", start_when={"peer": "client", "event": "video_mark resume-client-lobby"}))

    def test_a_gate_on_another_players_end_is_met_by_the_end_its_box_reports(self):
        document = self.edited("died", "host", kill_when={"peer": "client", "ended": True})
        peers.check_scene(document)
        wanted = peers.gate_facts(document, document["runs"][0])["client"]
        self.assertIn({"peer": "client", "ended": True}, wanted)
        request = self.request("client", gate_facts=wanted)
        self.assertNotIn(peers.fact_key({"peer": "client", "ended": True}), peers.read_player_files(request)["facts"])
        ended = self.root / "client" / "died" / "client" / peers.video.PEER_ENDED
        ended.parent.mkdir(parents=True)
        ended.write_text(json.dumps({"exit_code": 137}), encoding="utf-8")
        self.assertIn(peers.fact_key({"peer": "client", "ended": True}), peers.read_player_files(request)["facts"])

    def reconnect(self, **values):
        document = copy.deepcopy(peers.scene("mp-reconnect-relay"))
        next(peer for peer in document["runs"][0]["peers"] if peer["name"] == "rejoined").update(values)
        return document

    def test_a_player_restarted_in_its_run_plays_the_seat_of_the_player_it_restarts_from(self):
        document = self.reconnect()
        peers.check_scene(document)
        self.assertEqual(peers.player_seats(document), {"host": "host", "dropped": "dropped", "rejoined": "dropped"})
        self.assertEqual(peers.gate_facts(document, document["runs"][0])["dropped"], [{"peer": "dropped", "ended": True}])
        with self.assertRaisesRegex(ValueError, "only once that player has ended"):
            peers.check_scene(self.reconnect(start_when={"peer": "dropped", "event": "video_mark playing"}))
        reordered = self.reconnect()
        reordered["runs"][0]["peers"].reverse()
        with self.assertRaisesRegex(ValueError, "only once that player has ended"):
            peers.check_scene(reordered)

    def test_a_restarted_player_plays_on_the_box_of_the_player_it_restarts_from(self):
        boxes = self.root / "boxes.json"
        boxes.write_text(json.dumps({"boxes": [
            dict(name=name, ssh=name.lower(), os=system, hostname=name.lower() + "-host", repo="/repo", scratch="/scratch", python="python",
                 build_receipt="/receipt.json", binary="build/CortexCommand", session_dir="C:/session", task="task")
            for name, system in (("Host box", "win32"), ("Joiner box", "linux"), ("Other box", "darwin"))]}), encoding="utf-8")
        seats = peers.player_seats(self.reconnect())
        chosen = peers.selected_boxes("host=Host box,dropped=Joiner box,rejoined=Joiner box", boxes, seats=seats)
        self.assertEqual(chosen["rejoined"]["name"], "Joiner box")
        with self.assertRaisesRegex(ValueError, "plays on the box of the player it restarts from"):
            peers.selected_boxes("host=Host box,dropped=Joiner box,rejoined=Other box", boxes, seats=seats)
        with self.assertRaisesRegex(ValueError, "a different box for each player"):
            peers.selected_boxes("host=Joiner box,dropped=Joiner box,rejoined=Joiner box", boxes, seats=seats)
        with self.assertRaisesRegex(ValueError, "a different box for each player"):
            peers.selected_boxes("host=Host box,dropped=Joiner box,rejoined=Joiner box", boxes)

    def test_the_spread_check_reads_a_restarted_player_as_its_seat(self):
        records = {"host": ("Host box", "host-computer"), "dropped": ("Joiner box", "joiner-computer"),
                   "rejoined": ("Joiner box", "joiner-computer")}
        run = dict(name="reconnect", peer_boxes={role: box for role, (box, _) in records.items()}, peers=[])
        for role, (box, host) in records.items():
            root = self.root / role
            root.mkdir()
            record = dict(pid=42, exe_sha256="binary", box=box, engine_hostname=host, source_commit="tip", scene_sha256="hash")
            (root / "launch.json").write_text(json.dumps(record), encoding="utf-8")
            run["peers"].append(dict(peer=role, box=box, box_hostname=host, root=str(root), record=record))
        capture = dict(topology="spread", source={"tip": "tip"}, scene_sha256="hash", runs=[run], scenario_definition=self.reconnect())
        self.assertEqual(peers.spread_check(capture)["reasons"], [])
        alone = copy.deepcopy(capture)
        alone.pop("scenario_definition")
        self.assertIn("reconnect: peers ran on one named box", peers.spread_check(alone)["reasons"])

    def test_a_player_restarted_in_its_run_waits_for_the_end_its_box_reports(self):
        request = self.request("rejoined", run="reconnect")
        remote = peers.PlayerRun(json.loads(request.read_text()), self.root / "rejoined-started.json")
        gate = {"peer": "dropped", "ended": True}
        with self.assertRaisesRegex(ValueError, "never ended"):
            peers.await_earlier_player(remote, gate, 0.2, poll_s=0.05)
        peers.write_player_files(request, {"files": {}, "facts": {"dropped": [peers.fact_key(gate)]}})
        peers.await_earlier_player(remote, gate, 0.2, poll_s=0.05)

    def test_a_player_restarted_in_its_run_starts_from_the_runtime_its_box_kept(self):
        root = self.root / "dropped" / "reconnect"
        runtime = peers.video.retained_runtime({"run": "reconnect", "peer": "dropped"}, root, {}, [])
        self.assertEqual(runtime, (root / "dropped" / "runtime").resolve())
        local = SimpleNamespace(cwd=str(self.root / "elsewhere"))
        self.assertEqual(peers.video.retained_runtime({"run": "reconnect", "peer": "dropped"}, root, {"dropped": local}, []),
                         (self.root / "elsewhere").resolve())
        earlier = [{"name": "died", "peers": [{"peer": "dropped", "root": str(self.root / "died"), "runtime": str(self.root / "kept")}]}]
        self.assertEqual(peers.video.retained_runtime({"run": "died", "peer": "dropped"}, root, {}, earlier), (self.root / "kept").resolve())

    def test_a_restarted_player_packs_only_its_own_files_beside_its_earlier_player(self):
        request = self.request("rejoined", run="reconnect", box=dict(repo=str(Path(peers.__file__).resolve().parents[1])),
                               scene_relative="tools/e2e/mp-reconnect-relay.json", scene_out=str(self.root / "seat"))
        played = self.root / "seat" / "reconnect"
        for relative in ("dropped/stdout.log", "dropped/video/frames.jsonl", "dropped-capture.json", "dropped.ticket", "dropped_trace.json",
                         "dropped/runtime/Userdata/Settings.ini", "rejoined/stdout.log", "rejoined-stage/probe/net-ui-result.json",
                         "rejoined-capture.json", "rejoined-match.json"):
            (played / relative).parent.mkdir(parents=True, exist_ok=True)
            (played / relative).write_text("{}", encoding="utf-8")
        with mock.patch("builtins.print"):
            peers.pack(request)
        import tarfile
        with tarfile.open(Path(json.loads(request.read_text())["out"]).with_suffix(".tgz")) as archive:
            names = sorted(archive.getnames())
        self.assertEqual(names, ["reconnect/rejoined-capture.json", "reconnect/rejoined-match.json",
                                 "reconnect/rejoined-stage/probe/net-ui-result.json", "reconnect/rejoined/stdout.log"])

    def request(self, role, run="died", **values):
        runner = self.root / role / (run + ".runner")
        runner.mkdir(parents=True, exist_ok=True)
        path = runner / "request.json"
        fields = dict(out=str(runner), scene_out=str(self.root / role), role=role, run_name=run, run_index=0, signal_files=[])
        path.write_text(json.dumps({**fields, **values}), encoding="utf-8")
        return path

    def test_a_players_gate_on_the_other_box_is_met_by_the_fact_that_box_reports(self):
        run = self.document["runs"][0]
        wanted = peers.gate_facts(self.document, run)
        host_request = self.request("host", gate_facts=wanted["host"])
        client_request = self.request("client", gate_facts=wanted["client"])
        retained = {"peer": "host", "log": "^\\[autosave\\] retained tick="}
        listening = next(part for part in wanted["host"] if part.get("event"))
        waiting = peers.PlayerRun(json.loads(client_request.read_text()), self.root / "client-started.json")
        self.assertEqual(peers.read_player_files(host_request)["facts"], [])
        log = self.root / "host" / "died" / "host" / "stdout.log"
        log.parent.mkdir(parents=True)
        log.write_text("[autosave] tick=60 capture_ms=4\n[autosave] retained tick=60 keep=2 \n", encoding="utf-8")
        facts = peers.read_player_files(host_request)["facts"]
        self.assertEqual(facts, [peers.fact_key(retained)])
        self.assertFalse(waiting.peer_fact(retained))
        # The joiner's start waits before its engine starts, so the facts reach it before its run's folder exists.
        peers.write_player_files(client_request, {"files": {}, "facts": {"host": facts}})
        self.assertTrue(waiting.peer_fact(retained))
        self.assertFalse(waiting.peer_fact(listening))
        with self.assertRaises(ValueError):
            peers.write_player_files(client_request, {"facts": {"client": facts}})

    def test_a_restarted_player_finds_its_own_runtime_and_the_other_players_end(self):
        half = self.root / "host" / "died" / "host-capture.json"
        half.parent.mkdir(parents=True)
        runtime = self.root / "host" / "died" / "host" / "runtime"
        half.write_text(json.dumps({"peers": [{"peer": "host", "root": str(runtime.parent), "runtime": str(runtime),
                                               "record": {"exit_code": 137}}]}), encoding="utf-8")
        earlier = [{"name": "died", "peers": [{"peer": "host", "record": {"exit_code": 137}}, {"peer": "client", "record": {"exit_code": 137}}]}]
        runs = peers.completed_runs({"completed_runs": earlier}, self.root / "host", "host")
        self.assertEqual(peers.video.prior_peer(runs, {"run": "died", "peer": "host"})["runtime"], str(runtime))
        self.assertTrue(peers.video.cross_run_ready(runs, {"run": "died", "peers": ["host", "client"], "ended": True}))
        self.assertTrue(Path(peers.video.prior_peer(runs, {"run": "died", "peer": "host"})["runtime"]).is_relative_to(self.root / "host"))

    def test_the_save_is_read_on_the_box_that_wrote_it(self):
        runtime = self.root / "host" / "died" / "host" / "runtime"
        store = runtime / "Autosaves"
        store.mkdir(parents=True)
        for tick in (540, 600):
            (store / f"m1-{tick}.ccmanifest").write_text(f"SavedTick = {tick}\nMatchId = m1\n", encoding="utf-8")
            (store / f"m1-{tick}.ccsave").write_bytes(b"save")
        (store / "m1.admission").write_bytes(b"admission")
        (self.root / "host" / "died" / "host-capture.json").write_text(json.dumps({"peers": [{"peer": "host", "root": str(runtime.parent),
                                                                                               "runtime": str(runtime)}]}), encoding="utf-8")
        found = peers.checkpoint(self.request("host"))
        self.assertEqual(found["tokens"], {"RESUME_MATCH": "m1", "RESUME_TICK": 600})
        self.assertTrue(all(row["path"].startswith(str(runtime)) for row in found["evidence"]))

    def test_a_host_player_that_ends_before_its_engine_starts_ends_the_wait(self):
        import sys
        import time
        runner = self.root / "host" / "run0.runner"
        runner.mkdir(parents=True)
        (runner / "request.json").write_text(json.dumps({"out": str(runner), "role": "host"}), encoding="utf-8")
        log = self.root / "run0-host.log"
        log.write_text("ValueError: player checkout differs from the committed scene launcher\n", encoding="utf-8")
        player = subprocess.Popen([sys.executable, "-c", "pass"])
        player.wait()
        started = time.monotonic()
        with self.assertRaisesRegex(RuntimeError, "ended before starting its engine: ValueError: player checkout differs"):
            peers.wait_host_start({"ssh": peers.LOCAL, "python": sys.executable}, str(Path(peers.__file__)), str(runner / "request.json"),
                                  player, log, settle_s=1)
        self.assertLess(time.monotonic() - started, 30)

    def test_a_killed_players_partial_recording_is_collected_and_its_save_stays_on_its_box(self):
        request = self.request("host", box=dict(repo=str(Path(peers.__file__).resolve().parents[1])),
                               scene_relative="tools/e2e/mp-resume-from-disk-relay.json")
        died = self.root / "host" / "died"
        for relative in ("host/video/frames.jsonl", "host/video/fragment-0000.mp4", "host/stdout.log", "host-stage/probe/lobby.json",
                         "host/runtime/Autosaves/m1-600.ccsave", "client-stage/probe/marker.json", "host.ticket"):
            (died / relative).parent.mkdir(parents=True, exist_ok=True)
            (died / relative).write_text("{}", encoding="utf-8")
        (self.root / "host" / "died.runner" / "session.log").write_text("session\n", encoding="utf-8")
        with mock.patch("builtins.print"):
            peers.pack(request)
        import tarfile
        with tarfile.open(self.root / "host" / "died.tgz") as archive:
            names = sorted(archive.getnames())
        self.assertEqual(names, ["commands/died-host-session.log", "died/host-stage/probe/lobby.json", "died/host.ticket",
                                 "died/host/stdout.log", "died/host/video/fragment-0000.mp4", "died/host/video/frames.jsonl"])


if __name__ == "__main__":
    unittest.main()
