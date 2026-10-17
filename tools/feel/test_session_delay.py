import unittest

from feel.session_delay import preview_obligations, summarize


class SessionDelayMeasurement(unittest.TestCase):
    def test_a_preview_cap_short_of_the_match_delay_is_red(self):
        frame = dict(_line=1, tick=600, frame=500, delay=15, preview_depth=10, actors=[{}],
                     activity_running=True, lockstep_active=True, lockstep_paused=False)
        owed, mismatches, missing = preview_obligations([frame], True)
        self.assertEqual(len(owed), 1)
        self.assertEqual(mismatches, [dict(line=1, tick=600, frame=500, delay=15, depth=10)])
        self.assertFalse(missing)

    def test_an_end_record_does_not_excuse_the_preceding_live_frame(self):
        live = dict(_line=1, tick=1199, frame=100, delay=15, preview_depth=0, actors=[{}],
                    activity_running=True, lockstep_active=True, lockstep_paused=False)
        ended = dict(live, _line=2, tick=1200, frame=101, activity_running=False)
        owed, mismatches, missing = preview_obligations([live, ended], True)
        self.assertEqual([r['tick'] for r in owed], [1199])
        self.assertEqual([r['tick'] for r in mismatches], [1199])
        self.assertFalse(missing)

    def test_missing_activity_state_is_not_an_excuse(self):
        _, _, missing = preview_obligations([dict(_line=17, actors=[{}])], True)
        self.assertEqual(missing, [17])

    def test_absent_peers_and_missing_repeats_cannot_pass(self):
        result = summarize([], 3, ['sp', '30ms', '200ms'])
        self.assertFalse(result['complete'])
        self.assertFalse(result['pass_check'])
        self.assertIsNone(result['single_player_repeat_noise'])

    def test_network_latency_outside_measured_noise_stays_red(self):
        def case(arm, value):
            peer = dict(peer='sp' if arm == 'sp' else 'host', b3_coverage_pass=True,
                        latency_ms=dict(p50=value), latency_frames=dict(p50=2),
                        b1_delay_pass=True, b2_depth_pass=True, b4_overlay_pass=True,
                        preview_ms_per_actor_tick=0.2, delay_min=3, delay_max=3)
            return dict(arm=arm, repeat=1, peers=[peer],
                        manifest=dict(complete=True, quiet=True, binary_unchanged=True))
        cases = [case('sp', value) for value in (30, 31, 32)]
        result = summarize(cases + [case('30ms', 33)], 3, ['sp', '30ms', '200ms'])
        self.assertFalse(result['rows'][-1]['b3_pass'])
        self.assertEqual(result['single_player_repeat_noise']['p50_max_ms'], 32)


if __name__ == '__main__':
    unittest.main()
