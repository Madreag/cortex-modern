"""Acceptance collection scope for standard matches and the two scheduled four-box arms."""
import copy
import unittest

import cross_report
from feel.host_loss import host_loss_evidence
from feel.test_attempt_requirements import attempt
from test_acceptance_resume import INVENTORY
if INVENTORY.is_dir():
    import extract_defects


@unittest.skipUnless(INVENTORY.is_dir(), 'external inventory tools are absent')
class CrossCollectionApplicability(unittest.TestCase):
    def test_unasked_host_loss_is_not_a_required_failure(self):
        loss = host_loss_evidence(dict(faults=[]), {}, {}, {}, [])
        self.assertTrue(extract_defects.file_verdict(dict(passed=True, host_loss=loss), 'result.json'))

    def test_planning_requirements_do_not_override_current_acceptance_oracles(self):
        planning = cross_report.requirements({}, {}, {})
        self.assertTrue(extract_defects.file_verdict(dict(passed=True, requirements=planning), 'result.json'))

    def test_declared_l4p_workload_has_a_reachable_required_predicate(self):
        args = attempt()
        args[0].update(scenario='match', acceptance_row=17, acceptance_arm='L4P', ticks=12001, faults=[
            dict(id='l4p', peer='edith', action='jitter', tick=600, lag_ms=200, jitter_ms=60,
                 percent=5, duration_ms=120000, duration_ticks=7200)])
        args[1].update(l4p_effects=True, survivor_feel=True, survivor_pace=True)
        self.assertTrue(cross_report.judge_attempt(*args)['v1_passed'])
        for key in ('l4p_effects', 'survivor_feel', 'survivor_pace', 'memory_bounds', 'full_history'):
            changed = copy.deepcopy(args); changed[1][key] = False
            self.assertFalse(cross_report.judge_attempt(*changed)['v1_passed'], key)


if __name__ == '__main__':
    unittest.main()
