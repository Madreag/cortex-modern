"""Fabricated violations must fail even when every unrelated receipt is green."""
from copy import deepcopy
import unittest

from acceptance_rows import judge


ROWS = ("spectator", "mod-match", "mod-refusal", "world-join", "image-sizes", "world-soak")
BOXES = ("pc", "edith", "mac", "linux")


def hashes(first=1, last=1201, cadence=1):
    return dict(first=first, last=last, expected=(last-first)//cadence+1, compared=(last-first)//cadence+1,
                missing=0, unequal=0, invalid=0, duplicates=0)


def peer(first=1, last=1201, box="pc"):
    return dict(box=box, first=first, last=last, completed=True, holds=[],
                timing=[dict(first=first, last=last, elapsed_ms=(last-first)*1000/60,
                             wait_ms=0, max_wait_ms=0, samples=last-first+1)])


def transfer():
    return dict(start_ms=1000, end_ms=12000, received_bytes=34567, archive_bytes=34567,
                labels=[dict(at_ms=t, texts=["Joining world"]) for t in (1000, 6000, 11000, 12000)])


def good(row):
    data = dict(peers={b: peer(box=b) for b in BOXES}, live=hashes(), fullstate=hashes(60, 1200, 60),
                tree_hashes={b: "a"*64 for b in BOXES}, module="VoidWanderers.rte",
                activity="Void Wanderers", installed_activity="Void Wanderers",
                transfer=transfer(), configuration=dict(seats=3, world_max_spectators=1),
                seated=["pc", "mac", "linux"], spectator="edith",
                watch=dict(first=300, last=600, image_received=True, role="Spectator", hashes=hashes(300, 600)),
                throttle=dict(start_ms=5000, end_ms=35000, sim_cost_us=500000, process="edith"),
                promotion=dict(host_authorized=True, freed_seat=2, seat=2, actor=123,
                               ticket_incarnation=2, applied_incarnation=2, applied_seat=2,
                               applied_actor=123, activation_tick=800, input_tick=801,
                               input_created_tick=801, applied_input="L_RIGHT", state="Running"),
                refusal=dict(joiner="linux", altered_box="linux", files_changed=1, bytes_changed=1,
                             before="a"*64, altered="b"*64, restored="a"*64,
                             reason="module manifest mismatch", log_text="module manifest mismatch: VoidWanderers.rte",
                             landing_text="module manifest mismatch: VoidWanderers.rte", joined=False,
                             refusal_tick=600, survivors=["pc", "edith", "mac"]),
                join=dict(peer="edith", host_tick=1200, activation_tick=1201, last_tick=1801,
                          directory="public-default", nat_to_nat=True, stun=True, route="direct"),
                offered_scenes=["Grasslands", "Desert"], build=dict(configuration="Final", sanitizer=False),
                scenes=[dict(name=n, archive_bytes=34567, received_bytes=34567, capture_count=1,
                             transfer=transfer()) for n in ("Grasslands", "Desert")],
                soak=dict(elapsed_s=3660, late_join_elapsed_s=3000, autosave_seconds=60,
                          fullstate_every=60, census_every_s=60, journal_pruning="not-landed",
                          journal=[dict(minute=m, bytes=123*m) for m in (10,30,50,60)]),
                census={"pc": [dict(uptime_ms=m*60000, process_bytes=100*1024**2,
                                    instrument_bytes=0) for m in range(62)],
                        "edith": [dict(uptime_ms=m*60000, process_bytes=100*1024**2,
                                       instrument_bytes=0) for m in range(12)]})
    if row in ("world-join", "world-soak"):
        data["live"] = hashes(1201, 1801)
        data["fullstate"] = hashes(1260, 1800, 60)
    if row == "world-soak":
        data["peers"] = {"pc": peer(1, 219601), "edith": peer(180001, 219601, "edith")}
        data["join"].update(host_tick=180000, activation_tick=180001, last_tick=219601)
        data["live"] = hashes(180001, 219601)
        data["fullstate"] = hashes(180060, 219600, 60)
    return data


