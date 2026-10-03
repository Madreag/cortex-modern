import unittest
import copy
import cross_report
from feel import report
from feel.records import retract_private_history


RANGES = [dict(session='s', match='1', history_branch='initial', source_round=1,
               first=1, last=10, peers=['a', 'b', 'c'])]


def live_row(tick, peer):
    return dict(session='s', match='1', round=1, history_branch='initial', source_round=1, tick=tick,
                instance=peer, execution='one', incarnation=0, phase='live',
                sim_gated='a' * 64, subsystems={'controller': 'b' * 64, 'sim_rng': 'c' * 64})


class HistoryAuthority(unittest.TestCase):
    def test_p08_all_away_ticks_are_not_equal(self):
        prefix = ('s', '1', 'initial', 1)
        result = report.compare_histories({peer: [] for peer in 'abc'}, RANGES, {'controller', 'sim_rng'},
                                          {(peer, prefix): (1, 10) for peer in 'abc'})
        self.assertEqual(result['equal_keys'], 0, result)
        self.assertEqual(result['not_comparable_keys'], 10)

    def test_p26_replay_only_round_needs_a_hold_from_start_receipt(self):
        live = {peer: [live_row(tick, peer) for tick in range(1, 11)] for peer in 'ab'}
        live['c'] = [dict(live_row(tick, 'c'), phase='catchup', history_branch=None) for tick in range(1, 11)]
        compared, away = cross_report.adopt_restored_histories(live, RANGES,
            {'c': ['[net-match] rejoin phase Loading -> TailReplay']})
        result = report.compare_histories(compared, RANGES, {'controller', 'sim_rng'}, away)
        self.assertFalse(result['passed'], dict(away=str(away), result=result))

    def test_one_remaining_witness_is_not_equal(self):
        peers = {name: [live_row(tick, name) for tick in range(1, 11)] for name in 'abc'}
        prefix = ('s', '1', 'initial', 1)
        result = report.compare_histories(peers, RANGES, {'controller', 'sim_rng'},
            {(peer, prefix): (1, 10) for peer in 'bc'})
        self.assertEqual(result['equal_keys'], 0, result)

    def test_unexpected_round_keys_fail(self):
        peers = {name: [live_row(tick, name) for tick in range(1, 11)] for name in 'abc'}
        peers['a'].append(dict(live_row(1, 'a'), match='foreign'))
        self.assertFalse(report.compare_histories(peers, RANGES, {'controller', 'sim_rng'})['passed'])

    def test_abandon_cannot_retract_player_visible_authority(self):
        rows = [dict(live_row(tick, 'c'), player_visible=True) for tick in range(1, 7)]
        rows.append(dict(abandon_from=5, round=1, incarnation=0, instance='c', execution='one'))
        try:
            kept = cross_report.void_abandoned(rows)
        except ValueError:
            return
        self.assertEqual([row['tick'] for row in kept if 'tick' in row], list(range(1, 7)), kept)

    def test_only_private_rows_of_the_named_incarnation_can_be_retracted(self):
        rows = [dict(live_row(5, 'c'), phase='private', player_visible=False), dict(live_row(5, 'c'), incarnation=1)]
        marker = dict(abandon_from=5, round=1, incarnation=0, instance='c', execution='one')
        kept = retract_private_history(rows + [marker])
        self.assertEqual(kept, [rows[1]])

    def test_whole_round_hold_requires_the_same_round_and_incarnation(self):
        live = {peer: [live_row(tick, peer) for tick in range(1, 11)] for peer in 'ab'}
        live['c'] = [dict(live_row(tick, 'c'), phase='catchup', history_branch=None) for tick in range(1, 11)]
        text = ('[net-lockstep] start round=1 frame=1 local_peer=3 peers=3\n'
                '[net-lockstep] hold of this seat at 1 revision=1\n[net-match] rejoin phase Loading -> TailReplay')
        for entry in (dict(text=text, incarnation=1), dict(text=text.replace('round=1', 'round=2'), incarnation=0)):
            compared, away = cross_report.adopt_restored_histories(live, RANGES, {'c': [entry]})
            self.assertFalse(report.compare_histories(compared, RANGES, {'controller', 'sim_rng'}, away)['passed'])
        compared, away = cross_report.adopt_restored_histories(live, RANGES, {'c': [dict(text=text, incarnation=0)]})
        self.assertTrue(report.compare_histories(compared, RANGES, {'controller', 'sim_rng'}, away)['passed'])
        live['c'].extend(live_row(tick, 'c') for tick in range(1, 11))
        compared, away = cross_report.adopt_restored_histories(live, RANGES, {'c': [dict(text=text, incarnation=0)]})
        self.assertFalse(report.compare_histories(compared, RANGES, {'controller', 'sim_rng'}, away)['passed'])


if __name__ == '__main__':
    unittest.main()
