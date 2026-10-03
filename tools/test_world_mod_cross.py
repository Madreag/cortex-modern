from copy import deepcopy
import unittest
from unittest.mock import patch

from world_mod_cross import configure_plan, flag, late_join_due


def baseline():
    names = ("erol", "edith", "mac", "linux")
    return dict(host="erol", scene="Grasslands", ticks=1201, specs=[dict(peer=n, box=n, role="host" if n == "erol" else "player",
                own=f"/scratch/{n}", root="/scratch", repo="/repo", flags=["-net-match-peers", "4", "-net-fullstate-dump", "/dump"],
                env={}, settings={}) for n in names], instances=[dict(name=n, box=n) for n in names],
                boxes=[dict(name=n) for n in names], deadlines=dict(launch_s=600))


def mods():
    return {b: dict(module="VoidWanderers.rte", tree_sha256="a"*64, algorithm="fixture", files=[dict(path="Index.ini", bytes=1, sha256="b"*64)], file_count=1, bytes=1)
            for b in ("pc", "edith", "mac", "linux")}


class Plans(unittest.TestCase):
    def test_mod_is_four_box_and_not_a_fixture_activity(self):
        old = baseline()
        result = configure_plan(old, "mod-match", mods())
        self.assertNotIn("acceptance_row", old)
        for spec in result["specs"]:
            args = spec["flags"]
            self.assertEqual(args[args.index("-net-match-service-module")+1], "VoidWanderers.rte")
            self.assertNotIn("-net-fullstate-dump", args)
            self.assertEqual(spec["ticks"], 1201)
            self.assertTrue(spec["preserve_evidence"])

    def test_mod_mismatch_stops_plan_before_launch(self):
        manifests = mods()
        manifests["linux"]["tree_sha256"] = "c"*64
        with self.assertRaisesRegex(ValueError, "differs"):
            configure_plan(baseline(), "mod-match", manifests)

    def test_refusal_allows_three_to_start_before_bad_joiner(self):
        result = configure_plan(baseline(), "mod-refusal", mods())
        self.assertEqual(result["late_join"], dict(peer="linux", host_tick=600))
        self.assertEqual(sum(bool(s.get("module_refusal")) for s in result["specs"]), 1)
        for spec in result["specs"]:
            self.assertEqual(spec["flags"][spec["flags"].index("-net-match-peers")+1], "3")

    def test_world_join_never_uses_wall_sleep_as_frame_evidence(self):
        result = configure_plan(baseline(), "world-join")
        self.assertEqual(result["late_join"], dict(peer="edith", host_tick=1200))
        with patch("world_mod_cross.latest_tick", return_value=1199):
            self.assertFalse(late_join_due(result, {}, now=100000))
        with patch("world_mod_cross.latest_tick", return_value=1200):
            self.assertTrue(late_join_due(result, {}, now=100000))

    def test_soak_is_two_boxes_and_joins_at_fifty_real_minutes(self):
        result = configure_plan(baseline(), "world-soak")
        self.assertEqual({s["peer"] for s in result["specs"]}, {"erol", "edith"})
        self.assertEqual(result["ticks"], 219601)
        clock = {"host_started": 10}
        with patch("world_mod_cross.latest_tick", return_value=180000):
            self.assertFalse(late_join_due(result, clock, now=3009))
            self.assertTrue(late_join_due(result, clock, now=3010))

    def test_flag_replacement_does_not_drop_adjacent_flag(self):
        self.assertEqual(flag(["-net-host", "-seed", "42"], "-net-host", False), ["-seed", "42"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
