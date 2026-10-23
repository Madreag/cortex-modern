import unittest
from unittest.mock import patch

from feel.session_delay import contenders, live_resize_proof, preview_obligations, summarize


class SessionDelayMeasurement(unittest.TestCase):
    def test_posix_process_inventory_includes_engines_and_compilers(self):
        listing = ' 10 /lane/build/CortexCommand\n 11 /tool/cc1plus\n 12 /tool/ninja\n 13 /usr/bin/python3\n'
        with patch('feel.session_delay.subprocess.run') as run:
            run.return_value.stdout = listing
            self.assertEqual(contenders(), [dict(pid=10, exe='/lane/build/CortexCommand'),
                                            dict(pid=11, exe='/tool/cc1plus'), dict(pid=12, exe='/tool/ninja')])
            run.assert_called_once_with(['ps', '-axo', 'pid=,comm='], capture_output=True,
                                        text=True, timeout=60, check=True)

    def test_process_inventory_failure_cannot_claim_a_quiet_box(self):
        with patch('feel.session_delay.subprocess.run', side_effect=OSError('process list unavailable')):
            with self.assertRaises(OSError):
                contenders()

    def test_live_resize_requires_both_applied_boundaries_and_real_fault_readback(self):
        fault = '[feel-lag-step] requested_frame=600 observed_target_frame=600 per_engine_lag_ms=100 applied=1 readback=1 send_lag_ms=50 receive_lag_ms=50\n'
        first = fault + '[net-input-delay-applied] local=1 peer=1 frame=625 ticks=14\n'
        second = fault + '[net-input-delay-applied] local=2 peer=1 frame=625 ticks=14\n'
        self.assertTrue(live_resize_proof(dict(host=first, client=second), ['host', 'client'], [600, 100])['pass_check'])
        for bad in (second.replace('frame=625', 'frame=626'), second.replace('readback=1', 'readback=0'), fault):
            self.assertFalse(live_resize_proof(dict(host=first, client=bad), ['host', 'client'], [600, 100])['pass_check'])

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
                        c_duplicates_pass=True, c_own_hold_pass=True,
                        preview_ms_per_actor_tick=0.2, delay_min=3, delay_max=3)
            return dict(arm=arm, repeat=1, peers=[peer],
                        manifest=dict(complete=True, quiet=True, binary_unchanged=True))
        cases = [case('sp', value) for value in (30, 31, 32)]
        result = summarize(cases + [case('30ms', 33)], 3, ['sp', '30ms', '200ms'])
        self.assertFalse(result['rows'][-1]['b3_pass'])
        self.assertEqual(result['single_player_repeat_noise']['p50_max_ms'], 32)
        within = summarize(cases + [case('30ms', 31)], 3, ['sp', '30ms', '200ms'])
        self.assertTrue(within['rows'][-1]['b3_pass'])


if __name__ == '__main__':
    unittest.main()
