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
        self.assertTrue(summary['gate_b_eligible'])
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
