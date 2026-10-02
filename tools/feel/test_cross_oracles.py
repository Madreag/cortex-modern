import copy
import unittest
import cross_report


class AttemptOracles(unittest.TestCase):
    def fixture(self):
        manifest=dict(scenario='match',ticks=1201,faults=[],fullstate_every=600,capture_rows_pending=[1,2])
        checks=dict.fromkeys(cross_report.CORE_CHECKS,True)
        checks.update(shared_fullstate=False,no_engine_findings=False,quiet_feel=True,bounded_recovery=True)
        peers={name:dict(feel_gated=False,feel_status='REPORTED; quiet window not scheduled',
                        memory_by_incarnation={'0':dict(passed=False,sizes={},missing_samples=0)}) for name in ('a','b','c')}
        return manifest,checks,peers,[dict(status='NOT COVERED')],[]

    def test_assigned_capture_rows_stay_red_beside_core_pass(self):
        summary=cross_report.judge_attempt(*self.fixture())
        self.assertTrue(summary['core_passed'])
        self.assertFalse(summary['gate_b_eligible'])
        self.assertEqual(summary['oracles']['full_state']['status'],'FAIL')
        self.assertIn('LuaMan.cpp:1613',summary['oracles']['full_state']['reason'])
        self.assertIn('FloatText.h:324',summary['oracles']['full_state']['reason'])
        self.assertEqual(summary['oracles']['memory']['status'],'NOT COVERED')
        self.assertEqual(summary['oracles']['recovery']['status'],'NOT APPLICABLE')

    def test_each_core_failure_and_nonstandard_budget_blocks_b(self):
        for name in cross_report.CORE_CHECKS:
            args=copy.deepcopy(self.fixture()); args[1][name]=False
            self.assertFalse(cross_report.judge_attempt(*args)['core_passed'],name)
        args=copy.deepcopy(self.fixture()); args[0]['ticks']=600
        self.assertFalse(cross_report.judge_attempt(*args)['gate_b_eligible'])

    def test_other_oracles_are_judged_even_with_assigned_capture_failure(self):
        args=copy.deepcopy(self.fixture())
        args[0].update(scenario='soak',faults=[dict(id='leave')])
        args[1]['bounded_recovery']=False
        args[2]['b']['memory_by_incarnation']['0']=dict(passed=False,sizes={'private':{}},missing_samples=0)
        args[2]['a'].update(feel_gated=True,feel_status='FAIL')
        args[1]['quiet_feel']=False
        summary=cross_report.judge_attempt(*args)
        for name in ('recovery','memory','feel','full_state','engine_findings'):
            self.assertEqual(summary['oracles'][name]['status'],'FAIL',name)
        self.assertEqual(summary['oracles']['coverage']['status'],'NOT COVERED')

    def test_disabled_capture_cannot_qualify_for_gate_b(self):
        args=copy.deepcopy(self.fixture()); args[0]['fullstate_every']=0
        self.assertFalse(cross_report.judge_attempt(*args)['gate_b_eligible'])

    def test_capture_hold_engine_red_keeps_missing_history_red(self):
        args=copy.deepcopy(self.fixture()); args[0]['capture_rows_pending']=[]
        args[1].update(shared_fullstate=True,only_capture_induced_holds=True,zero_unscheduled_holds=False,full_history=False)
        result=cross_report.judge_attempt(*args)
        self.assertTrue(result['core_engine_red']); self.assertTrue(result['gate_b_eligible'])
        self.assertFalse(result['core_passed']); self.assertEqual(result['oracles']['live_hashes']['status'],'FAIL')
        args[1]['only_capture_induced_holds']=False
        self.assertFalse(cross_report.judge_attempt(*args)['gate_b_eligible'])
        args[1]['only_capture_induced_holds']=True; args[1]['shared_fullstate']=False
        self.assertFalse(cross_report.judge_attempt(*args)['gate_b_eligible'])

    def test_hold_uses_held_peer_last_live_timing_and_adopted_bound(self):
        hold=dict(peer=3,tick=607,source_round=1)
        config=dict(type='adopted_config',peer=3,source_round=1,incarnation=0,tick=1,sim_tick_ms=16.6666,
                    config=dict(rules=dict(slow_player_bound_ticks=3)))
        timing=dict(type='tick_timing',peer=3,source_round=1,incarnation=0,phase='live',tick=600,capture_us=56323,compute_us=7712,wait_us=27,partition_valid=True)
        events={'edith':[config,timing,dict(timing,tick=607,phase='catchup',capture_us=5)]}
        peers={'edith':dict(own_hold_notifications=[dict(tick=607,source_round=1,incarnation=0)])}
        result=cross_report.classify_hold(hold,events,peers)
        self.assertEqual(result['classification'],'capture-induced'); self.assertEqual(result['capture_tick'],600)
        self.assertEqual(result['hold_bound_us'],49000)
        timing['partition_valid']=False
        self.assertEqual(cross_report.classify_hold(hold,events,peers)['classification'],'other')
        timing['partition_valid']=True
        self.assertEqual(cross_report.classify_hold(dict(hold,round=99),events,peers)['classification'],'other')
        events['edith'].append(dict(timing,tick=601,capture_us=20,compute_us=100000))
        self.assertEqual(cross_report.classify_hold(hold,events,peers)['classification'],'other')
        self.assertEqual(cross_report.classify_hold(dict(hold,source_round=2),events,peers)['classification'],'other')

    def test_every_incarnation_exit_needs_its_own_expected_cause(self):
        good=dict(started=True,exit_code=0,timed_out=False)
        crash=dict(started=True,exit_code=137,timed_out=False,injected_termination='scheduled crash drop')
        fault=dict(id='drop',action='crash-restart',peer='a',incarnation=0)
        receipt=dict(id='drop',peer='a',incarnation=0,phase='fault_applied',native=dict(action='crash-restart',applied=True))
        self.assertTrue(cross_report.judge_exit(good,'a',0,[],[])['passed'])
        self.assertFalse(cross_report.judge_exit(crash,'a',0,[fault],[])['passed'])
        self.assertTrue(cross_report.judge_exit(crash,'a',0,[fault],[receipt])['passed'])
        self.assertFalse(cross_report.judge_exit(crash,'a',1,[fault],[receipt])['passed'])
        self.assertFalse(cross_report.judge_exit(dict(good,timed_out=True),'a',0,[],[])['passed'])

    def test_a_box_whose_sim_fits_the_tick_holds_the_rounds_rate(self):
        # The four-machine block's EDITH: 10.4 ms of sim a tick at 55 ticks/s - its sim fits, so the rate is a verdict, not a report.
        self.assertFalse(cross_report.pace_verdict(dict(pace=dict(sim_ms_per_tick=10.4, wall_tps=55.0)))['passed'])
        self.assertTrue(cross_report.pace_verdict(dict(pace=dict(sim_ms_per_tick=10.4, wall_tps=59.6)))['passed'])
        # A box whose sim alone cannot hold the rate is a slow machine: held by the bound, its rate reported.
        slow = cross_report.pace_verdict(dict(pace=dict(sim_ms_per_tick=19.5, wall_tps=48.9)))
        self.assertEqual((slow['gated'], slow['passed']), (False, True))
        self.assertTrue(cross_report.pace_verdict(dict(pace=dict(sim_ms_per_tick=3.1, wall_tps=None)))['gated'])
        self.assertFalse(cross_report.pace_verdict(dict(pace=dict(sim_ms_per_tick=3.1, wall_tps=None)))['passed'])

    def test_a_return_after_an_announced_leave_must_exit_clean(self):
        refused=dict(started=True,exit_code=1,timed_out=False)
        leave=dict(id='left',action='announced-leave-rejoin',peer='a',incarnation=0,return_incarnation=1)
        row=dict(id='left',peer='a',incarnation=1,phase='first_controllable_input',native=dict(recovery_phase='first_controllable_input'))
        self.assertFalse(cross_report.judge_exit(refused,'a',1,[leave],[row])['passed'])
        self.assertTrue(cross_report.judge_exit(dict(refused,exit_code=0),'a',1,[leave],[row])['passed'])

    def test_hold_is_scheduled_only_with_matching_native_start_and_completed_recovery(self):
        recovery=dict(id='stall',peer='a',passed=True,phases=[dict(phase='first_controllable_input',native=dict(source_round=1,tick=45))])
        receipt=dict(id='stall',source_peer='a',peer=3,source_round=1,tick=40,applied=True,action='live-stall')
        hold=dict(peer=3,source_round=1,tick=42)
        self.assertEqual(cross_report.scheduled_hold(hold,[receipt],[recovery]),'stall')
        for altered in [dict(hold,peer=2),dict(hold,source_round=2),dict(hold,tick=46),dict(hold,tick=39)]:
            self.assertIsNone(cross_report.scheduled_hold(altered,[receipt],[recovery]))
        self.assertIsNone(cross_report.scheduled_hold(hold,[receipt],[dict(recovery,passed=False)]))

    def test_numbered_hash_and_timing_rows_require_complete_measured_evidence(self):
        peers={n:dict(frames=1201,tick_timing_valid=True,tick_compute_ms=dict(count=1201),timing=dict(
            complete=True,steady_waits_over_50=2,steady_missing_frame_stalls=4,net_wait_ms=130,waiting_percent=1,longest_stall_ms=70)) for n in 'abc'}
        claimed={r['number']:r['status'] for r in cross_report.requirements({},dict(passed=True),peers)}
        for number in (41,52,'reread-4','reread-5'): self.assertEqual(claimed[number],'PASS')
        peers['b']['tick_timing_valid']=False
        peers['c']['timing']['complete']=False
        claimed={r['number']:r['status'] for r in cross_report.requirements({},dict(passed=False),peers)}
        for number in (41,52,'reread-4','reread-5'): self.assertEqual(claimed[number],'NOT COVERED')


