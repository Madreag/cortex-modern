import unittest
import cross_report


class HoldEvidence(unittest.TestCase):
    def test_p04_configured_host_stall_is_not_a_firing_receipt(self):
        result = cross_report.host_stall_hold(dict(peer=1, tick=950, classification='other'), '900:300')
        self.assertFalse(result.get('scheduled_recovery_id'), result)

    def test_p05_an_unnamed_late_input_is_not_a_design_cause(self):
        log = ('[net-lockstep] start round=9 frame=1 local_peer=1\n'
               '[net-lockstep] propose hold peer=4 next_frame=118 ready=0 heard_through=117 accepted_through=117 last_heard_ms=1\n'
               '[net-match] hold peer=4 frame=118 AI in control\n')
        result = cross_report.hold_causes(log)
        self.assertIsNone(result.get(('9', 4, 118)), result)

    def test_named_capacity_needs_the_published_round_comparison(self):
        def records(extra='', cause='capacity'):
            log = ('[net-lockstep] start round=9 frame=1 local_peer=1 peers=4\n' + extra +
                   f'[net-lockstep] propose hold peer=4 next_frame=118 cause={cause}\n'
                   '[net-match] hold peer=4 frame=120 AI in control\n')
            return cross_report.host_hold_evidence(log)[0]
        hold = dict(peer=4, tick=120, round=9)
        self.assertFalse(cross_report.design_hold(hold, records()))
        line = "[net-lockstep] slow machine peer 4 at frame 118: it runs 40 ticks/s against the fastest's 60; the AI takes its seat\n"
        result = cross_report.design_hold(hold, records(line))
        self.assertEqual(result['classification'], 'capacity (design)')
        for wrong in (line.replace('40 ticks/s', '59.5 ticks/s'), line.replace('frame 118', 'frame 117'), line.replace('peer 4', 'peer 3')):
            self.assertFalse(cross_report.design_hold(hold, records(wrong)))
        self.assertFalse(cross_report.design_hold(dict(hold, round=10), records(line)))
        self.assertFalse(cross_report.design_hold(hold, records(line, cause='')))

    def test_host_stall_receipt_is_bound_to_the_host_and_round(self):
        text = '[net-lockstep] start round=7 frame=1 local_peer=1 peers=4\n[net-test] live stall frame=900 ms=300\n'
        _, stalls = cross_report.host_hold_evidence(text, incarnation=2)
        held = dict(peer=1, tick=902, round=7, classification='other')
        self.assertEqual(cross_report.host_stall_hold(held, '900:300', stalls)['classification'], 'scheduled-fault')
        for bad in (dict(held, round=8), dict(held, peer=2), dict(held, tick=979)):
            self.assertFalse(cross_report.host_stall_hold(bad, '900:300', stalls))


if __name__ == '__main__':
    unittest.main()
