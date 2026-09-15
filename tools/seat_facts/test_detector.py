"""Exercise the seat detector without launching an engine."""
import copy
import unittest

from run_pair import score_seats


def logs(rows):
    return {peer: "\n".join("[seat-facts] player=%d active=%d human=%d team=%d brain=%d mark=%d screen=%d" % tuple(row)
                            for row in values) for peer, values in rows.items()}


class SeatDetectorTest(unittest.TestCase):
    def setUp(self):
        self.rows = {"host": [[0, 1, 1, 0, 1048577, 101, 0], [1, 1, 1, 1, 1048596, 102, -1],
                              [2, 0, 0, -1, 0, 0, -1], [3, 0, 0, -1, 0, 0, -1]],
                     "client": [[0, 1, 1, 0, 1048577, 101, -1], [1, 1, 1, 1, 1048596, 102, 0],
                                [2, 0, 0, -1, 0, 0, -1], [3, 0, 0, -1, 0, 0, -1]]}

    def test_shared_facts_with_local_screens(self):
        self.assertTrue(score_seats(logs(self.rows))["pass"])

    def test_original_local_only_seats_fail(self):
        self.rows["host"][1] = [1, 0, 0, 0, 0, 0, -1]
        self.rows["client"][0] = [0, 1, 1, 1, 1048596, 101, 0]
        self.rows["client"][1] = [1, 0, 0, 0, 0, 0, -1]
        self.assertFalse(score_seats(logs(self.rows))["pass"])

    def test_different_brain_fails(self):
        self.rows["client"][1][4] = 1048787
        self.assertFalse(score_seats(logs(self.rows))["pass"])

    def test_missing_remote_branch_fails(self):
        self.rows["host"][1][5] = 0
        self.assertFalse(score_seats(logs(self.rows))["pass"])

    def test_two_screens_on_each_peer_fail(self):
        for peer in self.rows:
            self.rows[peer][0][6], self.rows[peer][1][6] = 0, 1
        self.assertFalse(score_seats(logs(self.rows))["pass"])

    def test_missing_or_duplicate_rows_fail(self):
        for peer in self.rows:
            for offset in range(4):
                rows = copy.deepcopy(self.rows)
                rows[peer].pop(offset)
                self.assertFalse(score_seats(logs(rows))["pass"])
                rows = copy.deepcopy(self.rows)
                rows[peer].append(rows[peer][offset])
                self.assertFalse(score_seats(logs(rows))["pass"])

    def test_missing_peer_fails(self):
        self.assertFalse(score_seats({})["pass"])
        self.assertFalse(score_seats({"host": logs(self.rows)["host"]})["pass"])


if __name__ == "__main__":
    unittest.main()
