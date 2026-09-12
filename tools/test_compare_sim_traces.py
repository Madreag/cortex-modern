import copy
import json
from pathlib import Path
import tempfile
import unittest

if __package__:
    from .compare_sim_traces import CORE, strict_compare
else:
    from compare_sim_traces import CORE, strict_compare


class TraceContracts(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.paths = [Path(self.temp.name) / name for name in ("host.json", "client.json")]
        self.trace = {"runs": [{"tick_hashes": [{"tick": tick, "total": "a" * 64, "subsystems": dict.fromkeys(CORE | {"controller"}, "b" * 64)} for tick in range(1, 4)]}]}

    def compare(self, host=None, client=None, **kwargs):
        host = host if host is not None else self.trace
        client = client if client is not None else host
        for path, trace in zip(self.paths, (host, client)):
            path.write_text(json.dumps(trace), encoding="utf-8")
        return strict_compare(*self.paths, **({"expected_ticks": 3} | kwargs))

    def test_intact_and_off_wire_control(self):
        self.assertTrue(self.compare()[0])
        client = copy.deepcopy(self.trace)
        client["runs"][0]["tick_hashes"][1]["subsystems"]["controller"] = "c" * 64
        self.assertTrue(self.compare(self.trace, client)[0])

    def test_real_divergence_is_localized(self):
        client = copy.deepcopy(self.trace)
        client["runs"][0]["tick_hashes"][1]["subsystems"]["actors"] = "c" * 64
        passed, result = self.compare(self.trace, client)
        self.assertFalse(passed)
        self.assertEqual(result["first_divergence"], 2)
        self.assertEqual(result["divergent_subsystems"], ["actors"])

    def test_missing_and_invalid_hashes_never_agree(self):
        for mode in ("no_subsystems", "no_terrain", "only_tick_terrain", "null", "short", "nonhex", "invalid_total"):
            with self.subTest(mode=mode):
                trace = copy.deepcopy(self.trace)
                for entry in trace["runs"][0]["tick_hashes"]:
                    if mode == "no_subsystems": entry.pop("subsystems")
                    elif mode == "no_terrain": entry["subsystems"].pop("terrain")
                    elif mode == "only_tick_terrain": entry["subsystems"] = {k: entry["subsystems"][k] for k in ("tick", "terrain")}
                    elif mode == "invalid_total": entry["total"] = None
                    else: entry["subsystems"] = dict.fromkeys(entry["subsystems"], {"null": None, "short": "a", "nonhex": "z" * 64}[mode])
                self.assertFalse(self.compare(trace)[0])

    def test_tick_range_is_exact(self):
        for mode in ("empty", "duplicate", "gap", "reordered", "extra", "late_start", "bool_tick"):
            with self.subTest(mode=mode):
                trace = copy.deepcopy(self.trace)
                ticks = trace["runs"][0]["tick_hashes"]
                if mode == "empty": ticks.clear()
                elif mode == "duplicate": ticks.insert(1, copy.deepcopy(ticks[0]))
                elif mode == "gap": ticks.pop(1)
                elif mode == "reordered": ticks.reverse()
                elif mode == "extra": ticks.append(copy.deepcopy(ticks[-1])); ticks[-1]["tick"] = 4
                elif mode == "late_start":
                    for tick in ticks: tick["tick"] += 1
                elif mode == "bool_tick": ticks[0]["tick"] = True
                self.assertFalse(self.compare(trace)[0])

    def test_two_run_trace_is_not_silently_truncated(self):
        trace = copy.deepcopy(self.trace)
        trace["runs"].append(copy.deepcopy(trace["runs"][0]))
        self.assertFalse(self.compare(trace)[0])

    def test_paused_schema_requires_independent_phase_record(self):
        trace = copy.deepcopy(self.trace)
        paused = trace["runs"][0]["tick_hashes"][1]
        paused["subsystems"] = {k: paused["subsystems"][k] for k in ("tick", "terrain")}
        self.assertFalse(self.compare(trace)[0])
        paused["paused"] = True
        passed, result = self.compare(trace)
        self.assertTrue(passed)
        self.assertEqual(result["paused_ticks"], 1)
        paused["paused"] = "true"
        self.assertFalse(self.compare(trace)[0])

    def test_matching_ranges_are_required_without_a_cap(self):
        client = copy.deepcopy(self.trace)
        client["runs"][0]["tick_hashes"].pop()
        self.assertFalse(self.compare(self.trace, client, expected_ticks=None)[0])

    def test_departing_peer_needs_an_explicit_complete_prefix(self):
        client = copy.deepcopy(self.trace)
        client["runs"][0]["tick_hashes"].pop()
        self.assertTrue(self.compare(self.trace, client, expected_ticks=2, prefix=True)[0])
        self.assertFalse(self.compare(self.trace, client, expected_ticks=3, prefix=True)[0])
        client["runs"][0]["tick_hashes"][1]["subsystems"]["actors"] = "c" * 64
        self.assertFalse(self.compare(self.trace, client, expected_ticks=2, prefix=True)[0])

    def test_missing_or_malformed_trace_fails(self):
        for contents in ("{}", "null", "{", '{"runs": [{}]}'):
            for path in self.paths: path.write_text(contents, encoding="utf-8")
            self.assertFalse(strict_compare(*self.paths, 3)[0])
        self.paths[0].unlink()
        self.assertFalse(strict_compare(*self.paths, 3)[0])

    def test_one_tick_pair_fails_expected_full_run(self):
        one = {"runs": [{"tick_hashes": [{"tick": 1, "total": "a" * 64, "subsystems": dict.fromkeys(CORE | {"controller"}, "b" * 64)}]}]}
        for path in self.paths:
            path.write_text(json.dumps(one), encoding="utf-8")
        passed, result = strict_compare(*self.paths, 600)
        self.assertFalse(passed)
        self.assertTrue(any("too few ticks" in reason for reason in result["reasons"]))


if __name__ == "__main__":
    unittest.main()
