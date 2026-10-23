"""A scene drops a peer when every part of an "all" gate is met; the resume scenes drop theirs only with checkpoints on disk.
A scene hangs a peer's whole process for a time and lets it run on."""
import json
from pathlib import Path
import subprocess
import sys
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

    def test_a_capture_that_stops_lets_its_hung_peer_run_on_at_once(self):
        calls = []
        handle = SimpleNamespace(suspend=lambda: calls.append("suspend"), resume=lambda: calls.append("resume"))
        stop = threading.Event()
        stop.set()
        receipt = e2e_video.hang_peer(handle, 60, stop)
        self.assertEqual(calls, ["suspend", "resume"])
        self.assertLess(receipt["suspended_ms"], 5000)


if __name__ == "__main__":
    unittest.main()
