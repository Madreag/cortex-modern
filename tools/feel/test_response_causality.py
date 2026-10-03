import unittest
from feel import report
from feel.test_report import actor, frame


def delayed_response(early_carry=False):
    edge = dict(_line=1, tick=10, wall_ms=100, delay=3, actor=actor(), last_presented_frame=1,
                changes=[dict(action='L_LEFT', held=True)])
    timeline = [dict(type='committed', tick=12, wall_ms=90, actors=[actor()])] + [
        dict(type='committed', tick=13 + n, wall_ms=150 + 17 * n, actors=[actor(vx=-1 - n)]) for n in range(4)]
    late = [frame(1, 95, actor(), tick=8)]
    if early_carry:
        late += [frame(2, 115, actor(left=True), tick=9), frame(3, 132, actor(left=True), tick=10)]
    late += [frame(len(late) + n + 1, 150 + 17 * n, actor(vx=-1 - n, left=True), tick=9 + n) for n in range(6)]
    hit = dict(type='interaction', tick=11, uid=7, fields='vel,angvel', source='world-object')
    return edge, timeline, late, hit


class ResponseCausality(unittest.TestCase):
    def test_p11_generic_world_hit_does_not_prove_a_delayed_horizontal_response(self):
        edge, timeline, late, hit = delayed_response()
        result = report.previewed_responses([edge], late, timeline, [hit])[0]
        self.assertFalse(result['pass_check'], result)

    def test_unresolved_interaction_is_counted(self):
        edge, timeline, late, hit = delayed_response()
        rows = report.previewed_responses([edge], late, timeline, [hit])
        value, passed = report.response_verdict(rows)
        self.assertFalse(passed)
        self.assertEqual(value['unjudged_required'], ['L_LEFT+@10'])

    def test_first_step_component_comparison_can_prove_the_truth_was_delayed(self):
        edge, timeline, late, hit = delayed_response()
        timeline.insert(0, dict(type='committed', tick=11, wall_ms=80, actors=[actor()]))
        evidence = report.interaction_evidence([hit],
            '[preview-fidelity] tick=11 step=1 from=10 uid=7 differing=1 first: vel=-0x1p+0,0x0p+0 | vel=0x0p+0,0x0p+0')
        row = report.previewed_responses([edge], late, timeline, evidence)[0]
        self.assertTrue(row['pass_check'], row)
        self.assertEqual(row['interaction']['tick'], 11)

    def test_vertical_difference_preserves_the_original_deadline(self):
        edge, timeline, late, hit = delayed_response()
        timeline.insert(0, dict(type='committed', tick=11, wall_ms=80, actors=[dict(actor(), vy=3)]))
        evidence = report.interaction_evidence([hit],
            '[preview-fidelity] tick=11 step=1 from=10 uid=7 differing=1 first: vel=0,0 | vel=0,3')
        row = report.previewed_responses([edge], late, timeline, evidence)[0]
        self.assertTrue(row['judged'], row)
        self.assertFalse(row['pass_check'], row)
        self.assertAlmostEqual(row['budget_ms'], 33.6666666667)

    def test_interaction_after_committed_response_cannot_move_the_deadline(self):
        edge, timeline, late, hit = delayed_response()
        hit['tick'] = 14
        hit['fidelity'] = dict(pairs={'vx': [-1, 0]})
        row = report.previewed_responses([edge], late, timeline, [hit])[0]
        self.assertNotIn('interaction', row)
        self.assertFalse(row['pass_check'], row)

    def test_ambiguous_component_pairs_are_not_causal_evidence(self):
        edge, timeline, late, hit = delayed_response()
        timeline.insert(0, dict(type='committed', tick=11, wall_ms=80, actors=[actor()]))
        evidence = report.interaction_evidence([hit],
            '[preview-fidelity] tick=11 step=1 from=10 uid=7 differing=2 first: vel=-1,0 | vel=0,0 ;; vel=5,2 | vel=9,2')
        row = report.previewed_responses([edge], late, timeline, evidence)[0]
        self.assertFalse(row['pass_check'], row)
        self.assertFalse(row['judged'], row)

    def test_p19_carried_input_does_not_excuse_a_vertical_only_hit(self):
        edge, timeline, late, hit = delayed_response(early_carry=True)
        hit.update(before=dict(vx=0, vy=0), after=dict(vx=0, vy=3))
        carried = report.input_latencies([edge], late)[0]
        self.assertTrue(carried['carried_pass'], carried)
        result = report.previewed_responses([edge], late, timeline, [hit])[0]
        self.assertFalse(result['pass_check'], result)


if __name__ == '__main__':
    unittest.main()
