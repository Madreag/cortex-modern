"""A fight below its pace gate says so after three slow minutes and plays on; a fight at pace never says so."""

import io
import json
from pathlib import Path
import random
import tempfile
import threading
import unittest
from contextlib import redirect_stdout

import e2e_video as video

SPEC = {"item": "twenty-minutes-host", "count": 20, "minimum_tps": 58.0}


def probe_document(paces, start_frame=526, start_ms=9000):
    """The engine's full-minute samples for the given per-minute paces, as measure_minute records them."""
    steps, frame, at = [], start_frame, start_ms
    for number, pace in enumerate(paces, 1):
        frame += round(pace * 60)
        at += 60000
        steps.append({"op": "measure_minute", "observed": {
            "at_ms": at, "lockstep_frame": frame, "service": "Running", "paused": False, "editing": False,
            "local_actor_alive": True, "minute": {"number": number, "elapsed_ms": 60000, "pace": pace}}})
        steps.append({"op": "wait", "observed": {"at_ms": at + 1500, "lockstep_frame": frame + 90}})
    return {"steps": steps}


def original_minute_evidence(document, spec):
    """The minute gate as it read before the early line existed."""
    samples = [step.get("observed", {}) for step in document.get("steps", [])
               if step.get("op") == "measure_minute" and "minute" in step.get("observed", {})]
    intervals, previous = [], None
    for number, sample in enumerate(samples, 1):
        minute = sample["minute"]
        elapsed = sample["at_ms"] - previous["at_ms"] if previous else minute["elapsed_ms"]
        pace = ((sample["lockstep_frame"] - previous["lockstep_frame"]) * 1000.0 / elapsed
                if previous and elapsed > 0 else minute["pace"])
        passed = (minute["number"] == number and minute["elapsed_ms"] >= 60000 and elapsed >= 60000
                  and minute["pace"] >= spec["minimum_tps"] and pace >= spec["minimum_tps"]
                  and sample.get("service") == "Running" and sample.get("paused") is False
                  and sample.get("editing") is False and sample.get("local_actor_alive") is True)
        intervals.append({"number": number, "elapsed_ms": elapsed, "pace": pace, "pass": passed})
        previous = sample
    return len(intervals) == spec["count"] and all(row["pass"] for row in intervals), intervals


class FightPaceTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def evidence(self, document):
        path = self.root / "net-ui-result.json"
        path.write_text(json.dumps(document), encoding="utf-8")
        return video.continuous_minute_evidence(path, SPEC)

    def test_the_scene_names_each_players_pace_gate(self):
        scenario = video.load_scenario("fight15-s10")
        for peer in ("host", "joiner"):
            specs = video.pace_specs(scenario, "run0", peer)
            self.assertEqual(specs, [{"item": "twenty-minutes-" + peer, "count": 20, "minimum_tps": 58.0}])

    def test_a_fight_at_pace_never_says_red(self):
        paces = [60.0, 59.9, 60.0, 58.0, 59.2] + [60.0] * 15
        for minutes in range(1, 21):
            self.assertIsNone(video.early_pace_verdict(probe_document(paces[:minutes]), SPEC, "fight host"))
        self.assertTrue(self.evidence(probe_document(paces))["pass"])

    def test_two_slow_minutes_fail_the_gate_but_wait_for_a_third(self):
        document = probe_document([60.0, 57.9, 60.0, 50.0])
        self.assertIsNone(video.early_pace_verdict(document, SPEC, "fight host"))
        self.assertFalse(self.evidence(probe_document([60.0, 57.9, 60.0, 50.0] + [60.0] * 16))["pass"])

    def test_the_third_slow_minute_writes_the_red_line(self):
        verdict = video.early_pace_verdict(probe_document([60.0, 57.9, 60.0, 50.0, 41.5]), SPEC, "fight15-s10/run0 host")
        self.assertEqual(verdict["line"], "[fight-pace] RED fight15-s10/run0 host: 3 of 5 measured minutes below 58 ticks a second "
                                          "(minute 2 57.9, minute 4 50.0, minute 5 41.5); the fight plays on")
        self.assertEqual([row["number"] for row in verdict["slow_minutes"]], [2, 4, 5])
        self.assertEqual(verdict["item"], "twenty-minutes-host")

    def test_a_slow_interval_between_samples_counts(self):
        document = probe_document([60.0] * 6)
        # Captures and chat between the samples stretch every interval by four seconds.
        for number, step in enumerate(document["steps"][::2]):
            step["observed"]["at_ms"] += 4000 * number
        verdict = video.early_pace_verdict(document, SPEC, "fight host")
        self.assertIsNotNone(verdict)
        self.assertTrue(all(row["minute_pace"] == 60.0 and row["pace"] < 58.0 for row in verdict["slow_minutes"]))

    def test_the_gate_reads_exactly_as_before(self):
        generator = random.Random(1012)
        for _ in range(400):
            paces = [generator.choice((60.0, 59.0, 58.0, 57.99, 45.0)) for _ in range(generator.randint(1, 21))]
            document = probe_document(paces)
            for step in document["steps"]:
                observed = step["observed"]
                if step["op"] == "measure_minute" and generator.random() < 0.1:
                    observed[generator.choice(("paused", "editing", "local_actor_alive"))] = generator.choice((True, False))
                if step["op"] == "measure_minute" and generator.random() < 0.1:
                    observed["at_ms"] += generator.choice((-3000, 2000))
            passed, intervals = original_minute_evidence(document, SPEC)
            current = self.evidence(document)
            self.assertEqual(current["pass"], passed)
            self.assertEqual([row["pass"] for row in current["intervals"]], [row["pass"] for row in intervals])
            self.assertEqual([row["pace"] for row in current["intervals"]], [row["pace"] for row in intervals])
            # Every early line names a fight its final gate also fails.
            if video.early_pace_verdict(document, SPEC, "fight host"):
                self.assertFalse(passed)

    def watch(self, paces):
        probe = self.root / "net-ui-result.json"
        report = self.root / "host" / video.PACE_REPORT
        report.parent.mkdir(exist_ok=True)
        stop = threading.Event()
        output = io.StringIO()
        result = {}
        with redirect_stdout(output):
            watcher = threading.Thread(target=lambda: result.update(verdict=video.watch_pace(probe, [SPEC], "fight15-s10/run0 host", report, stop, poll_s=0.02)))
            watcher.start()
            for minutes in range(1, len(paces) + 1):
                probe.write_text(json.dumps(probe_document(paces[:minutes])), encoding="utf-8")
                threading.Event().wait(0.1)
            probe.write_text('{"steps": [', encoding="utf-8")
            threading.Event().wait(0.1)
            stop.set()
            watcher.join(5)
        return result.get("verdict"), report, output.getvalue()

    def test_the_watcher_writes_the_line_once_while_the_fight_plays(self):
        verdict, report, printed = self.watch([60.0, 52.0, 51.0, 49.0, 48.0, 47.0])
        self.assertTrue(verdict["during_play"])
        self.assertEqual(printed.count("[fight-pace] RED"), 1)
        self.assertEqual(json.loads(report.read_text(encoding="utf-8"))["line"], verdict["line"])
        self.assertIn("3 of 4 measured minutes", verdict["line"])

    def test_the_watcher_stays_quiet_at_pace(self):
        verdict, report, printed = self.watch([60.0] * 8)
        self.assertIsNone(verdict)
        self.assertFalse(report.exists())
        self.assertEqual(printed, "")

    def test_a_gate_item_without_its_picture_still_names_the_verdict(self):
        peer = self.root / "host"
        (peer / "video").mkdir(parents=True)
        (peer / "video" / "events.jsonl").write_text("", encoding="utf-8")
        verdict = {"status": "FAIL", "errors": ["joiner: state differs at tick 3083"], "first_difference": {"tick": 3083, "components": ["particles"]}}
        record = {"root": str(peer), "probe_dir": str(self.root / "probe"), "video_dir": str(peer / "video"), "index": [],
                  "gates": {"fight-hashes": verdict}}
        _, evidence = video.item_evidence(record, {"id": "fight-hashes-host", "mark": "fight-ended-host", "gate": "fight-hashes"})
        self.assertEqual(evidence["probe"], "not-reached")
        self.assertEqual(evidence["assertions"], [verdict])
        self.assertEqual(evidence["reason"], "No recorded marker fight-ended-host; the fight-hashes gate is FAIL: "
                                             "first differing tick 3083 in particles; joiner: state differs at tick 3083")

    def test_the_review_carries_the_line(self):
        peer = self.root / "host"
        probe = self.root / "host-stage" / "probe"
        probe.mkdir(parents=True)
        peer.mkdir()
        (probe / "net-ui-result.json").write_text(json.dumps(probe_document([50.0, 50.0, 50.0])), encoding="utf-8")
        verdict = video.early_pace_verdict(probe_document([50.0, 50.0, 50.0]), SPEC, "fight15-s10/run0 host")
        (peer / video.PACE_REPORT).write_text(json.dumps(verdict), encoding="utf-8")
        record = {"root": str(peer), "probe_dir": str(probe), "video_dir": str(peer / "video"), "index": []}
        _, evidence = video.item_evidence(record, {"id": "twenty-minutes-host", "continuous_minutes": {"count": 20, "minimum_tps": 58.0}})
        self.assertEqual(evidence["probe"], "fail")
        self.assertEqual(evidence["continuous_minutes"]["early_red"]["line"], verdict["line"])


if __name__ == "__main__":
    unittest.main()
