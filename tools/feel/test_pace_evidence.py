import unittest
import cross_report


class PaceEvidence(unittest.TestCase):
    def test_p02_absent_pace_is_incomplete(self):
        result = cross_report.pace_verdict({})
        self.assertFalse(result['passed'], result)

    def test_p03_slow_simulation_does_not_supply_a_missing_wall_rate(self):
        result = cross_report.pace_verdict(dict(pace=dict(sim_ms_per_tick=17.5)))
        self.assertFalse(result['passed'], result)

    def test_slow_simulation_alone_does_not_exempt_measured_slow_wall_rate(self):
        value = cross_report.pace_verdict(dict(pace=dict(sim_ms_per_tick=17.5, wall_tps=57)))
        self.assertEqual(value['status'], 'INCOMPLETE', value)

    def test_uniformly_heavy_round_keeps_control_under_relative_capacity(self):
        natives = {name: dict(pace=dict(sim_ms_per_tick=20, wall_tps=50),
                             lockstep=dict(round_id=7, local_capacity_tps=50, sim_tick_ms=1000/60)) for name in ('a', 'b')}
        relative = cross_report.round_capacity_evidence(natives)
        self.assertTrue(relative['complete'], relative)
        for name, native in natives.items():
            value = cross_report.pace_verdict(native, relative=relative, peer=name)
            self.assertTrue(value['passed'], value)
            self.assertTrue(value['gated'])
        natives['a']['pace']['wall_tps'] = 44.9
        self.assertFalse(cross_report.pace_verdict(natives['a'], relative=relative, peer='a')['passed'])

    def test_capacity_from_another_round_is_not_an_exemption(self):
        natives = {name: dict(lockstep=dict(round_id=round_id, local_capacity_tps=50, sim_tick_ms=1000/60))
                   for name, round_id in [('a', 7), ('b', 8)]}
        self.assertFalse(cross_report.round_capacity_evidence(natives)['complete'])

    def test_a_fast_peer_keeps_the_unchanged_absolute_bar(self):
        self.assertFalse(cross_report.pace_verdict(dict(pace=dict(sim_ms_per_tick=8, wall_tps=59.49)))['passed'])
        self.assertTrue(cross_report.pace_verdict(dict(pace=dict(sim_ms_per_tick=8, wall_tps=59.5)))['passed'])


if __name__ == '__main__':
    unittest.main()
