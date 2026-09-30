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

    def test_a_return_told_it_left_exits_refused_by_design(self):
        refused=dict(started=True,exit_code=1,timed_out=False)
        leave=dict(id='left',action='announced-leave-rejoin',peer='a',incarnation=0,return_incarnation=1)
        told=dict(id='left',peer='a',incarnation=1,phase='told_it_left',native=dict(recovery_phase='told_it_left',terminal=True))
        self.assertTrue(cross_report.judge_exit(refused,'a',1,[leave],[told])['passed'])
        # Only its own terminal row excuses it: no row, another incarnation's, or another phase fail as before.
        self.assertFalse(cross_report.judge_exit(refused,'a',1,[leave],[])['passed'])
        self.assertFalse(cross_report.judge_exit(refused,'a',0,[leave],[dict(told,incarnation=0)])['passed'])
        self.assertFalse(cross_report.judge_exit(refused,'a',1,[leave],[dict(told,phase='catch_up')])['passed'])

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
