"""A scene drops a peer when every part of an "all" gate is met; the resume scenes drop theirs only with checkpoints on disk.
A scene hangs a peer's whole process for a time and lets it run on."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest

import e2e_video

SCENES = Path(__file__).resolve().parent / "e2e"
RETAINED = {"peer": "host", "log": "^\\[autosave\\] retained tick="}
CLIENT_RETAINED = {"peer": "client", "log": "^\\[autosave\\] retained tick="}


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

    def test_the_resume_scenes_drop_their_peers_only_after_both_retained_a_checkpoint(self):
        for name in ("mp-resume-from-disk.json", "mp-resume-from-disk-autosave.json"):
            scene = json.loads((SCENES / name).read_text(encoding="utf-8"))
            died = next(run for run in scene["runs"] if run["name"] == "died")
            for peer in died["peers"]:
                parts = e2e_video.gate_parts(peer.get("kill_when") or {})
                self.assertTrue(RETAINED in parts and CLIENT_RETAINED in parts and {"peer": peer["name"], "sim_tick": 601} in parts,
                                f"{name} drops its {peer['name']} without waiting for both peers' checkpoints on disk")
                self.assertIsNone(e2e_video.run_preflight({"peers": died["peers"]}, {"name": "died", "peers": [
                    {key: value for key, value in node.items() if key not in ("menu_script", "probe", "input_script")} for node in died["peers"]]}, [], {}))

    def test_no_resume_scene_drops_a_peer_before_every_peer_recorded_the_drop_tick(self):
        # A peer killed first stops the other short of the tick its own drop waits for, and that drop never comes.
        for name in ("mp-resume-from-disk.json", "mp-resume-from-disk-autosave.json"):
            scene = json.loads((SCENES / name).read_text(encoding="utf-8"))
            for run in (run for run in scene["runs"] if any("kill_when" in peer for peer in run["peers"])):
                ticks = {(part["peer"], part["sim_tick"]) for peer in run["peers"] for part in e2e_video.gate_parts(peer.get("kill_when") or {}) if "sim_tick" in part}
                for peer in run["peers"]:
                    own = {(part["peer"], part["sim_tick"]) for part in e2e_video.gate_parts(peer.get("kill_when") or {}) if "sim_tick" in part}
                    missing = sorted({(other["name"], tick) for other in run["peers"] for _, tick in ticks} - own)
                    self.assertEqual(missing, [], f"{name} {run['name']} drops its {peer['name']} before these players recorded these ticks")

    def test_a_tick_gate_counts_only_frames_the_recorder_saved(self):
        # The drop item reads saved frames only; a gate met by an unsaved frame drops the peer short of its recorded tick.
        with tempfile.TemporaryDirectory() as folder:
            video = Path(folder) / "client" / "video"
            video.mkdir(parents=True)
            rows = [dict(screen="game", sim_tick=589, saved=True), dict(screen="game", sim_tick=601, saved=False)]
            (video / "frames.jsonl").write_text("".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")
            self.assertFalse(e2e_video.peer_gate_met({"peer": "client", "sim_tick": 601}, folder))
            (video / "frames.jsonl").write_text("".join(json.dumps(row) + "\n" for row in rows + [dict(screen="game", sim_tick=603, saved=True)]), encoding="utf-8")
            self.assertTrue(e2e_video.peer_gate_met({"peer": "client", "sim_tick": 601}, folder))


class HangGateTests(unittest.TestCase):
    def test_a_hang_names_how_long_the_peer_stays_stopped(self):
        hang = {"peer": "host", "event": "video_mark hang-host"}
        scenario = {"peers": [{"name": "host", "suspend_when": hang}, {"name": "client"}]}
        self.assertEqual((e2e_video.run_preflight(scenario, {"name": "run0"}, [], {}) or {}).get("class"), "harness")
        scenario["peers"][0]["suspend_s"] = 25
        self.assertIsNone(e2e_video.run_preflight(scenario, {"name": "run0"}, [], {}))
        scenario["peers"][0]["suspend_when"] = {"peer": "host"}
        self.assertEqual((e2e_video.run_preflight(scenario, {"name": "run0"}, [], {}) or {}).get("class"), "harness")

    @unittest.skipIf(sys.platform == "win32", "the POSIX runner's process group")
    def test_a_hung_peer_stops_every_thread_then_runs_on(self):
        # The engine leads its own process group under the runner; this stand-in does too, with a second thread.
        code = "import threading, time\nthreading.Thread(target=time.sleep, args=(30,), daemon=True).start()\ntime.sleep(30)\n"
        child = subprocess.Popen([sys.executable, "-c", code], start_new_session=True)
        try:
            receipts = []
            hang = threading.Thread(target=lambda: receipts.append(e2e_video.hang_peer(SimpleNamespace(engine_pid=child.pid), 1.5, threading.Event())))
            hang.start()
            time.sleep(.5)
            stopped = subprocess.run(["ps", "-o", "stat=", "-p", str(child.pid)], capture_output=True, text=True).stdout.strip()
            hang.join()
            running = subprocess.run(["ps", "-o", "stat=", "-p", str(child.pid)], capture_output=True, text=True).stdout.strip()
            self.assertTrue(stopped.startswith("T"), stopped)
            self.assertFalse(running.startswith("T"), running)
            self.assertEqual(receipts[0]["method"], "SIGSTOP")
            self.assertGreaterEqual(receipts[0]["suspended_ms"], 1500)
        finally:
            child.kill()
            child.wait()

    def test_a_stopped_peer_is_named_where_it_drew_nothing(self):
        import tempfile
        probe = Path(tempfile.mkdtemp())
        self.assertIsNone(e2e_video.stopped_by_scene({"probe_dir": str(probe)}))
        (probe / e2e_video.SUSPEND_RECEIPT).write_text(json.dumps({"seconds": 25.0, "suspended_ms": 25003,
                                                                   "last_recorded_frame": {"sim_tick": 833, "wall_ms": 1000}}))
        stopped = e2e_video.stopped_by_scene({"probe_dir": str(probe)})
        self.assertEqual((stopped["tick"], stopped["from_ms"], stopped["to_ms"]), (833, 1000, 26003))
        self.assertIn("stopped this peer's whole process for 25.0 s", stopped["reason"])
        (probe / e2e_video.SUSPEND_RECEIPT).write_text(json.dumps({"error": "ProcessLookupError", "last_recorded_frame": {"sim_tick": 1, "wall_ms": 1}}))
        self.assertIsNone(e2e_video.stopped_by_scene({"probe_dir": str(probe)}))

    def test_a_returning_player_stands_on_the_hosts_last_save_or_later(self):
        import tempfile
        folder = Path(tempfile.mkdtemp())
        host, player, probe = folder / "host", folder / "joiner", folder / "host-stage" / "probe"
        for path in (host, player, probe):
            path.mkdir(parents=True)
        (probe / e2e_video.SUSPEND_RECEIPT).write_text(json.dumps({"seconds": 25.0, "suspended_ms": 25003,
                                                                   "last_recorded_frame": {"sim_tick": 1700, "wall_ms": 1000}}))
        (host / "stdout.log").write_text("[autosave] tick=1201 capture_ms=8.2 bytes=1\n[autosave] tick=2401 capture_ms=8.1 bytes=1\n")

        ended = "[net-lockstep] host unresponsive 15000ms: the round ends at frame 1700\n"

        def back(line, pressed=True):
            (player / "stdout.log").write_text(ended + line)
            record = {"root": str(player), "probe_dir": self.probe(folder / "joiner-stage" / "probe", ("Failed", "Failed"), pressed)}
            return e2e_video.world_return_evidence(record, {"root": str(host), "probe_dir": str(probe)})

        evidence = back("[net-world] catch-up complete peer=2 at=2210 input_horizon=2210\n")
        self.assertTrue(evidence["pass"], evidence["reason"])
        self.assertEqual((evidence["last_save_before_stop"], evidence["player_back_at_tick"]), (1201, 2210))
        # A dedicated round's return catches up privately.
        self.assertEqual(back("[net-match] private catch-up complete frame=2300 in_place=0 from=1201 clock=1\n")["player_back_at_tick"], 2300)
        self.assertIn("before the host's last save", back("[net-world] catch-up complete peer=2 at=1100 input_horizon=1100\n")["reason"])
        self.assertIn("never pressed Rejoin Match", back("[net-world] catch-up complete peer=2 at=2210 input_horizon=2210\n", pressed=False)["reason"])

    def test_a_capture_that_stops_lets_its_hung_peer_run_on_at_once(self):
        calls = []
        handle = SimpleNamespace(suspend=lambda: calls.append("suspend"), resume=lambda: calls.append("resume"))
        stop = threading.Event()
        stop.set()
        receipt = e2e_video.hang_peer(handle, 60, stop)
        self.assertEqual(calls, ["suspend", "resume"])
        self.assertLess(receipt["suspended_ms"], 5000)

    @staticmethod
    def probe(folder, services, pressed=True, landing=""):
        """A player's probe result: the landing's reason waited for, the steps after it, the Rejoin Match press."""
        script = [{"op": "wait", "service": "Running"},
                  {"op": "wait", "scope": "menu", "control": "LabelMultiplayerLandingStatus", "text_contains": "The host stopped responding."},
                  *({"op": "wait", "elapsed_ms": 1200} for _ in services[1:]),
                  {"op": "menu", "command": "activate ButtonMultiplayerReconnect"}]
        steps = [{"index": 0, "observed": {"service": "Running"}}] + [{"index": 1 + index, "observed": {"service": service, "control": {"text": landing}}}
                                                                      for index, service in enumerate(services)]
        if pressed:
            steps.append({"index": len(script) - 1, "observed": {"service": "Failed"}})
        folder.mkdir(parents=True, exist_ok=True)
        (folder / "net-ui-result.json").write_text(json.dumps({"script": {"steps": script}, "steps": steps}))
        return str(folder)

    LINE = "The host stopped responding. The round has ended; the world continues from its last save when the host is back."

    def hang(self, silences, ended, menu_after_ms, receipt=None, line=True, after="", services=("Failed", "Failed"), landing=""):
        import tempfile
        folder = Path(tempfile.mkdtemp())
        player, probe = folder / "joiner", folder / "host-stage" / "probe"
        player.mkdir()
        probe.mkdir(parents=True)
        LINE = "The host stopped responding. The round has ended; the world continues from its last save when the host is back."
        (probe / e2e_video.SUSPEND_RECEIPT).write_text(json.dumps(receipt or {"suspended_ms": 25003, "method": "SIGSTOP"}))
        log = "".join(f"[net-lockstep] host silent {value}ms against 15000ms (rtt 0) heard=1 wait_start=0 next=900\n" for value in silences)
        if ended is not None:
            log += f"[net-lockstep] host unresponsive {ended}ms: the round ends at frame 900\n" + after
        (player / "stdout.log").write_text(log)
        (player / "console.log").write_text(("NETWORK: " + LINE + "\n") if line else "")
        index = [{"frame": 1, "screen": "MultiplayerScreen", "wall_ms": 0}, {"frame": 2, "screen": "game", "wall_ms": 1000},
                 {"frame": 3, "screen": "game", "wall_ms": 2000}, {"frame": 4, "screen": "MultiplayerScreen", "wall_ms": 2000 + menu_after_ms}]
        return e2e_video.host_hang_evidence({"root": str(player), "index": index, "probe_dir": self.probe(folder / "joiner-stage" / "probe", services, landing=landing)},
                                            {"probe_dir": str(probe)}, {"window_ms": 15000, "line": LINE})

    def test_a_player_back_on_the_menu_at_the_window_with_the_reason_passes(self):
        evidence = self.hang([1000, 14000], 15004, 400)
        self.assertTrue(evidence["pass"], evidence["reason"])
        self.assertEqual(evidence["return_ms"], 15404)

    def test_a_player_kept_past_the_window_fails(self):
        self.assertIn("sat through", self.hang([1000, 14000, 16000, 17000], 17020, 400)["reason"])
        self.assertIn("never ended", self.hang([1000, 14000], None, 400)["reason"])
        self.assertIn("back on the multiplayer menu 18004", self.hang([14000], 15004, 3000)["reason"])
        self.assertIn("word for word", self.hang([14000], 15004, 400, line=False)["reason"])
        self.assertTrue(self.hang([14000], 15004, 400, line=False, landing=self.LINE)["pass"])
        self.assertIn("not past", self.hang([14000], 15004, 400, receipt={"suspended_ms": 9000})["reason"])
        self.assertIn("before it asked", self.hang([14000], 15004, 400, services=("Failed", "Starting"))["reason"])
        self.assertTrue(self.hang([14000], 15004, 400, services=("Failed", "Failed", "Failed"))["pass"])


if __name__ == "__main__":
    unittest.main()
