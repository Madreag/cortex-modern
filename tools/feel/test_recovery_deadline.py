import unittest
import json
from pathlib import Path
import tempfile
from feel import report
from feel.records import RecoveryLedger


def case():
    return dict(id='lag', action='lag', peer='mac', incarnation=0, deadline_ms=120000,
                outcomes=['first_controllable_input'], duration_ms=120000, duration_ticks=7200)


def event(phase, wall):
    return dict(id='lag', peer='mac', incarnation=0, phase=phase, wall_ms=wall, clock_domain='mac-payload')


class RecoveryDeadline(unittest.TestCase):
    def test_p07_requested_duration_cannot_replace_the_reset_receipt(self):
        result = report.reduce_recoveries([case()], [event('fault_applied', 0),
            event('first_controllable_input', 240000)], now_ms=240000)[0]
        self.assertFalse(result['passed'], result)

    def test_p27_deadline_starts_at_the_observed_reset(self):
        fault = dict(case(), duration_ticks=1)
        result = report.reduce_recoveries([fault], [event('fault_applied', 0), event('fault_reset', 17),
            event('first_controllable_input', 200000)], now_ms=200000)[0]
        self.assertFalse(result['passed'], result)
        self.assertEqual(result['recovery_after_fault_end_ms'], 199983)

    def test_observed_end_keeps_the_declared_post_fault_deadline(self):
        rows = [event('fault_applied', 0), event('fault_reset', 120000), event('first_controllable_input', 240000)]
        self.assertTrue(report.reduce_recoveries([case()], rows, now_ms=240000)[0]['passed'])
        rows[-1]['wall_ms'] += 1
        self.assertFalse(report.reduce_recoveries([case()], rows, now_ms=240001)[0]['passed'])

    def test_reset_uses_owning_payload_bounds_and_rejects_wrong_identity(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'recovery.jsonl'
            ledger = RecoveryLedger(path, 'mac', 'payload:mac', 0, [case()])
            ledger.observe([dict(type='fault_begin', id='lag'), dict(type='fault', id='lag', action='lag', applied=True)], 0, 0, 10)
            ledger.observe([dict(type='fault_reset', id='lag', send_recv_armed=True, wall_ms=10**12)], 0, 119990, 120010)
            ledger.observe([dict(type='recovery', id='lag', recovery_phase='first_controllable_input', terminal=True)], 0, 239900, 240000)
            rows = [json.loads(line) for line in path.read_text().splitlines()]
        result = report.reduce_recoveries([case()], rows, now_ms=240000)[0]
        self.assertFalse(result['passed'], result)
        self.assertEqual(result['recovery_after_fault_end_ms'], 120010)
        rows[1]['incarnation'] = 1
        self.assertFalse(report.reduce_recoveries([case()], rows, now_ms=240000)[0]['passed'])

    def test_reset_in_another_clock_domain_is_not_comparable(self):
        rows = [event('fault_applied', 0), dict(event('fault_reset', 120000), clock_domain='other'),
                event('first_controllable_input', 130000)]
        self.assertFalse(report.reduce_recoveries([case()], rows, now_ms=130000)[0]['passed'])


if __name__ == '__main__':
    unittest.main()