if __name__=='__main__': unittest.main()

def live_row(tick, peer):
    return dict(session='s', match='m', history_branch='initial', source_round=1, tick=tick, peer=peer, instance=peer, execution='one',
                incarnation=0, phase='live', sim_gated='a'*64, subsystems={'controller': 'b'*64, 'sim_rng': 'c'*64})


class HeldSeatAwayRange(unittest.TestCase):
    RANGE = [dict(session='s', match='m', history_branch='initial', source_round=1, first=1, last=10, peers=['a', 'b', 'c'])]
    LEFT = '[net-match] completed_by_next_round=1 held_from=6'

    def judge(self, c_ticks, c_log):
        live = {p: [live_row(t, p) for t in range(1, 11)] for p in ('a', 'b')}
        live['c'] = [live_row(t, 'c') for t in c_ticks]
        away = cross_report.held_away_ranges(live, self.RANGE, {'a': [''], 'b': [''], 'c': [c_log]})
        return away, cross_report.report.compare_histories(live, self.RANGE, {'controller', 'sim_rng'}, away)

    def test_a_held_seat_that_left_its_round_is_away_not_unknown(self):
        away, result = self.judge(range(1, 7), self.LEFT)
        self.assertEqual(away, {('c', ('s', 'm', 'initial', 1)): (7, 10)})
        self.assertTrue(result['passed'])
        self.assertEqual((result['unknown_keys'], result['equal_keys'], result['peers']['c']['away']), (0, 10, 4))

    def test_a_held_seat_abandons_its_ticks_from_its_hold_frame(self):
        # l4p-21's Mac: it simulated frame 561 before the hold from 561 reached it, then left the round on its record.
        c_ticks = [live_row(t, 'c') for t in range(1, 6)] + [dict(live_row(6, 'c'), sim_gated='d' * 64)]
        def judge(log):
            live = {p: [live_row(t, p) for t in range(1, 11)] for p in ('a', 'b')}
            live['c'] = c_ticks
            away = cross_report.held_away_ranges(live, self.RANGE, {'a': [''], 'b': [''], 'c': [log]})
            return away, cross_report.report.compare_histories(live, self.RANGE, {'controller', 'sim_rng'}, away)
        away, result = judge('[net-lockstep] hold of this seat at 6 revision=4' + chr(10) + self.LEFT)
        self.assertEqual(away, {('c', ('s', 'm', 'initial', 1)): (6, 10)})
        self.assertTrue(result['passed'])
        # Without the seat's own hold line the same differing tick is compared.
        away, result = judge(self.LEFT)
        self.assertEqual(result['unequal_keys'], 1)

    def test_a_present_seat_missing_keys_stays_unknown(self):
        # No word from the seat that it left: the same missing tail is UNKNOWN.
        away, result = self.judge(range(1, 7), '')
        self.assertEqual(away, {})
        self.assertFalse(result['passed'])
        self.assertEqual(result['unknown_keys'], 4)
        # A seat that left later still answers for a hole inside the round it played.
        away, result = self.judge([1, 2, 4, 5, 6], self.LEFT)
        self.assertEqual(away, {})
        self.assertEqual(result['unknown_keys'], 5)

