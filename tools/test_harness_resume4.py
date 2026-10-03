"""Laptop launch and planning counterexamples; no engines or network probes."""
import json
import os
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch

import win32_test_runner as runner
from feel import launch_budget, harness_cost
import soak_two_peer
from test_soak_two_peer import write_peer
from test_inventory_oracle_evidence import run_split, run_stream

GIB = 1024 ** 3
REPO = Path(__file__).resolve().parents[1]


class LaptopHarness(unittest.TestCase):
    def test_h01_payload_floor_allows_the_measured_box_and_refuses_below_it(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            run = SimpleNamespace(record={}, env={'CC_RUNNER_MIN_FREE_GB': '6.5', 'CC_RUNNER_BOX_NAME': 'Z13',
                'CC_RUNNER_JOB_MEMORY_GB': '5', 'CC_RUNNER_AFFINITY_MASK': 'none', 'COMPUTERNAME': 'EROL-TABLET'}, _save=lambda: None)
            with patch.object(runner.IsolatedRun, 'start', lambda run: 'started'), \
                 patch.object(launch_budget, 'MARKER', root/'absent'), patch.object(launch_budget, 'CROSS_GUARD', root/'cross'), \
                 patch.object(launch_budget, 'free_memory_bytes', return_value=7*GIB):
                launch_budget.install_memory_guard()
                try:
                    result = runner.IsolatedRun.start(run)
                except RuntimeError as error:
                    self.fail(f'box-specific launch refused: {error}')
                self.assertEqual(result, 'started')
                self.assertEqual(run.record['launch_budget']['required_bytes'], int(6.5*GIB))
                with patch.object(launch_budget, 'free_memory_bytes', return_value=6*GIB):
                    with self.assertRaisesRegex(RuntimeError, 'Z13.*6442450944.*6979321856'):
                        runner.IsolatedRun.start(run)

    def test_h02_hostname_selects_the_box_runner_limits(self):
        with tempfile.TemporaryDirectory() as folder:
            manifest = Path(folder)/'boxes.json'
            manifest.write_text(json.dumps({'boxes': [dict(name='Z13', hostname='EROL-TABLET', min_free_gb=6.5,
                runner=dict(engine_memory_gb=5, affinity_mask='0xFFFF'))]}))
            limits = runner.box_runner_limits({'COMPUTERNAME': 'EROL-TABLET'}, manifest, physical=16*GIB, system_mask=0xFFFFF)
            self.assertEqual(limits['engine_memory_limit_bytes'], 5*GIB, limits)
            self.assertEqual(limits['affinity_mask'], '0x0000ffff')
            self.assertEqual(limits['min_free_bytes'], int(6.5*GIB))

    def boxes(self):
        common = dict(repo=str(REPO), exe='unit-never-run.exe', tools_root=str(REPO/'tools'), scratch_root='unused', streams=['S2'])
        local = run_split.Box(name='EROL-PC', kind='local', **common)
        laptop = run_split.Box(name='Z13', kind='windows-task', ssh='z13', task='unit', session_script='unused', **common)
        laptop.optional, laptop.max_engines, laptop.hostname = True, 2, 'EROL-TABLET'
        return local, laptop

    def test_h03_absent_optional_box_has_no_share_and_names_the_fallback(self):
        local, laptop = self.boxes()
        command = run_stream.Command('S2.unit', 'tools/e2e_video.py', [], case='unit')
        command.engine_count = 2
        with patch.object(run_split, 'probe_optional_boxes', return_value={'Z13': False}, create=True):
            shares, notes = run_split.plan([local, laptop], {}, {'S2': [command]}, ['S2'], {'S2': 'Z13'}, None)
        self.assertFalse(any(share.box.name == 'Z13' and share.ids for share in shares), shares)
        self.assertIn('Z13 absent: rows planned on EROL-PC', notes)

    def test_h03_three_engine_scene_is_refused_on_a_two_engine_box(self):
        local, laptop = self.boxes()
        command = run_stream.Command('S2.three', 'tools/e2e_video.py', [], case='three')
        command.engine_count = 3
        with patch.object(run_split, 'probe_optional_boxes', return_value={'Z13': True}, create=True):
            with self.assertRaisesRegex(ValueError, 'S2.three.*3.*Z13.*2'):
                run_split.plan([local, laptop], {}, {'S2': [command]}, ['S2'], {'S2': 'Z13'}, None)

    def test_h04_history_reports_the_exact_committed_terminal_tick(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for peer, peer_id in (('host', 1), ('client', 2)):
                write_peer(root, peer, peer_id, 1000/60, 8, 3901)
            result = soak_two_peer.acceptance_history(root, 3900)
            self.assertTrue(result['pass'], result['errors'])
            self.assertEqual(result['expected_ticks'], 3901, result)
            self.assertEqual(result['declared_match_ticks'], 3900)

    def test_h07_session_zero_refuses_by_name_and_writes_a_record(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            with patch.object(runner, 'open_input', side_effect=OSError(22, 'Incorrect function.')), \
                 patch.object(runner, 'create_desktop') as desktop, patch.object(runner, 'create_process') as process:
                try:
                    runner.IsolatedRun(['unit-never-run.exe', '--unit'], root, root/'run')
                except runner.StartupCheckError as error:
                    self.assertEqual(str(error), "no interactive desktop in this session: start engines through the box's session task")
                except OSError as error:
                    self.fail(f'unnamed desktop failure: {error}')
                else:
                    self.fail('session without an input desktop was accepted')
                desktop.assert_not_called(); process.assert_not_called()
            record = json.loads((root/'run/launch.json').read_text())
            self.assertEqual(record['refusal']['code'], 'no_interactive_desktop')
            self.assertEqual(record['exit_code'], 3)
            self.assertFalse(record['started'])

    def test_h09_census_instant_is_named_as_instrument_time(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'stdout.log'
            path.write_text('[mem-census] tick=3600 census_us=cow:200000,atoms:375000\n')
            result = harness_cost.reduce_costs([path])
            self.assertEqual(result['instruments']['census'].get('native_instant_max_ms'), 575, result)
            self.assertIn('census instant=575', result['reason'])
            self.assertEqual(result['subtraction_ms'], 0)
            self.assertFalse(result['passed'])


if __name__ == '__main__':
    unittest.main()
