"""The soak's own judgement: the terminal tick, the pace windows through pace_verdict, and which holds after a return are
excused.

    python tools/test_soak_two_peer.py
"""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import soak_two_peer as soak  # noqa: E402

TICKS = 3900
ROUND = 7


def write_peer(root: Path, peer: str, peer_id: int, tick_ms: float, census_sim_ms: float, last_tick: int, log: str = "") -> None:
    subsystems = {name: "0" for name in ("actors", "funds", "lua_state", "rot_angle", "rot_angvel", "scene", "sim_rng", "terrain",
                                         "tick", "controller")}
    rows = [json.dumps({"round": ROUND, "tick": tick, "wall_ms": tick * tick_ms, "paused": False, "sim_gated": "0", "subsystems": subsystems})
            for tick in range(1, last_tick + 1)]
    (root / f"{peer}-live.jsonl").write_text("\n".join(rows) + "\n", encoding="utf-8")
    (root / peer).mkdir(parents=True, exist_ok=True)
    census = "".join(f"[mem-census] tick={tick} private_mb=1 pace: wall_tps={1000 / tick_ms:.3f} sim_ms_per_tick={census_sim_ms}\n"
                     for tick in (3600, 7200))
    (root / peer / "stdout.log").write_text(f"[net-lockstep] start round={ROUND} frame=1 local_peer={peer_id} peers=2\n" + census + log, encoding="utf-8")
    (root / f"{peer}_report.json").write_text(json.dumps({"service": {"local_peer_id": peer_id}}), encoding="utf-8")


