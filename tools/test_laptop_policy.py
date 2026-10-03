"""Boundary and payload integration checks; only Python stand-ins may be launched."""
import contextlib
import io
import json
import os
from pathlib import Path
from types import SimpleNamespace
import sys
import tempfile
import unittest
from unittest.mock import patch

import win32_test_runner as runner
from feel import launch_budget, harness_cost
from feel.test_harness_cost import complete_cost_log
import test_harness_resume4 as resume4
from test_harness_resume4 import GIB
from test_inventory_oracle_evidence import run_split, run_stream


class LaptopPolicy(unittest.TestCase):
    def test_native_cost_cannot_borrow_another_peers_partition(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            a, b = root/'host.log', root/'client.log'
            b.write_text(complete_cost_log(first=300, last=300, costs={'census': 45}))
            for prefix in ('', complete_cost_log(first=300, last=300, costs={'census': 5, 'fullstate': 20})):
                with self.subTest(prefix=bool(prefix)):
                    a.write_text(prefix+'[mem-census] tick=300 census_us=cow:20000,atoms:20000\n')
                    result = harness_cost.reduce_costs([a, b])
                    self.assertFalse(result['instrument_valid'], result)

    def test_native_census_instant_cannot_be_understated_by_individual_parts(self):
        with tempfile.TemporaryDirectory() as folder:
            log = Path(folder)/'stdout.log'
            log.write_text(complete_cost_log(first=300, last=300, costs={'census': 40}) +
                           '[mem-census] tick=300 census_us=cow:30000,atoms:30000\n')
            result = harness_cost.reduce_costs([log])
            self.assertFalse(result['instrument_valid'], result)
            self.assertEqual(result['max_frame_ms'], 60)

    def test_payload_limits_apply_without_a_manifest(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            _, box = resume4.LaptopHarness().boxes()
            box.min_free_gb = 6.5
            box.runner = dict(affinity_mask='none', engine_memory_gb=5)
            env = run_split.runner_environment(box)
            env['CC_RUNNER_BOX_MANIFEST'] = str(root/'absent-manifest.json')
            with patch.object(launch_budget, 'MARKER', root/'absent-marker'), \
                 patch.object(launch_budget, 'CROSS_GUARD', root/'absent-cross'), \
                 patch.object(launch_budget, 'free_memory_bytes', return_value=7*GIB):
                launch_budget.install_memory_guard()
                result = runner.run([sys.executable, '-c', 'pass'], root, root/'python', env=env, startup_checks=False)
            self.assertEqual(result['exit_code'], 0)
            self.assertEqual(result['runner_limits']['engine_memory_limit_bytes'], 5*GIB)
            self.assertEqual(result['launch_budget']['required_bytes'], int(6.5*GIB))
            self.assertEqual(result['runner_limits']['box'], 'Z13')
            self.assertIsNone(result['runner_limits']['affinity_mask'])

    def test_one_hostname_probe_per_optional_box(self):
        local, laptop = resume4.LaptopHarness().boxes()
        with patch.object(run_split.subprocess, 'run', return_value=SimpleNamespace(returncode=0, stdout='EROL-TABLET\n')) as probe:
            self.assertEqual(run_split.probe_optional_boxes([local, laptop]), {'Z13': True})
        self.assertEqual(probe.call_count, 1)
        self.assertEqual(probe.call_args.args[0][-2:], ['z13', 'hostname'])

    def test_automatic_split_keeps_three_engines_local_and_moves_two(self):
        local, laptop = resume4.LaptopHarness().boxes(); laptop.speed = .1
        two = run_stream.Command('S2.two', 'tools/e2e_video.py', [], engine_count=2)
        three = run_stream.Command('S2.three', 'tools/e2e_video.py', [], engine_count=3)
        with patch.object(run_split, 'probe_optional_boxes', return_value={'Z13': True}):
            shares, _ = run_split.plan([local, laptop], {'measured_minutes': {'S2': 100}}, {'S2': [two, three]}, ['S2'], {}, None)
        self.assertEqual({share.box.name: share.ids for share in shares}, {'EROL-PC': ['S2.three'], 'Z13': ['S2.two']})

    def test_only_selected_scenes_are_checked_for_the_engine_cap(self):
        local, laptop = resume4.LaptopHarness().boxes()
        two = run_stream.Command('S2.two', 'tools/e2e_video.py', [], engine_count=2)
        three = run_stream.Command('S2.three', 'tools/e2e_video.py', [], engine_count=3)
        with patch.object(run_split, 'load_manifest', return_value=([local, laptop], {})), \
             patch.object(run_split, '_SplitContext', return_value=None), \
             patch.object(run_stream, 'build_streams', return_value={'S2': [two, three]}), \
             patch.object(run_split, 'probe_optional_boxes', return_value={'Z13': True}), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(run_split.main(['--repo', local.repo, '--exe', 'a'*64, '--streams', 'S2', '--assign', 'S2=Z13',
                                            '--only', 'S2.two', '--dry-run']), 0)

    def test_nonoptional_boxes_are_never_silently_reassigned(self):
        local, laptop = resume4.LaptopHarness().boxes(); laptop.optional = False
        command = run_stream.Command('S2.two', 'tools/e2e_video.py', [], engine_count=2)
        with patch.object(run_split, 'probe_optional_boxes', return_value={'Z13': False}):
            shares, notes = run_split.plan([local, laptop], {}, {'S2': [command]}, ['S2'], {'S2': 'Z13'}, None)
        self.assertEqual(shares[0].box.name, 'Z13')
        self.assertFalse(any('absent' in note for note in notes))

    def test_floor_default_precedence_and_invalid_values(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'boxes.json'
            path.write_text(json.dumps({'boxes': [dict(name='EROL-TABLET', runner=dict(engine_memory_gb=12)),
                dict(name='Z13', hostname='EROL-TABLET', min_free_gb=6.5, runner=dict(engine_memory_gb=5))]}))
            def limits(env, manifest=path):
                return runner.box_runner_limits(env, manifest, physical=16*GIB, system_mask=0xFFFFF)
            self.assertEqual(limits({'COMPUTERNAME': 'EROL-TABLET'})['engine_memory_limit_bytes'], 5*GIB)
            self.assertEqual(limits({'COMPUTERNAME': 'EROL-TABLET', 'CC_RUNNER_MIN_FREE_GB': '4'})['min_free_bytes'], 4*GIB)
            self.assertEqual(limits({}, Path(folder)/'absent')['min_free_bytes'], 10*GIB)
            for value in ('0', '-1', 'nan', 'inf'):
                with self.subTest(value=value), self.assertRaises(ValueError):
                    limits({'CC_RUNNER_MIN_FREE_GB': value})


if __name__ == '__main__':
    unittest.main()
