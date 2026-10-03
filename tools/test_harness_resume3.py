"""Coverage duration and native host-capacity counterexamples; no engine launches."""
import unittest

import cross_report
from feel.test_attempt_requirements import attempt


COUNT_CATEGORIES = {'movement', 'weapons', 'buy', 'gold', 'objects'}
ROUND_START = '[net-lockstep] start round=13148901089125808271 frame=1 local_peer=1 peers=2 input_delay=4\n'
# Retained host stdout lines 1045-1047, including the explicit own-seat commit before cause= was printed.
OWN_WARNING = "[net-lockstep] this machine cannot keep up at frame 107: 36.7877 ticks/s against the others' 54.1, 0 ticks of slack; its seat goes quiet after frame 110 for the AI\n"
OWN_PROPOSAL = '[net-lockstep] propose hold peer=1 next_frame=111 played=0 first_missing_ms=24059 now=26550 own_park=1114 peer_park=70 ready=4\n'
OWN_COMMIT = "[net-match] hold peer=1 frame=111 AI in control (the host's own seat, AI of peer 2)\n"


def classified(text, *, tick=111, peer=1, round_id=13148901089125808271):
    holds, _ = cross_report.host_hold_evidence(text)
    return cross_report.design_hold(dict(peer=peer, tick=tick, round=round_id), holds)


def native_events():
    kinds = ('movement_observed', 'climb_limb_push', 'impact_damage', 'round_fired', 'reload_completed',
             'thrown_release', 'cargo_ejected', 'craft_departure', 'craft_refund', 'gold_deposited',
             'door_open_completed', 'door_close_completed', 'wound_added', 'wound_damage', 'gibbed', 'dying', 'dead')
    return [dict(type='coverage', phase='live', gameplay_tick=True, event=kind,
                 result='human_death_motion' if kind == 'dead' else 'success', amount=10) for kind in kinds]


