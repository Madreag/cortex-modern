"""Negative controls for multi-peer history and bounded recovery reductions."""
import copy
import tempfile
import unittest
import zipfile
from pathlib import Path

from feel import report, records


def sample(tick, peer='a', execution='one', **changes):
    return dict(session='s', match='m', history_branch='initial', source_round=1,
                tick=tick, peer=peer, instance=peer, execution=execution, incarnation=0,
                phase='live', sim_gated='a'*64, subsystems={'controller': 'b'*64, 'sim_rng': 'c'*64}, **changes)


class CrossReducers(unittest.TestCase):
    def test_recovery_observation_bounds_do_not_mix_engine_clock_origins(self):
        schedule=[dict(id='stall',peer='a',incarnation=0,return_incarnation=0,deadline_ms=240,outcomes=['first_controllable_input'])]
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'recovery.jsonl'
            ledger=records.RecoveryLedger(path,'a','payload:1',100,schedule)
            ledger.observe([dict(type='fault_begin',id='stall',fault_started_wall_ms=1e12),
                            dict(type='fault',id='stall',applied=True)],0,100,120)
            ledger.observe([dict(type='recovery',id='stall',terminal=True,recovery_phase='first_controllable_input',recovery_wall_ms=8)],0,300,350)
            rows=[__import__('json').loads(line) for line in path.read_text().splitlines()]
            result=report.reduce_recoveries(schedule,rows,now_ms=350)[0]
            self.assertEqual(result['duration_ms'],250)
            self.assertEqual(result['duration_lower_ms'],180)
            self.assertFalse(result['passed'])
            schedule[0]['deadline_ms']=260
            self.assertTrue(report.reduce_recoveries(schedule,rows,now_ms=350)[0]['passed'])

    def test_restart_recovery_keeps_identity_and_cannot_credit_an_unapplied_fault(self):
        schedule=[dict(id='crash',peer='a',incarnation=0,return_incarnation=1,deadline_ms=1000,outcomes=['first_controllable_input'])]
        with tempfile.TemporaryDirectory() as temporary:
            ledger=records.RecoveryLedger(Path(temporary)/'recovery.jsonl','a','payload:2',0,schedule)
            ledger.observe([dict(type='fault_begin',id='crash')],0,10,20)
            self.assertFalse(ledger.restart_inputs(1))
            ledger.external_start(schedule[0],0,30,40,dict(tick=100))
            restart=ledger.restart_inputs(1)
            self.assertIn('crash',restart)
            self.assertEqual(restart['crash']['engine_after_wall_ms'],0)
            self.assertTrue(restart['crash']['effect_finished'])
    def test_the_soaks_announced_leave_ends_told_it_left_and_the_soak_plays_on(self):
        import sys
        from types import SimpleNamespace
        sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
        import cross_peers, cross_report
        peers = [dict(name='erol', box='EROL-PC'), dict(name='edith', box='EDITH'), dict(name='mac', box='Mac')]
        boxes = {'EROL-PC': dict(kind='windows-local'), 'EDITH': dict(kind='windows-task'), 'Mac': dict(kind='posix-ssh')}
        options = SimpleNamespace(schedule=None, scenario='soak', host='erol', recovery_deadline_ms=120000, ticks=72000, chaos_seed=1, chaos_faults=0)
        faults = cross_peers.schedule_for(options, peers, boxes)
        leave = next(f for f in faults if f['action'] == 'announced-leave-rejoin')
        # The ruled outcome: a clean leaver's return reads that it left, and nothing waits on a catch-up it will not have.
        self.assertEqual((leave['outcomes'], leave['incarnation'], leave['return_incarnation']), (['told_it_left'], 0, 1))
        self.assertFalse([f for f in faults if f.get('recovery_id') == leave['id']])
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'recovery.jsonl'
            ledger = records.RecoveryLedger(path, 'mac', 'payload:3', 0, [leave])
            ledger.external_start(leave, 0, 100, 110, dict(tick=leave['tick']))
            told = dict(type='recovery', id=leave['id'], recovery_phase='told_it_left', terminal=True,
                        text='You left this match. It is still running. It takes no new players until it ends.')
            ledger.observe([told], 1, 4000, 4100)
            self.assertIn(leave['id'], ledger.completed)
            rows = [__import__('json').loads(line) for line in path.read_text().splitlines()]
            result = report.reduce_recoveries([leave], rows, now_ms=5000)[0]
            self.assertEqual((result['outcome'], result['passed']), ('told_it_left', True))
            # The refused return exits 1 by design; its own row excuses that exit, and the old text would not.
            refused = dict(started=True, exit_code=1, timed_out=False)
            self.assertTrue(cross_report.judge_exit(refused, 'mac', 1, faults, rows)['passed'])
            self.assertFalse(cross_report.judge_exit(refused, 'mac', 1, faults, [dict(r, phase='catch_up') for r in rows])['passed'])
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
        peers['c'][1]['subsystems']['controller'] = 'd'*64
        peers['c'].append(sample(2, 'c', execution='two'))
        result = self.compare(peers)
        self.assertFalse(result['passed'])
        self.assertEqual(result['first_difference']['section'], 'controller')

    def test_missing_subsystem_fails(self):
        peers = self.histories(); del peers['b'][1]['subsystems']['sim_rng']
        self.assertFalse(self.compare(peers)['passed'])

    def test_equal_missing_hash_values_cannot_pass(self):
        peers=self.histories()
        for values in peers.values(): values[0]['subsystems']['controller']=None
        result=self.compare(peers)
        self.assertFalse(result['passed']); self.assertEqual(result['invalid_count'],3)

    def test_new_round_never_fills_old_round(self):
        peers = self.histories(); peers['b'][-1]['source_round'] = 2
        self.assertEqual(self.compare(peers)['peers']['b']['missing'], 1)

    def test_soak_ranges_require_native_boundaries_and_do_not_infer_missing_tails(self):
        first=sample(1); second=sample(1); second.update(match='m2',source_round=2)
        ranges,missing=report.declared_history_ranges([first,second],[],['a','b','c'],final_tick=7)
        self.assertEqual(len(missing),1); self.assertEqual(ranges[0]['last'],7)
        ranges,missing=report.declared_history_ranges([first,second],[dict(first,final_tick=20)],['a','b','c'],final_tick=7)
        self.assertFalse(missing); self.assertEqual([r['last'] for r in ranges],[20,7])

    def test_admission_and_cancel_are_not_terminal(self):
        schedule = [dict(id='r1', peer='c', incarnation=0, deadline_ms=1000,
                         outcomes=['first_controllable_input'])]
        events = [dict(id='r1', peer='c', incarnation=0, phase=p, wall_ms=t)
                  for p, t in [('loss', 0), ('queued_admission', 100), ('cancelled_reclaim', 200)]]
        result = report.reduce_recoveries(schedule, events, now_ms=1500)
        self.assertFalse(result[0]['passed'])
        self.assertTrue(result[0]['censored'])

    def test_fault_duration_is_distinct_from_measured_recovery(self):
        schedule=[dict(id='stall',peer='c',incarnation=0,duration_ms=600,deadline_ms=2000,
                       outcomes=['first_controllable_input'])]
        events=[dict(id='stall',peer='c',incarnation=0,phase=phase,wall_ms=stamp)
                for phase,stamp in [('fault_applied',100),('first_controllable_input',1300)]]
        result=report.reduce_recoveries(schedule,events,now_ms=1500)[0]
        self.assertTrue(result['passed'])
        self.assertEqual(result['duration_ms'],1200)
        self.assertEqual(result['scheduled_duration_ms'],600)
        missing=report.reduce_recoveries(schedule,[],now_ms=1500)[0]
        self.assertFalse(missing['passed']); self.assertIsNone(missing['duration_ms'])

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

    def test_sealed_record_is_verified_before_raw_retirement(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary); path=root/'events.jsonl'
            original=b'{"tick":1,"event":"shot"}\r\n'*50
            path.write_bytes(original)
            receipt=records.compress_closed_record(path,root)
            self.assertFalse(path.exists())
            self.assertEqual(receipt['original_bytes'],len(original))
            with records.open_record(path,'rb') as source: self.assertEqual(source.read(),original)

    def test_checkpoint_file_identity_and_manifest_are_both_required(self):
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'match-77.ccsave'
            descriptor='RestoreSchema = 1\nMatchId = match\nSavedTick = 77\nSessionId = 1\nRoundId = 9\n'
            with zipfile.ZipFile(path,'w') as archive:
                archive.writestr('Restore.ini',descriptor)
                archive.writestr('Save.ini','SimUpdateCount = 77\nRuntimeGlobals = blob\nLuaStateGraph = graph\n')
                for name in ('Index.ini','Save Mat.png','Save FG.png','Save BG.png'): archive.writestr(name,b'fixture')
            self.assertFalse(records.inspect_checkpoint(path)['passed'])
            manifest=path.with_suffix('.ccmanifest')
            manifest.write_text(descriptor+'ManifestSchema = 3\nConfigHash = hash\nConfigPayload = 00\n')
            result=records.inspect_checkpoint(path)
            self.assertTrue(result['passed']); self.assertIn('not a restore',result['scope'])
            manifest.write_text(manifest.read_text().replace('SavedTick = 77','SavedTick = 78'))
            self.assertFalse(records.inspect_checkpoint(path)['passed'])


if __name__ == '__main__':
    unittest.main()
