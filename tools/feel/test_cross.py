"""Negative controls for multi-peer history and bounded recovery reductions."""
import copy
import tempfile
import unittest
from pathlib import Path

from feel import report, records


def sample(tick, peer='a', execution='one', **changes):
    return dict(session='s', match='m', history_branch='initial', source_round=1,
                tick=tick, peer=peer, instance=peer, execution=execution, incarnation=0,
                phase='live', sim_gated='aa', subsystems={'controller': 'bb', 'sim_rng': 'cc'}, **changes)


class CrossReducers(unittest.TestCase):
    def histories(self):
        return {p: [sample(t, p) for t in range(1, 5)] for p in ('a', 'b', 'c')}

    def compare(self, peers):
        return report.compare_histories(peers, [dict(session='s', match='m', history_branch='initial',
            source_round=1, first=1, last=4, peers=list(peers))], {'controller', 'sim_rng'})

    def test_three_peers_full_coverage(self):
        result = self.compare(self.histories())
        self.assertTrue(result['passed'])
        self.assertEqual(result['equal_keys'], 4)

    def test_missing_tail_is_unknown(self):
        peers = self.histories(); peers['c'].pop()
        result = self.compare(peers)
        self.assertFalse(result['passed'])
        self.assertEqual(result['unknown_keys'], 1)
        self.assertEqual(result['peers']['c']['missing'], 1)

    def test_duplicate_execution_is_not_more_coverage(self):
        peers = self.histories(); peers['c'].extend(copy.deepcopy(peers['c']))
        result = self.compare(peers)
        self.assertFalse(result['passed'])
        self.assertEqual(result['duplicates'], 4)

    def test_later_reexecution_never_erases_divergence(self):
        peers = self.histories()
        peers['c'][1]['subsystems']['controller'] = 'wrong'
        peers['c'].append(sample(2, 'c', execution='two'))
        result = self.compare(peers)
        self.assertFalse(result['passed'])
        self.assertEqual(result['first_difference']['section'], 'controller')

    def test_missing_subsystem_fails(self):
        peers = self.histories(); del peers['b'][1]['subsystems']['sim_rng']
        self.assertFalse(self.compare(peers)['passed'])

    def test_new_round_never_fills_old_round(self):
        peers = self.histories(); peers['b'][-1]['source_round'] = 2
        self.assertEqual(self.compare(peers)['peers']['b']['missing'], 1)

    def test_admission_and_cancel_are_not_terminal(self):
        schedule = [dict(id='r1', peer='c', incarnation=0, deadline_ms=1000,
                         outcomes=['first_controllable_input'])]
        events = [dict(id='r1', peer='c', incarnation=0, phase=p, wall_ms=t)
                  for p, t in [('loss', 0), ('queued_admission', 100), ('cancelled_reclaim', 200)]]
        result = report.reduce_recoveries(schedule, events, now_ms=1500)
        self.assertFalse(result[0]['passed'])
        self.assertTrue(result[0]['censored'])

    def test_wrong_incarnation_cannot_complete_recovery(self):
        schedule = [dict(id='r1', peer='c', incarnation=1, deadline_ms=1000,
                         outcomes=['first_controllable_input'])]
        events = [dict(id='r1', peer='c', incarnation=1, phase='loss', wall_ms=0),
                  dict(id='r1', peer='c', incarnation=0, phase='first_controllable_input', wall_ms=100)]
        self.assertFalse(report.reduce_recoveries(schedule, events, now_ms=1200)[0]['passed'])

    def test_wait_count_is_thresholded_separately(self):
        ticks = [dict(tick=t, wall_ms=t * 1000 / 60) for t in range(300, 1202)]
        result = report.reduce_net_window(ticks, [dict(tick=301, wait_ms=12), dict(tick=500, wait_ms=51)],
                                          300, 1201, 1000 / 60, missing_frame_stalls=2)
        self.assertEqual(result['steady_waits_over_50'], 1)
        self.assertEqual(result['steady_missing_frame_stalls'], 2)
        self.assertEqual(result['net_wait_ms'], 63)

    def test_incomplete_window_is_not_tps_pass(self):
        result = report.reduce_net_window([dict(tick=300, wall_ms=0), dict(tick=1201, wall_ms=15000)], [],
                                          300, 1201, 1000 / 60)
        self.assertFalse(result['complete'])
        self.assertIsNone(result['steady_wall_tps'])

    def test_memory_bounds_are_declared_and_no_subtraction(self):
        samples = [dict(elapsed_s=t, working_set=1000 + t * 100, private=500 + t * 20) for t in (0, 60, 120, 180)]
        result = report.reduce_memory(samples, warmup_s=60, slope_bytes_per_minute=1000,
                                      retained_bytes=2000, sample_seconds=60, elapsed_s=180)
        self.assertFalse(result['passed'])
        self.assertEqual(result['sizes']['working_set']['retained_bytes'], 12000)

    def test_rotated_records_preserve_all_lines(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with records.RecordWriter(root / 'events', chunk_bytes=60, total_bytes=10000) as writer:
                for t in range(20): writer.write(dict(tick=t, event='shot'))
            self.assertEqual([row['tick'] for row in records.read_records(root / 'events.index.json')], list(range(20)))


if __name__ == '__main__':
    unittest.main()
