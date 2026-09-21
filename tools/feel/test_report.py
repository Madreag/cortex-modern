"""Detector coverage with synthetic records; never launches an engine or writes a run tree."""
from __future__ import annotations

import copy
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch
from types import SimpleNamespace
from contextlib import nullcontext
import stat

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from feel import report


def actor(aim=0, vx=0, left=False):
    return dict(uid=7, aim=aim, vx=vx, vy=0, x=100, y=100, render_x=100, render_y=100,
                input=dict(aim_x=1, aim_y=1, move_left=left, move_right=False), fired=False)


def frame(number, wall, pose, tick=10):
    return dict(frame=number, _line=number, tick=tick, draw_begin_ms=wall - 2, present_end_ms=wall,
                actors=[pose], cap_hz=60, interval_ms=1000 / 60, preview_depth=4, alpha=.5,
                scene_width=1000, scene_height=1000, wraps_x=False, wraps_y=False)


class ReportTests(unittest.TestCase):
    def test_compressed_records_keep_the_original_bytes_and_detector(self):
        from tempfile import TemporaryDirectory
        from feel.records import compress_case_records, open_record
        with TemporaryDirectory() as folder:
            root = Path(folder)
            dump = root / 'host_trace.json.simdump.txt'
            original = b'1 activity active\n2 activity active\n'
            dump.write_bytes(original)
            records = compress_case_records(root)
            self.assertEqual(records[0]['original_bytes'], len(original))
            with open_record(dump, 'rb') as stream:
                self.assertEqual(stream.read(), original)
            self.assertEqual(report.early_decision_tick(root, 'host'), 2)

    def test_matrix_footprint_has_its_own_limit(self):
        import feel_measure
        entry = SimpleNamespace(stat=lambda **kwargs: SimpleNamespace(st_mode=stat.S_IFREG, st_size=7_000_000_000,
                                                                      st_file_attributes=0))
        with patch.object(Path, 'exists', return_value=True), patch.object(feel_measure.os, 'scandir', return_value=nullcontext([entry])):
            self.assertEqual(feel_measure.scratch_bytes(Path('matrix'), feel_measure.MATRIX_BYTE_LIMIT), 7_000_000_000)
        with patch.object(Path, 'exists', return_value=True), patch.object(feel_measure.os, 'scandir', return_value=nullcontext([entry])):
            with self.assertRaisesRegex(RuntimeError, '5000000000 byte limit'):
                feel_measure.scratch_bytes(Path('one-case'))

    def test_live_hash_comparison_keeps_every_replayed_pass(self):
        from tempfile import TemporaryDirectory
        from feel.retained_resume import compare_live_hashes
        with TemporaryDirectory() as folder:
            host, client = Path(folder) / 'host.jsonl', Path(folder) / 'client.jsonl'
            rows = [dict(tick=tick, sim_gated=str(tick), subsystems={'controller': str(tick)}) for tick in range(1, 61)]
            host.write_text('\n'.join(map(json.dumps, rows)), encoding='utf-8')
            repeated = copy.deepcopy(rows[:40] + rows[20:])
            repeated[12]['subsystems']['controller'] = 'different'
            client.write_text('\n'.join(map(json.dumps, repeated)), encoding='utf-8')
            compared = compare_live_hashes(host, client, 1)
            self.assertEqual([row['compared_ticks'] for row in compared], [40, 40])
            self.assertEqual([row['mismatched_ticks'] for row in compared], [1, 0])
            self.assertEqual(compared[0]['first_mismatches'], [13])

    def item9a(self, *, wall_ms=15000, waits='', missing=0, complete=True, silent=False, survivor_log='', client_log='', final_tick=1200):
        from tempfile import TemporaryDirectory
        with TemporaryDirectory() as folder:
            run = Path(folder)
            (run / 'host').mkdir()
            (run / 'manifest.json').write_text(json.dumps(dict(silent_tick=600 if silent else None, ticks=final_tick)), encoding='utf-8')
            (run / 'host/stdout.log').write_text(waits, encoding='utf-8')
            if silent:
                (run / 'survivor').mkdir()
                (run / 'survivor/stdout.log').write_text(survivor_log, encoding='utf-8')
                (run / 'client').mkdir()
                (run / 'client/stdout.log').write_text(client_log, encoding='utf-8')
            (run / 'host_report.json').write_text(json.dumps({'runner': {'lockstep': {
                'next_frame': final_tick + 1, 'missing_frame_stalls': 100, 'steady_missing_frame_stalls': missing,
                'sim_tick_ms': 1000 / 60}}}), encoding='utf-8')
            rows = [dict(type='committed', tick=300, wall_ms=5000)]
            if complete:
                rows.append(dict(type='committed', tick=final_tick, wall_ms=5000 + wall_ms))
            return report.item9a_gates(run, rows=rows)

    def test_item9a_steady_rate_and_wait_boundaries(self):
        self.assertTrue(self.item9a()['pass_check'])
        self.assertEqual(self.item9a(wall_ms=16000)['pins']['item9a_wall_tps']['status'], 'MISS')
        self.assertEqual(self.item9a(waits='[net-frame-wait] frame=600 wait_ms=51')['pins']['item9a_longest_wait']['status'], 'MISS')
        waits = '\n'.join(f'[net-frame-wait] frame={tick} wait_ms=50' for tick in (600, 700, 800))
        self.assertEqual(self.item9a(waits=waits)['pins']['item9a_net_wait']['status'], 'MISS')

    def test_same_machine_tps_ruling_retains_block_limits(self):
        reference = dict(steady_wall_tps=55, evidence='single-player.jsonl')
        measured = self.item9a(wall_ms=900000 / 54)
        report.apply_tps_call(measured, reference)
        self.assertTrue(measured['pass_check'])
        self.assertEqual(measured['tps_call']['absolute']['status'], 'MISS')
        self.assertFalse(measured['pins']['item9a_confirmed_horizon_lag']['required'])
        blocked = self.item9a(wall_ms=900000 / 54, waits='[net-frame-wait] frame=600 wait_ms=51')
        report.apply_tps_call(blocked, reference)
        self.assertFalse(blocked['pass_check'])
        too_slow = self.item9a(wall_ms=900000 / 52)
        report.apply_tps_call(too_slow, reference)
        self.assertEqual(too_slow['pins']['item9a_wall_tps']['status'], 'MISS')
        fast_box = self.item9a(wall_ms=900000 / 59)
        report.apply_tps_call(fast_box, dict(steady_wall_tps=60, evidence='stock.jsonl'))
        self.assertEqual(fast_box['pins']['item9a_wall_tps']['status'], 'MISS')
        self.assertNotIn('required', fast_box['pins']['item9a_confirmed_horizon_lag'])

    def test_item9a_recovery_elapsed_time_cannot_be_reset_away(self):
        result = self.item9a(wall_ms=15700)
        self.assertEqual(result['metrics']['steady_wall_ms'], 15700)
        self.assertEqual(result['pins']['item9a_wall_tps']['status'], 'MISS')

    def test_item9a_prefetch_misses_are_diagnostic_and_end_evidence_is_required(self):
        self.assertTrue(self.item9a(missing=1)['pass_check'])
        self.assertEqual(self.item9a(missing=1)['metrics']['steady_missing_frame_stalls'], 1)
        self.assertTrue(self.item9a(missing=None)['pass_check'])
        self.assertFalse(self.item9a(complete=False)['pass_check'])

    def test_item9a_confirmed_horizon_lag_cannot_hide_behind_average_rate(self):
        self.assertEqual(self.item9a(wall_ms=15050)['pins']['item9a_confirmed_horizon_lag']['status'], 'PASS')
        result = self.item9a(wall_ms=15051)
        self.assertEqual(result['pins']['item9a_wall_tps']['status'], 'PASS')
        self.assertEqual(result['pins']['item9a_confirmed_horizon_lag']['status'], 'MISS')

    def test_item9a_extended_rejoin_window_requires_its_actual_end(self):
        result = self.item9a(final_tick=2400, wall_ms=35000)
        self.assertTrue(result['pass_check'])
        self.assertEqual(result['metrics']['last_tick'], 2400)
        self.assertFalse(self.item9a(final_tick=2400, complete=False)['pass_check'])

    def test_item9a_rejoin_requires_committed_live_reclaim_on_both_survivors(self):
        request = '[net-match] hold peer=2 frame=603 AI in control\n[net-match] rejoin: player reconnected - resyncing the match\n'
        applied = '[net-match] seat-reclaimed peer=2 frame=700 live_actors=2\n'
        def status(host, survivor, completed=""):
            return self.item9a(silent=True, waits=request + host, survivor_log=survivor, client_log=completed)['pins']['item9a_rejoin']['status']
        self.assertEqual(status('', request), 'MISS')
        self.assertEqual(status(applied, ''), 'MISS')
        self.assertEqual(status(applied, applied.replace('700', '701')), 'MISS')
        self.assertEqual(status(applied, applied), 'MISS')
        self.assertEqual(status(applied, applied, '[net-match] private catch-up complete frame=700\n'), 'PASS')
        for refused in (applied.replace('700', '500'), applied.replace('700', '1201'), applied.replace('actors=2', 'actors=0'), applied.replace('peer=2', 'peer=3')):
            self.assertEqual(status(refused, refused), 'MISS')

    def test_item9a_private_rejoin_rejects_survivor_reload(self):
        hold = '[net-match] hold peer=2 frame=603 AI in control\n'
        applied = '[net-match] seat-reclaimed peer=2 frame=700 live_actors=2\n'
        client = '[net-match] private catch-up complete frame=700\n'
        def result(extra):
            return self.item9a(silent=True, waits=hold + applied + extra, survivor_log=applied, client_log=client)['pins']['item9a_private_rejoin']['status']
        self.assertEqual(result(''), 'PASS')
        self.assertEqual(result('[net-match] rejoin: player reconnected - resyncing the match\n'), 'MISS')

    def test_latency_uses_present_return_and_counts_frames(self):
        edge = dict(_line=1, tick=10, wall_ms=100, actor=actor(), last_presented_frame=1,
                    changes=[dict(action='AIM_VECTOR', held=True, x=1, y=1)])
        frames = [frame(1, 95, actor()), frame(2, 115, actor()), frame(3, 140, actor(aim=-.5))]
        value = report.input_latencies([edge], frames)[0]
        self.assertEqual((value['ms'], value['frames']), (40, 2))
        self.assertFalse(value['pass_check'])

    def test_pose_change_without_requested_controller_state_does_not_reflect(self):
        edge = dict(_line=1, tick=10, wall_ms=100, actor=actor(), last_presented_frame=1,
                    changes=[dict(action='L_LEFT', held=True)])
        frames = [frame(1, 95, actor()), frame(2, 110, actor(vx=-2))]
        value = report.input_latencies([edge], frames)[0]
        self.assertIsNone(value['ms'])
        self.assertFalse(value['pass_check'])

    def test_a_later_edge_cannot_supply_the_missing_reflection(self):
        first = dict(_line=1, tick=10, wall_ms=100, actor=actor(), last_presented_frame=1,
                     changes=[dict(action='L_LEFT', held=True)])
        later = copy.deepcopy(first)
        later.update(_line=2, tick=20, wall_ms=200)
        later['changes'][0]['held'] = False
        frames = [frame(1, 95, actor()), frame(2, 220, actor(vx=-1, left=True))]
        self.assertIsNone(report.input_latencies([first, later], frames)[0]['ms'])

    def test_draw_percentile_is_nearest_rank_and_boundary_is_not_relaxed(self):
        values = report.distribution([1] * 99 + [51])
        self.assertEqual((values['p99'], values['max']), (1, 51))
        self.assertEqual(report.pin(51, '<= 50', 51 <= 50, [])['status'], 'MISS')

    def test_unknown_value_never_passes(self):
        self.assertEqual(report.pin(None, '<= 8', True, [])['status'], 'MISS')

    def test_correction_uses_matching_target_and_strict_four_pixel_boundary(self):
        forecast = dict(_line=3, committed_tick=9, target_tick=10, actor=dict(uid=7, x=100, y=100))
        committed = [dict(tick=10, wall_ms=170, scene_width=1000, scene_height=1000, wraps_x=False, wraps_y=False)]
        canonical = {(10, 7): (dict(pos=[104, 100]), 8)}
        with patch.object(report, 'canonical_positions', return_value=canonical), \
                patch.object(report, 'remote_commands', return_value=({10: [dict(command='AIOrder')]}, True)):
            values, missing, _ = report.corrections([forecast], committed, Path('canonical'), Path('commands'), 1)
        self.assertFalse(missing)
        self.assertEqual(values[0]['displacement_px'], 4)
        self.assertFalse(values[0]['over_4_px'])
        self.assertFalse(values[0]['remote_cause_proven'])

    def test_every_nonzero_warp_residual_is_retained(self):
        first, second = actor(), actor()
        second['render_x'] += .001
        values = report.warp_records([frame(1, 100, first), frame(2, 120, second)])
        self.assertEqual(len(values), 1)
        self.assertGreater(values[0]['residual_px'], 0)
        self.assertFalse(values[0]['over_4_px'])

    def test_correction_rate_checks_sliding_windows(self):
        self.assertEqual(report.max_in_window([0, 9_999, 20_000], 10_000), 2)
        self.assertEqual(report.max_in_window([0, 10_000, 20_000], 10_000), 1)

    def test_incomplete_canonical_dump_names_the_decided_tick(self):
        from tempfile import TemporaryDirectory
        with TemporaryDirectory() as folder:
            dump = Path(folder) / 'sp_trace.json.simdump.txt'
            dump.write_text('1 activity running\n356 activity over\n', encoding='utf-8')
            with self.assertRaises(report.EarlyDecision) as raised:
                report.canonical_positions(dump, set())
            self.assertEqual(raised.exception.tick, 356)
            self.assertEqual(raised.exception.fail_line,
                             'FAIL: decided at tick 356; measurement window is 1200 ticks')

    def test_early_decision_tick_reads_killall_from_the_run_log(self):
        from tempfile import TemporaryDirectory
        with TemporaryDirectory() as folder:
            run = Path(folder)
            (run / 'sp').mkdir()
            (run / 'sp' / 'stdout.log').write_text(
                '[gib-cause] killall sparing team 0 at tick 356\n'
                '[scenario] FeelBaseline passed=no ticks=356\n', encoding='utf-8')
            self.assertEqual(report.early_decision_tick(run, 'sp'), 356)

    def test_canonical_voice_repeat_one_tick_later_is_detected(self):
        press = dict(_line=1, tick=10, wall_ms=100, actor=dict(uid=7), changes=[dict(action='FIRE', held=True)])
        shot = dict(_line=4, committed_tick=10, target_tick=14, wall_lower_ms=101, wall_upper_ms=110,
                    actor=dict(uid=7, gun_uid=8, fired=True))
        log = ('[preview-event] voice committed=10 tick=14 uid=8 previewed=1 asset=0 preset=2 seq=0 preset_name="gun" path=fire.flac\n'
               '[preview-event] voice committed=15 tick=15 uid=8 previewed=1 asset=42 preset=2 seq=0 preset_name="gun" path=fire.flac\n')
        with patch.object(Path, 'read_text', return_value=log):
            result = report.firing_records([press], [shot], [], Path('stdout'))[0]
        self.assertEqual(result['audio_ms_upper'], 10)
        self.assertFalse(result['once'])


if __name__ == '__main__':
    unittest.main()
