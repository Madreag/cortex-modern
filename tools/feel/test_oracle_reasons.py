import unittest
import cross_report
from feel.test_attempt_requirements import attempt


class OracleReasons(unittest.TestCase):
    def test_failed_fullstate_reason_names_the_observed_key(self):
        args = attempt()
        args[1]['shared_fullstate'] = False
        result = cross_report.judge_attempt(*args)['oracles']['full_state']
        self.assertIn('shared_fullstate=False', result['reason'])


if __name__ == '__main__':
    unittest.main()
