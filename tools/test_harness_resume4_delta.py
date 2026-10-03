"""Measured-duration and immediately causal own-seat evidence; no engines."""
import unittest

import cross_report
from test_harness_resume3 import COUNT_CATEGORIES, ROUND_START, OWN_WARNING, OWN_PROPOSAL, OWN_COMMIT, classified


def counts(declared, progress):
    events = {peer: [dict(type='progress', budget_tick=value)] if value is not None else []
              for peer, value in progress.items()}
    return [row for row in cross_report.coverage(events, dict.fromkeys(progress, {}), dict(scenario='match', ticks=declared))
            if row['id'] in COUNT_CATEGORIES]


class Resume4Delta(unittest.TestCase):
    def test_y06_long_measured_progress_cannot_borrow_a_short_declaration(self):
        for row in counts(1201, dict(host=72000, client=72000)):
            self.assertTrue(row['required'], row)
            self.assertEqual(row['status'], 'FAIL', row)

    def test_y07_intervening_proposal_consumes_the_host_warning(self):
        self.assertTrue(classified(ROUND_START + OWN_WARNING + OWN_PROPOSAL + OWN_COMMIT))
        intervening = '[net-lockstep] propose hold peer=1 next_frame=110 cause=silent\n'
        self.assertFalse(classified(ROUND_START + OWN_WARNING + intervening + OWN_PROPOSAL + OWN_COMMIT))

    def test_short_measured_attempt_is_inapplicable_but_missing_progress_is_incomplete(self):
        for row in counts(1201, dict(host=1201, client=1201)):
            self.assertFalse(row['required'])
            self.assertEqual(row['status'], 'NOT APPLICABLE')
            self.assertEqual(row['reason'], 'below the coverage window: 1201 ticks')
        for progress in (dict(host=1201, client=None), dict(host=None, client=None), dict(host=True, client=1201)):
            for row in counts(1201, progress):
                self.assertTrue(row['required'])
                self.assertEqual(row['status'], 'INCOMPLETE', row)

    def test_either_a_long_declaration_or_one_long_peer_keeps_the_full_minima(self):
        for declared, progress in ((72000, dict(host=1201, client=1201)), (1201, dict(host=1201, client=36000))):
            for row in counts(declared, progress):
                self.assertTrue(row['required'])
                self.assertEqual(row['status'], 'FAIL')

    def test_refused_or_deduplicated_warning_cannot_reach_the_lateness_path(self):
        for intervening in ('[net-lockstep] hold refused peer=1\n',
                            '[net-lockstep] own seat late at frame 110: missing_ms=60\n'):
            with self.subTest(intervening=intervening):
                self.assertFalse(classified(ROUND_START + OWN_WARNING + intervening + OWN_PROPOSAL + OWN_COMMIT))


if __name__ == '__main__':
    unittest.main()
