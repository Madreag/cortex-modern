import unittest
import cross_report
from feel.test_attempt_requirements import attempt


class AcceptanceRoster(unittest.TestCase):
    def test_identity_checks_native_builds_and_host_role(self):
        self.assertTrue(cross_report.judge_attempt(*attempt())['v1_passed'])
        for case in ('tip', 'hash', 'runner', 'host', 'machine', 'box'):
            with self.subTest(case=case):
                args = attempt()
                if case == 'tip': args[0]['preflights']['Linux']['build']['commit'] = 'c'*40
                if case == 'hash': args[0]['preflights']['Linux']['build']['executable_sha256'] = 'c'*64
                if case == 'runner': args[2]['linux']['record']['exe_sha256'] = 'c'*64
                if case == 'host': args[0]['specs'][0]['role'] = 'player'
                if case == 'machine': args[0]['preflights']['Linux']['machine_id'] = 'Mac'
                if case == 'box': args[0]['instances'][-1]['box'] = 'Mac'
                self.assertFalse(cross_report.judge_attempt(*args)['v1_passed'])

    def test_forged_identity_checks_cannot_hide_missing_build_receipt(self):
        args = attempt()
        args[0]['preflights'] = {}
        self.assertFalse(cross_report.judge_attempt(*args)['v1_passed'])

    def test_three_box_evidence_is_diagnostic_only(self):
        args = attempt()
        args[0]['boxes'] = [dict(name=name) for name in ('EROL-PC', 'EDITH', 'Mac')]
        args[0]['instances'] = [dict(name=name, box=box) for name, box in
                                zip(('erol', 'edith', 'mac'), ('EROL-PC', 'EDITH', 'Mac'))]
        args[0]['host'] = 'erol'
        del args[2]['linux']
        self.assertFalse(cross_report.judge_attempt(*args)['v1_passed'])


if __name__ == '__main__':
    unittest.main()