class FaultWindowHolds(unittest.TestCase):
    def test_a_hold_of_the_faulted_seat_inside_its_window_is_the_faults(self):
        host = [dict(phase='live', round=7, tick=t, wall_ms=1000.0 + t * 16.7) for t in range(1, 2001)]
        host += [dict(phase='live', round=8, tick=t, wall_ms=40000.0 + t * 16.7) for t in range(1, 2001)]
        clock = cross_report.host_clock(host)
        faults = [dict(id='mac-lag', peer='mac', action='lag', duration_ms=30000)]
        receipts = [dict(id='mac-lag', applied=True, peer=2, round=7, applied_frame=700)]
        windows = cross_report.fault_windows(faults, receipts, clock)
        self.assertEqual([w['id'] for w in windows], ['mac-lag'])
        # The faulted seat held in the next round, still inside the window: the fault's.
        self.assertEqual(cross_report.fault_window_hold(dict(peer=2, round=8, tick=10), windows, clock), 'mac-lag')
        # Another seat held at the same moment is its own.
        self.assertIsNone(cross_report.fault_window_hold(dict(peer=4, round=8, tick=10), windows, clock))
        # The faulted seat held after the window closed is its own.
        self.assertIsNone(cross_report.fault_window_hold(dict(peer=2, round=8, tick=1900), windows, clock))
        # A fault never applied opens no window.
        self.assertEqual(cross_report.fault_windows(faults, [dict(receipts[0], applied=False)], clock), [])