class AcceptanceRows(unittest.TestCase):
    def assert_rejected(self, row, data, field):
        result = judge(row, data)
        self.assertFalse(result["passed"], f"{row}: fabricated {field} violation was accepted")
        self.assertIn(field, " ".join(result["failures"]))

    def test_valid_receipts(self):
        for row in ROWS:
            with self.subTest(row=row):
                result = judge(row, good(row))
                self.assertTrue(result["passed"], result)

    def test_spectator_running_with_wrong_actor_is_red(self):
        data = good("spectator")
        data["promotion"]["applied_actor"] += 1
        self.assert_rejected("spectator", data, "promotion.applied_actor")

    def test_four_box_mod_one_byte_mismatch_is_red(self):
        data = good("mod-match")
        data["tree_hashes"]["linux"] = "b"*64
        self.assert_rejected("mod-match", data, "tree_hashes")

    def test_mod_refusal_log_without_landing_message_is_red(self):
        data = good("mod-refusal")
        data["refusal"]["landing_text"] = "Could not connect"
        self.assert_rejected("mod-refusal", data, "refusal.landing_text")

    def test_late_join_missing_activation_hash_is_red(self):
        data = good("world-join")
        data["live"]["first"] += 1
        data["live"]["compared"] -= 1
        self.assert_rejected("world-join", data, "live")

    def test_image_table_omitted_world_scene_is_red(self):
        data = good("image-sizes")
        data["scenes"].pop()
        self.assert_rejected("image-sizes", data, "scenes")

    def test_soak_growth_hidden_by_instrument_subtraction_is_red(self):
        data = good("world-soak")
        for sample in data["census"]["pc"]:
            growth = sample["uptime_ms"] // 60000 * 9 * 1024**2
            sample["process_bytes"] += growth
            sample["instrument_bytes"] = growth
        self.assert_rejected("world-soak", data, "census.pc")

    def test_absent_receipts_are_not_green(self):
        for row in ROWS:
            with self.subTest(row=row):
                self.assertFalse(judge(row, {})["passed"])

    def test_spectator_faults_are_independent(self):
        cases = [("promotion", "applied_incarnation", 1), ("promotion", "input_created_tick", 799),
                 ("promotion", "host_authorized", False), ("throttle", "end_ms", 34999),
                 ("throttle", "process", "pc"), ("watch", "role", "Running"),
                 ("configuration", "world_max_spectators", 0)]
        for section, key, value in cases:
            with self.subTest(section=section, key=key):
                data = good("spectator")
                data[section][key] = value
                self.assert_rejected("spectator", data, section)

    def test_seated_wait_pace_and_hold_fail(self):
        for defect in ("wait", "pace", "hold", "sample", "nan"):
            with self.subTest(defect=defect):
                data = good("world-join")
                sample = data["peers"]["pc"]["timing"][0]
                if defect == "wait": sample["max_wait_ms"] = 50.001
                if defect == "pace": sample["elapsed_ms"] = 1200*1000/59.49
                if defect == "hold": data["peers"]["pc"]["holds"] = [600]
                if defect == "sample": sample["samples"] -= 1
                if defect == "nan": sample["elapsed_ms"] = float("nan")
                self.assert_rejected("world-join", data, "peers.pc")

    def test_missing_or_unequal_fullstate_fails(self):
        for defect in ("compared", "unequal", "missing", "invalid", "duplicates"):
            with self.subTest(defect=defect):
                data = good("mod-match")
                data["fullstate"][defect] += 1
                self.assert_rejected("mod-match", data, "fullstate")

    def test_late_join_topology_and_progress(self):
        for section, field, value in (("join", "host_tick", 1199), ("join", "nat_to_nat", False),
                                      ("join", "route", "relay"), ("transfer", "received_bytes", 0),
                                      ("transfer", "labels", [])):
            with self.subTest(field=field):
                data = good("world-join")
                data[section][field] = value
                self.assert_rejected("world-join", data, section)

    def test_image_bytes_are_measured_separately(self):
        data = good("image-sizes")
        data["scenes"][0]["received_bytes"] = 10000
        data["scenes"][0]["transfer"]["received_bytes"] = 10000
        result = judge("image-sizes", data)
        self.assertTrue(result["passed"], result)
        self.assertTrue(result["sizes"][0]["compresses"])
        self.assertEqual(result["sizes"][1]["ratio"], 1)

    def test_soak_missing_minute_and_journal_fail(self):
        for section in ("census", "journal"):
            with self.subTest(section=section):
                data = good("world-soak")
                if section == "census": data["census"]["pc"].pop(30)
                else: data["soak"]["journal"].pop(1)
                self.assert_rejected("world-soak", data, section)

    def test_pending_pruning_never_claims_green(self):
        result = judge("world-soak", good("world-soak"))
        self.assertTrue(result["passed"], result)
        self.assertTrue(result["journal_verdict"].startswith("RED L02"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
