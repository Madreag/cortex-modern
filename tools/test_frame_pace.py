"""The drawn-frames rule: every judged 60-tick window draws 95 % of the expected rate with interval p95 within 1.25 frame
periods, no window excused; while the recorder runs the expected rate is the recording's. A scene's item says the same."""

from pathlib import Path
import unittest

import frame_pace


def line(tick, fps, p95_ms, recorder=True, recorder_hz=30, target_hz=60.0):
    return (f"[frame-pace] mode=lockstep recorder={int(recorder)} recorder_hz={recorder_hz} target_hz={target_hz:.2f} first_tick={tick - 60} "
            f"tick={tick} frames={round(fps)} wall_ms=1000.0 fps={fps:.2f} interval_p50_ms=16.67 interval_p95_ms={p95_ms:.2f} interval_max_ms={p95_ms + 1:.2f}")


def log(windows, **options):
    """A round's frame lines: (fps, p95) per 60-tick window."""
    return "\n".join(line(60 * (index + 1), fps, p95, **options) for index, (fps, p95) in enumerate(windows)) + "\n"


EVEN = (59.8, 17.5)


class EvennessRule(unittest.TestCase):
    def judged(self, middle, **options):
        # Three start windows and one end window around the windows under test, all even.
        return frame_pace.check(log([EVEN] * 3 + middle + [EVEN], **options), 'lockstep')

    def test_a_window_at_the_floor_passes_and_one_under_it_fails(self):
        self.assertTrue(self.judged([EVEN, (28.5, 20.0), EVEN])['passed'])
        failed = self.judged([EVEN, (28.4, 20.0), EVEN])
        self.assertFalse(failed['passed'])
        self.assertEqual(failed['slow_windows'], 1)

    def test_the_p95_band_is_a_frame_period_and_a_quarter(self):
        self.assertTrue(self.judged([EVEN, (59.0, 41.6), EVEN])['passed'])
        failed = self.judged([EVEN, (59.0, 41.8), EVEN])
        self.assertFalse(failed['passed'])
        self.assertEqual(failed['uneven_windows'], 1)
        self.assertEqual(failed['p95_band_ms'], 41.67)

    def test_one_uneven_window_in_twenty_minutes_fails(self):
        windows = [EVEN] * 1200
        windows[600] = (59.0, 63.2)
        failed = self.judged(windows)
        self.assertFalse(failed['passed'])
        self.assertEqual((failed['windows'], failed['uneven_windows'], failed['first_uneven']['tick']), (1200, 1, 60 * (3 + 600 + 1)))

    def test_the_recording_rate_is_the_target_while_the_recorder_runs(self):
        recording = self.judged([EVEN, (40.0, 30.0), EVEN])
        self.assertTrue(recording['passed'])
        self.assertEqual((recording['target_hz'], recording['fps_floor']), (30.0, 28.5))
        presenting = self.judged([EVEN, (40.0, 30.0), EVEN], recorder=False, recorder_hz=0)
        self.assertFalse(presenting['passed'])
        self.assertEqual((presenting['target_hz'], presenting['fps_floor'], presenting['p95_band_ms']), (60.0, 57.0, 20.83))

    def test_the_rounds_first_windows_and_its_last_are_not_judged(self):
        loading = [(9.0, 150.0)] * frame_pace.SKIP + [EVEN] * 5 + [(9.0, 150.0)]
        self.assertTrue(frame_pace.check(log(loading), 'lockstep')['passed'])
        late = [EVEN] * frame_pace.SKIP + [(9.0, 150.0)] + [EVEN] * 5
        self.assertFalse(frame_pace.check(log(late), 'lockstep')['passed'])

    def test_a_scenes_drawn_frames_item_states_the_rule_the_check_judges(self):
        rule = getattr(frame_pace, 'RULE', '')
        for part in ('60-tick window', f'first {frame_pace.SKIP}', 'its last', f'{frame_pace.FPS_SHARE:.0%}', "the recording's",
                     f'{frame_pace.P95_PERIODS} frame periods', 'no window may miss'):
            self.assertIn(part, rule)
        source = (Path(__file__).resolve().parent / 'e2e_video.py').read_text(encoding='utf-8')
        start = source.index("f'drawn-frames-{name}'")
        self.assertIn('frame_pace.RULE', source[start:start + 200])


if __name__ == '__main__':
    unittest.main()