class ImageRejoinHistory(unittest.TestCase):
    RANGE = [dict(session='s', match='m', history_branch='initial', source_round=1, first=1, last=12, peers=['a', 'b', 'c'])]
    RELAUNCH = '[net-match] held client: replaying the private committed tail'

    def live(self):
        live = {p: [live_row(t, p) for t in range(1, 13)] for p in ('a', 'b')}
        restored = [dict(live_row(t, 'c'), history_branch=None, configured_start_frame=8) for t in range(8, 13)]
        replay = [dict(live_row(t, 'c'), history_branch=None, phase='catchup') for t in range(5, 8)]
        live['c'] = [live_row(t, 'c') for t in range(1, 4)] + replay + restored
        return live

    def test_an_image_rejoin_is_compared_from_its_resume_frame(self):
        compared, away = cross_report.adopt_restored_histories(self.live(), self.RANGE, {'c': [self.RELAUNCH]})
        self.assertEqual(away, {('c', ('s', 'm', 'initial', 1)): [(4, 7)]})
        result = cross_report.report.compare_histories(compared, self.RANGE, {'controller', 'sim_rng'}, away)
        self.assertTrue(result['passed'])
        self.assertEqual((result['equal_keys'], result['peers']['c']['away']), (12, 4))
        # A resumed history that differs is a difference, not an absence.
        changed = self.live(); changed['c'][-1] = dict(changed['c'][-1], sim_gated='d' * 64)
        compared, away = cross_report.adopt_restored_histories(changed, self.RANGE, {'c': [self.RELAUNCH]})
        self.assertEqual(cross_report.report.compare_histories(compared, self.RANGE, {'controller', 'sim_rng'}, away)['unequal_keys'], 1)

    def test_two_returns_in_one_round_are_both_compared(self):
        # The four-box run's Mac: an image return at 7978, held again, an in-place return at 10295.
        rng = [dict(self.RANGE[0], last=20)]
        live = {p: [live_row(t, p) for t in range(1, 21)] for p in ('a', 'b')}
        first = [dict(live_row(t, 'c'), history_branch=None, configured_start_frame=8) for t in range(8, 12)]
        second = [dict(live_row(t, 'c'), history_branch=None, configured_start_frame=15) for t in range(15, 21)]
        live['c'] = [live_row(t, 'c') for t in range(1, 4)] + first + second
        compared, away = cross_report.adopt_restored_histories(live, rng, {'c': [self.RELAUNCH]})
        self.assertEqual(away, {('c', ('s', 'm', 'initial', 1)): [(4, 7), (12, 14)]})
        result = cross_report.report.compare_histories(compared, rng, {'controller', 'sim_rng'}, away)
        self.assertTrue(result['passed'])
        self.assertEqual((result['equal_keys'], result['peers']['c']['away']), (20, 7))

    def test_records_without_a_relaunch_stay_unplaced(self):
        compared, away = cross_report.adopt_restored_histories(self.live(), self.RANGE, {'c': ['']})
        self.assertEqual(away, {})
        self.assertFalse(cross_report.report.compare_histories(compared, self.RANGE, {'controller', 'sim_rng'}, away)['passed'])