class SoakJudgement(unittest.TestCase):
    def setUp(self) -> None:
        self.scratch = tempfile.TemporaryDirectory()
        self.root = Path(self.scratch.name)

    def tearDown(self) -> None:
        self.scratch.cleanup()

    def test_the_terminal_tick_one_past_the_count_is_the_engines_own(self) -> None:
        for peer, peer_id in (("host", 1), ("client", 2)):
            write_peer(self.root, peer, peer_id, 1000 / 60, 8.0, TICKS + 1)
        result = soak.acceptance_history(self.root, TICKS)
        self.assertTrue(result["pass"], result["errors"])

    def test_a_tick_past_the_terminal_one_is_extra(self) -> None:
        for peer, peer_id in (("host", 1), ("client", 2)):
            write_peer(self.root, peer, peer_id, 1000 / 60, 8.0, TICKS + 2)
        self.assertIn(f"host round {ROUND}: missing/extra ticks or terminal tick", soak.acceptance_history(self.root, TICKS)["errors"])

    def test_an_engine_whose_sim_fits_is_held_to_the_rate(self) -> None:
        for peer, peer_id in (("host", 1), ("client", 2)):
            write_peer(self.root, peer, peer_id, 1000 / 57.3, 8.7, TICKS + 1)
        result = soak.acceptance_history(self.root, TICKS)
        self.assertFalse(result["pass"])
        self.assertTrue(all(window["gated"] and not window["passed"] for window in result["pace_windows"]["host"]))

    def test_slow_simulation_without_relative_evidence_is_incomplete(self) -> None:
        for peer, peer_id in (("host", 1), ("client", 2)):
            write_peer(self.root, peer, peer_id, 1000 / 57.3, 17.5, TICKS + 1)
        result = soak.acceptance_history(self.root, TICKS)
        self.assertFalse(result["pass"])
        self.assertTrue(all(window["gated"] and not window['passed'] for window in result["pace_windows"]["host"]))

    def test_a_uniformly_heavy_round_needs_both_native_capacity_receipts(self) -> None:
        for peer, peer_id in (("host", 1), ("client", 2)):
            write_peer(self.root, peer, peer_id, 1000 / 50, 20, TICKS + 1)
            (self.root / f'{peer}_report.json').write_text(json.dumps(dict(service=dict(local_peer_id=peer_id),
                pace=dict(sim_ms_per_tick=20), lockstep=dict(round_id=ROUND, local_capacity_tps=50, sim_tick_ms=1000/60))))
        self.assertTrue(soak.acceptance_history(self.root, TICKS)['pass'])
        (self.root / 'client_report.json').write_text('{}')
        self.assertFalse(soak.acceptance_history(self.root, TICKS)['pass'])

    def test_an_injected_stall_is_its_own_seats_away_time(self) -> None:
        # The client stalls 1.5 s at tick 1000 (the soak's own fault) and is held at 1004 until 1100.
        stall = ("[net-test] live stall frame=1000 ms=1500\n[net-lockstep] hold of this seat at 1004 revision=1\n"
                 f"[net-match] seat-reclaimed peer=2 frame=1100 live_actors=2\n")
        write_peer(self.root, "host", 1, 1000 / 60, 8.0, TICKS + 1)
        write_peer(self.root, "client", 2, 1000 / 60, 8.0, TICKS + 1, stall)
        path = self.root / "client-live.jsonl"
        rows = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
        for row in rows:
            if row["tick"] > 1000:
                row["wall_ms"] += 1500
        path.write_text("".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")
        self.assertTrue(soak.acceptance_history(self.root, TICKS)["pass"], "the stalled seat was charged its own injected stall")
        (self.root / "client" / "stdout.log").write_text(f"[net-lockstep] start round={ROUND} frame=1 local_peer=2 peers=2\n" + stall.replace("frame=1000", "frame=900"),
                                                        encoding="utf-8")
        self.assertFalse(soak.acceptance_history(self.root, TICKS)["pass"], "a stall its hold does not follow at once is still charged")

    def test_a_soaks_holds_are_judged_by_its_own_plan(self) -> None:
        (self.root / "host").mkdir(parents=True, exist_ok=True)
        (self.root / "client").mkdir(parents=True, exist_ok=True)

        def host(extra: str = "") -> None:
            (self.root / "host" / "stdout.log").write_text(
                "[net-lockstep] start round=7 frame=1 local_peer=1 peers=2\n"
                "[net-match] hold peer=2 frame=1005 AI in control\n" + extra + "[net-match] hold peer=2 frame=2500 AI in control\n", encoding="utf-8")
        (self.root / "client" / "stdout.log").write_text(
            "[net-lockstep] start round=7 frame=1 local_peer=2 peers=2\n"
            "[net-test] live stall frame=1000 ms=1500\n"
            "[net-match] seat-reclaimed peer=2 frame=1100 live_actors=2\n"
            "[net-match] private catch-up complete frame=2620 in_place=1\n", encoding="utf-8")
        from test_soak_oracle_evidence import control
        (self.root / 'client/events.jsonl').write_text('\n'.join(json.dumps(control(tick)) for tick in (1101, 2621)))
        # The planned stall's hold is planned; the other has no named cause.
        host()
        judged = soak.soak_hold_judgement(self.root, [1000])
        self.assertEqual((judged["planned"], judged["passed"]), ([1005], False))
        self.assertFalse(judged["unexplained"][0]["cause"])
        # A lost frame named at that tick, followed by own activation and fresh applied input.
        host("[lockstep-recv] asked peer 2 to resend frame=2500 of peer 2 after 51 ms; highest heard=2499\n")
        self.assertTrue(soak.soak_hold_judgement(self.root, [1000])["passed"])
        # The same hold the survivor felt is a defect.
        host("[lockstep-recv] asked peer 2 to resend frame=2500 of peer 2 after 51 ms; highest heard=2499\n"
             "[net-frame-wait] frame=2500 wait_ms=90 on=Client\n")
        self.assertFalse(soak.soak_hold_judgement(self.root, [1000])["passed"])
        # A cause with no recovery after it is a defect.
        host("[lockstep-recv] asked peer 2 to resend frame=2500 of peer 2 after 51 ms; highest heard=2499\n")
        (self.root / "client" / "stdout.log").write_text("", encoding="utf-8")
        self.assertFalse(soak.soak_hold_judgement(self.root, [1000])["passed"])

    def test_a_hold_after_a_return_is_excused_only_for_a_slow_machine_whose_sim_does_not_fit(self) -> None:
        row = {"round": ROUND, "held_peer": 1, "hold_tick": 1325, "returned_peer": 1, "return_tick": 1234}
        slow = "[net-lockstep] slow machine peer 1 at frame 1300 : it runs 52 ticks a second\n"
        write_peer(self.root, "host", 1, 1000 / 60, 17.5, TICKS + 1, slow)
        write_peer(self.root, "client", 2, 1000 / 60, 8.0, TICKS + 1)
        excused, kept = soak.excused_return_holds(self.root, [row])
        self.assertEqual((len(excused), len(kept)), (0, 1), 'the log supplies no relative capacity or waiting receipt')
        write_peer(self.root, "host", 1, 1000 / 60, 8.7, TICKS + 1, slow)
        excused, kept = soak.excused_return_holds(self.root, [row])
        self.assertEqual((len(excused), len(kept)), (0, 1), "an engine whose sim fits is not a slow machine")
        write_peer(self.root, "host", 1, 1000 / 60, 17.5, TICKS + 1)
        excused, kept = soak.excused_return_holds(self.root, [row])
        self.assertEqual((len(excused), len(kept)), (0, 1), "a hold the engine did not call a slow machine's stays a defect")

    def test_a_return_hold_needs_the_hosts_published_capacity_receipt(self) -> None:
        row = dict(round=ROUND, held_peer=2, hold_tick=1325, returned_peer=2, return_tick=1234)
        log = ("[net-lockstep] slow machine peer 2 at frame 1323: it runs 40 ticks/s against the fastest's 60; the AI takes its seat\n"
               "[net-lockstep] propose hold peer=2 next_frame=1323 cause=capacity\n"
               "[net-match] hold peer=2 frame=1325 AI in control\n")
        write_peer(self.root, 'host', 1, 1000/60, 8, TICKS+1, log)
        write_peer(self.root, 'client', 2, 25, 25, TICKS+1)
        excused, kept = soak.excused_return_holds(self.root, [row])
        self.assertEqual((len(excused), len(kept)), (1, 0))
        write_peer(self.root, 'host', 1, 1000/60, 8, TICKS+1, log.replace('40 ticks/s', '60 ticks/s'))
        self.assertEqual(len(soak.excused_return_holds(self.root, [row])[1]), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
