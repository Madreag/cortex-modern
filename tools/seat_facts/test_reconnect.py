"""Exercise the reconnect evidence oracle without launching an engine."""
import copy
import unittest

from check_reconnect import absent, local_view, pair


def view(player, frame=1):
    return {"event": "local_control", "round_id": 2, "frame": frame, "player_index": player,
        "input_player": 0, "player_controller_input": 0, "controller_input": 0, "screen": 0,
        "seat_mode": 1, "seat_player": player, "player_active": True, "player_human": True,
        "controlled_uid": 101 + player, "team": player, "brain_uid": 101 + player,
        "seat_facts": [{"player": index, "active": index < 2, "human": index < 2,
            "team": index if index < 2 else -1, "brain_uid": 101 + index if index < 2 else 0,
            "input": 0 if index == player else -1, "screen": 0 if index == player else -1} for index in range(4)]}


class ReconnectOracle(unittest.TestCase):
    def test_client_uses_physical_zero(self):
        local_view(view(1), 1, [0, 1])

    def test_rebuilt_identity_input_map_is_red(self):
        row = view(1)
        for name in ("input_player", "player_controller_input", "controller_input"):
            row[name] = 1
        with self.assertRaisesRegex(ValueError, "physical zero"):
            local_view(row, 1, [0, 1])

    def test_missing_shared_roster_is_red(self):
        row = view(1)
        del row["seat_facts"]
        with self.assertRaisesRegex(ValueError, "four-seat"):
            local_view(row, 1, [0, 1])

    def test_remote_screen_alias_is_red(self):
        row = view(1)
        row["seat_facts"][0]["screen"] = 0
        with self.assertRaisesRegex(ValueError, "remote seat"):
            local_view(row, 1, [0, 1])

    def test_canonical_controller_seat_is_required(self):
        row = view(1)
        row["seat_player"] = 0
        with self.assertRaisesRegex(ValueError, "canonical seat"):
            local_view(row, 1, [0, 1])

    def test_shared_frames_and_brains(self):
        host = [view(0, frame) for frame in range(1, 62)]
        client = [view(1, frame) for frame in range(1, 62)]
        self.assertEqual(pair(host, client, 1, [0, 1])["frames"], 61)
        bad = copy.deepcopy(client)
        bad[0]["seat_facts"][0]["brain_uid"] += 1
        with self.assertRaisesRegex(ValueError, "disagree"):
            pair(host, bad, 1, [0, 1])

    def test_restore_boundary_must_be_observed(self):
        host = [view(0, frame) for frame in range(2, 63)]
        client = [view(1, frame) for frame in range(1, 63)]
        with self.assertRaisesRegex(ValueError, "first returner frame"):
            pair(host, client, 1, [0, 1])

    def test_missing_and_duplicate_frames_are_red(self):
        host = [view(0, frame) for frame in range(1, 62)]
        with self.assertRaisesRegex(ValueError, "no shared"):
            pair(host, [], 1, [0, 1])
        client = [view(1, frame) for frame in range(1, 61)]
        with self.assertRaisesRegex(ValueError, "duplicate"):
            pair(host, client + [client[0]], 1, [0, 1])

    def test_denied_applicant_never_gets_gameplay(self):
        self.assertTrue(absent([])["pass"])
        with self.assertRaisesRegex(ValueError, "unadmitted"):
            absent([view(1)])

    def test_retained_a7_oracle_reports_both_interpretations(self):
        from reconnect import A7, load
        load("a7_support", A7 / "a7_support.py")
        retained = load("test_a7_oracle", A7 / "a7_driver.py")
        row = {**view(1), "peer_id": 2}
        with self.assertRaisesRegex(retained.GateError, "not seated at the local player"):
            retained.validate_local_control(row, row)
        local_view(row, 1, [0, 1])
        physical = {**row, "seat_player": row["controller_input"]}
        retained.validate_local_control(physical, physical)


if __name__ == "__main__":
    unittest.main()
