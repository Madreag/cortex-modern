"""Detector coverage with synthetic records; never launches an engine or writes a run tree."""
from __future__ import annotations

import copy
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

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
