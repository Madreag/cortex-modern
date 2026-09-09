"""Verify reconnect evidence distinguishes a missing ledger from dead units."""

import unittest

from h4_gate_evidence import reseat_evidence


def counters():
    return {
        "host_seats_dropped": 1,
        "host_ledger_drops_recorded": 1,
        "host_reseats_issued": 1,
        "host_reseats_without_a_ledger": 0,
        "host_reseats_without_survivors": 0,
        "host_reseat_live_on_team_not_named": 0,
    }


class ReseatEvidenceTests(unittest.TestCase):
    def test_issued_handoff_requires_observed_dispatch(self):
        self.assertTrue(reseat_evidence(counters(), "[net-reconnect] reseating team 1")[0])
        self.assertFalse(reseat_evidence(counters(), "")[0])

    def test_no_surviving_units_is_distinct_from_a_missing_ledger(self):
        data = counters()
        data.update(host_reseats_issued=0, host_reseats_without_survivors=1)
        self.assertTrue(reseat_evidence(data, "")[0])
        data["host_reseats_without_a_ledger"] = 1
        self.assertFalse(reseat_evidence(data, "")[0])

    def test_later_units_remain_visible(self):
        data = counters()
        data.update(host_reseats_issued=0, host_reseats_without_survivors=1,
                    host_reseat_live_on_team_not_named=2)
        accepted, detail = reseat_evidence(data, "")
        self.assertTrue(accepted)
        self.assertIn("host_reseat_live_on_team_not_named=2", detail)

    def test_missing_or_invalid_counters_cannot_default_to_zero(self):
        for name in counters():
            for value in (None, "0", -1, False):
                data = counters()
                data[name] = value
                self.assertFalse(reseat_evidence(data, "[net-reconnect] reseating team 1")[0])

    def test_drop_and_decision_must_both_be_present(self):
        for change in ({"host_seats_dropped": 0}, {"host_ledger_drops_recorded": 0},
                       {"host_reseats_issued": 0}):
            data = counters()
            data.update(change)
            self.assertFalse(reseat_evidence(data, "[net-reconnect] reseating team 1")[0])


if __name__ == "__main__":
    unittest.main()
