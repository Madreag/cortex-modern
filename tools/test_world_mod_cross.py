from copy import deepcopy
import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from world_mod_cross import configure_plan, flag, late_join_due, stage_activity, restore_activity, prepare_mod_runtime, retain_native_screens, named_row, is_row, check_driver_preflights, check_mod_preflights


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
    def test_numbered_acceptance_rows_keep_the_standard_driver(self):
        self.assertFalse(named_row(['--acceptance-row', '17']))
        self.assertFalse(named_row(['--acceptance-row=18']))
        self.assertTrue(named_row(['--acceptance-row=mod-match']))
        self.assertTrue(named_row(['--acceptance-row', 'world-join']))
        for row in (17, 18, 19):
            plan = dict(acceptance_row=row)
            self.assertFalse(is_row(plan))
            check_driver_preflights(plan, {})
            check_mod_preflights(plan, {})

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
        self.assertEqual({s["peer"] for s in result["specs"]}, {"erol", "edith-first", "edith"})
        self.assertEqual({s["box"] for s in result["specs"]}, {"erol", "edith"})
        self.assertEqual(next(s for s in result['specs'] if s['peer']=='edith')['session_leaf'], 'session-late.json')
        self.assertFalse(next(s for s in result['specs'] if s['peer']=='edith-first').get('defer_until_session', False))
        self.assertEqual(result["ticks"], 219601)
        clock = {"host_started": 10}
        with patch("world_mod_cross.latest_tick", return_value=180000):
            self.assertFalse(late_join_due(result, clock, now=3009))
            self.assertTrue(late_join_due(result, clock, now=3010))

    def test_flag_replacement_does_not_drop_adjacent_flag(self):
        self.assertEqual(flag(["-net-host", "-seed", "42"], "-net-host", False), ["-seed", "42"])

    def refusal_fixture(self):
        from acceptance_mod import manifest
        retained = os.environ.get('CC_ACCEPTANCE_TEST_ROOT')
        if retained:
            root = Path(tempfile.mkdtemp(prefix='refusal-stage-', dir=retained))
        else:
            temporary = tempfile.TemporaryDirectory()
            self.addCleanup(temporary.cleanup)
            root = Path(temporary.name)
        source = root/'repo/Data/VoidWanderers.rte'
        source.mkdir(parents=True)
        (source/'Index.ini').write_bytes(b'DataModule\n')
        before = manifest(source)
        own = root/'run/linux/incarnation-0'
        (own/'engine').mkdir(parents=True)
        args = ['engine', '-net-match-service-e2e', '-net-join-session', 'session-id']
        spec = dict(own=str(own), root=str(root/'run'), repo=str(root/'repo'), flags=args[:],
                    env={'CC_TEST_NET_UI_SCRIPT':'probe'}, port_block=[49320,49324], acceptance_row='mod-refusal',
                    module_refusal=True, module_tree_sha256=before['tree_sha256'])
        runtime = prepare_mod_runtime(spec)
        run = SimpleNamespace(cwd=runtime, argv=args, env={'CC_TEST_NET_UI_SCRIPT':'probe'},
                              record={'argv':args, 'env_set':{'CC_TEST_NET_UI_SCRIPT':'probe'}})
        return source, before, own, runtime, run, spec

    def test_refusal_uses_real_menu_and_restores_the_copy(self):
        from acceptance_mod import manifest
        source, before, own, runtime, run, spec = self.refusal_fixture()
        with patch('feel_measure.private_settings'):
            stage_activity(run, spec)
        self.assertNotIn('-net-match-service-e2e', run.argv)
        self.assertNotIn('-net-join-session', run.argv)
        self.assertIn('-menu-script', run.argv)
        self.assertIs(run.argv, run.record['argv'])
        self.assertNotIn('CC_TEST_NET_UI_SCRIPT', run.record['env_set'])
        self.assertIn('assert_substate Landing', (own/'refusal.menu.txt').read_text())
        restore_activity(spec)
        self.assertEqual(manifest(runtime/'Data/VoidWanderers.rte'), before)
        self.assertFalse((runtime/'Mods/VoidWanderers.rte').exists())
        self.assertEqual(manifest(source), before)

    def test_failed_menu_staging_restores_its_one_byte_mutation(self):
        from acceptance_mod import manifest
        source, before, own, runtime, run, spec = self.refusal_fixture()
        with patch('feel_measure.private_settings'), patch('world_mod_cross.stage_refusal_menu', side_effect=OSError('fixture staging failure')):
            with self.assertRaisesRegex(OSError, 'fixture staging failure'):
                stage_activity(run, spec)
        self.assertEqual(manifest(runtime/'Data/VoidWanderers.rte'), before)
        self.assertEqual(manifest(source), before)
        self.assertTrue((own/'mutation.json.restored.json').is_file())

    def test_native_landing_dump_survives_excluded_runtime(self):
        import json
        from acceptance_cross_report import native_labels
        source, before, own, runtime, run, spec = self.refusal_fixture()
        document = dict(screen='MultiplayerScreen', controls=[dict(name='LabelMultiplayerLandingStatus', visible=True,
                        text='module content hash does not match: VoidWanderers.rte')])
        path = runtime/'ScreenShots/dump_host_options_0.json'
        path.write_text(json.dumps(document), encoding='utf-8')
        retain_native_screens(spec, run)
        self.assertIn('module content hash does not match', native_labels(own))
        (own/'native-screens'/path.name).write_text('{}', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'differs'):
            native_labels(own)


if __name__ == "__main__":
    unittest.main(verbosity=2)