class HarnessResume3(unittest.TestCase):
    def test_y01_short_match_is_green_without_soak_coverage(self):
        manifest, checks, peers, _, recoveries = attempt()
        manifest.update(scenario='match', acceptance_row=17, ticks=1201, faults=[])
        matrix = cross_report.coverage({name: [] for name in peers}, peers, manifest)
        checks['coverage_minima'] = all(row['status'] in ('PASS', 'NOT APPLICABLE')
                                        for row in matrix if row.get('required', True))
        result = cross_report.judge_attempt(manifest, checks, peers, matrix, recoveries)
        self.assertTrue(result['v1_passed'], result['oracles']['coverage'])
        scoped = [row for row in matrix if row['id'] in COUNT_CATEGORIES]
        self.assertEqual(len(scoped), 5)
        for row in scoped:
            self.assertFalse(row['required'], row)
            self.assertEqual(row['status'], 'NOT APPLICABLE', row)
            self.assertEqual(row['reason'], 'below the coverage window: 1201 ticks')

    def test_y02_ten_human_deaths_credit_ten_dead_events(self):
        matrix = cross_report.coverage({'host': native_events()}, {'host': {}}, dict(scenario='soak', ticks=72000))
        objects = next(row for row in matrix if row['id'] == 'objects')
        self.assertEqual(objects['peers']['host']['successes']['dead'], 10, objects)
        self.assertTrue(objects['required'])
        self.assertEqual(objects['status'], 'PASS', objects)

    def test_y03_real_host_warning_binds_next_own_seat_proposal(self):
        result = classified(ROUND_START + OWN_WARNING + OWN_PROPOSAL + OWN_COMMIT)
        self.assertEqual(result.get('classification'), 'capacity (design)', result)
        self.assertEqual(result['design_cause'], 'own_seat')
        self.assertEqual(result['capacity_evidence']['frame'], 107)

    def test_twenty_minute_soak_with_zero_movement_stays_red(self):
        manifest, checks, peers, _, recoveries = attempt()
        events = [event for event in native_events() if event['event'] != 'movement_observed']
        events.append(dict(type='coverage', phase='live', gameplay_tick=True,
                           event='terrain_removed', result='success', amount=10000))
        matrix = cross_report.coverage({name: events for name in peers}, peers, manifest)
        movement = next(row for row in matrix if row['id'] == 'movement')
        self.assertTrue(movement['required'])
        self.assertEqual(movement['status'], 'FAIL')
        self.assertFalse(cross_report.judge_attempt(manifest, checks, peers, matrix, recoveries)['v1_passed'])

    def test_coverage_window_is_duration_based_with_unchanged_minima(self):
        for scenario in ('match', 'soak', 'chaos'):
            for ticks in (1201, 12001, 35999, 36000, 72000, 144000, 216000):
                with self.subTest(scenario=scenario, ticks=ticks):
                    rows = cross_report.coverage({'host': []}, {'host': {}}, dict(scenario=scenario, ticks=ticks))
                    for row in rows:
                        if row['id'] not in COUNT_CATEGORIES:
                            continue
                        self.assertEqual(row['required'], ticks >= 36000, row)
                        self.assertEqual(row['status'], 'FAIL' if ticks >= 36000 else 'NOT APPLICABLE', row)
                        self.assertEqual(row['minimum'], {'movement': 5, 'weapons': 1, 'buy': 2, 'gold': 1, 'objects': 1}[row['id']])
                        if ticks < 36000:
                            self.assertEqual(row['reason'], f'below the coverage window: {ticks} ticks')

    def test_missing_or_invalid_duration_cannot_excuse_coverage(self):
        for ticks in (None, -1, True, '1201', 1201.5):
            with self.subTest(ticks=ticks):
                rows = cross_report.coverage({'host': []}, {'host': {}}, dict(scenario='match', ticks=ticks))
                for row in rows:
                    if row['id'] in COUNT_CATEGORIES:
                        self.assertTrue(row['required'], row)
                        self.assertEqual(row['status'], 'FAIL', row)

    def test_human_death_result_does_not_credit_other_events_or_nonlive_deaths(self):
        for changes in ({'event': 'round_fired'}, {'phase': 'private'}, {'gameplay_tick': False}, {'amount': 0}):
            with self.subTest(changes=changes):
                event = dict(type='coverage', phase='live', gameplay_tick=True,
                             event='dead', result='human_death_motion', amount=10)
                event.update(changes)
                rows = cross_report.coverage({'host': [event]}, {'host': {}}, dict(scenario='soak', ticks=72000))
                self.assertEqual(sum(value for row in rows for value in row['peers']['host']['successes'].values()), 0)

    def test_own_capacity_window_includes_sixty_but_not_sixty_one_frames(self):
        for capacity_frame in (51, 50, 112):
            with self.subTest(capacity_frame=capacity_frame):
                warning = OWN_WARNING.replace('frame 107:', f'frame {capacity_frame}:')
                result = classified(ROUND_START + warning + OWN_PROPOSAL + OWN_COMMIT)
                self.assertEqual(bool(result), capacity_frame == 51, result)

    def test_own_capacity_cannot_cross_round_or_be_reused(self):
        self.assertFalse(classified(ROUND_START + OWN_WARNING + ROUND_START.replace('13148901089125808271', '8') +
                                    OWN_PROPOSAL + OWN_COMMIT, round_id=8))
        text = ROUND_START + OWN_WARNING + OWN_PROPOSAL + OWN_COMMIT
        text += OWN_PROPOSAL.replace('111', '115') + OWN_COMMIT.replace('111', '115')
        self.assertFalse(classified(text, tick=115))

    def test_own_capacity_is_consumed_by_first_named_proposal_even_without_commit(self):
        first = OWN_PROPOSAL.rstrip() + ' cause=own_seat\n'
        text = ROUND_START + OWN_WARNING + first
        text += first.replace('111', '115') + OWN_COMMIT.replace('111', '115')
        self.assertFalse(classified(text, tick=115))

    def test_own_hold_needs_explicit_cause_or_native_own_seat_commit(self):
        self.assertFalse(classified(ROUND_START + OWN_WARNING + OWN_PROPOSAL +
                                    '[net-match] hold peer=1 frame=111 AI in control\n'))
        named = OWN_PROPOSAL.rstrip() + ' cause=own_seat\n'
        self.assertTrue(classified(ROUND_START + OWN_WARNING + named +
                                   '[net-match] hold peer=1 frame=111 AI in control\n'))

    def test_unrelated_or_late_capacity_cannot_excuse_host_hold(self):
        cases = (
            ROUND_START + OWN_PROPOSAL + OWN_COMMIT,
            ROUND_START + OWN_PROPOSAL + OWN_WARNING + OWN_COMMIT,
            ROUND_START + OWN_WARNING + OWN_PROPOSAL.replace('peer=1 ', 'peer=2 ') + OWN_COMMIT.replace('peer=1 ', 'peer=2 '),
            ROUND_START + OWN_WARNING + OWN_PROPOSAL.rstrip() + ' cause=silent\n' + OWN_COMMIT,
            ROUND_START + OWN_WARNING.replace('36.7877', '54.1') + OWN_PROPOSAL + OWN_COMMIT,
            ROUND_START + OWN_WARNING.replace('36.7877', '0') + OWN_PROPOSAL + OWN_COMMIT,
            ROUND_START + OWN_WARNING + OWN_PROPOSAL.replace('next_frame=111', 'next_frame=112') + OWN_COMMIT,
        )
        for text in cases:
            with self.subTest(text=text):
                self.assertFalse(classified(text))


if __name__ == '__main__':
    unittest.main()
