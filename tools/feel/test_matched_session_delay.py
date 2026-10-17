import unittest

from .matched_session_delay import freeze_envelope, judge_peer


def peer(team, milliseconds):
    return dict(peer='test', latency_ms={'p50': milliseconds}, latency_frames={'p50': 2}, b3_coverage_pass=True,
                input_edges=[dict(reference={'team': team}, action='AIM_VECTOR', held=True, ms=milliseconds),
                             dict(reference={'team': team}, action='L_LEFT', held=False, ms=milliseconds)])


class MatchedEnvelopeTest(unittest.TestCase):
    def setUp(self):
        cases = [dict(arm='sp', baseline_team=team, peers=[peer(team, base + repeat)])
                 for repeat in range(3) for team, base in ((0, 30), (1, 40))]
        self.envelope = freeze_envelope(cases)

    def test_action_outside_noise_fails_even_with_an_equal_aggregate(self):
        observation = peer(0, 31)
        observation['input_edges'][0]['ms'] = 50
        result = judge_peer(observation, self.envelope)
        self.assertTrue(result['aggregate_pass'])
        self.assertFalse(result['pass_check'])
        self.assertEqual([action['key'] for action in result['actions'] if not action['pass_check']], ['0:AIM_VECTOR:1'])

    def test_each_actor_uses_its_own_single_player_controls(self):
        self.assertTrue(judge_peer(peer(1, 41), self.envelope)['pass_check'])
        self.assertFalse(judge_peer(peer(1, 31), self.envelope)['pass_check'])

    def test_a_missing_action_does_not_disappear_from_the_comparison(self):
        observation = peer(0, 31)
        observation['input_edges'].pop()
        self.assertFalse(judge_peer(observation, self.envelope)['pass_check'])

    def test_freezing_rejects_a_control_from_the_wrong_actor_team(self):
        cases = [dict(arm='sp', baseline_team=0, peers=[peer(1, 31)])]
        with self.assertRaisesRegex(ValueError, 'requested actor team'):
            freeze_envelope(cases)


if __name__ == '__main__':
    unittest.main()