class ScheduleKeyedOracles(unittest.TestCase):
    def judge(self, faults, **checks):
        manifest=dict(scenario='soak',ticks=14400,faults=faults,fullstate_every=600,capture_rows_pending=[])
        base=dict.fromkeys(cross_report.CORE_CHECKS,True); base.update(checks)
        peers={n:dict(memory_by_incarnation={'0':dict(passed=True,sizes={},missing_samples=0)}) for n in ('a','b','c')}
        return cross_report.judge_attempt(manifest,base,peers,[],[])['oracles']

    def test_items_the_schedule_never_asks_for_are_not_applicable(self):
        lag=[dict(id='lag',peer='mac',action='lag',duration_ms=1000)]
        o=self.judge(lag, round_ended=False)
        self.assertEqual((o['forced_ends']['status'],o['rematches']['status'],o['autosaves']['status']),('NOT APPLICABLE',)*3)

    def test_what_the_schedule_asks_for_is_judged(self):
        hold=[dict(id='end',peer='erol',action='brain-eliminate',phase='hold'),dict(id='crash',peer='edith',action='crash-restart')]
        o=self.judge(hold, round_ended=True, forced_end_during_hold=True, forced_end_during_transfer=False, changed_settings_rematch=True, fog_on_match=True, validated_autosave_archives=False)
        # Only the scheduled hold phase is judged; a transfer-phase end the schedule never forced does not count against it.
        self.assertEqual(o['forced_ends']['status'],'PASS')
        self.assertEqual(o['rematches']['status'],'PASS')
        self.assertEqual(o['autosaves']['status'],'FAIL')
        o=self.judge(hold, round_ended=True, forced_end_during_hold=False)
        self.assertEqual(o['forced_ends']['status'],'FAIL')

