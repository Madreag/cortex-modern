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
    (root / peer / "stdout.log").write_text(f"[net-lockstep] start round={ROUND} peer={peer_id}\n" + census + log, encoding="utf-8")
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

    def test_an_engine_whose_sim_does_not_fit_is_reported_not_gated(self) -> None:
        for peer, peer_id in (("host", 1), ("client", 2)):
            write_peer(self.root, peer, peer_id, 1000 / 57.3, 17.5, TICKS + 1)
        result = soak.acceptance_history(self.root, TICKS)
        self.assertTrue(result["pass"], result["errors"])
        self.assertTrue(all(not window["gated"] for window in result["pace_windows"]["host"]))

    def test_a_hold_after_a_return_is_excused_only_for_a_slow_machine_whose_sim_does_not_fit(self) -> None:
        row = {"round": ROUND, "held_peer": 1, "hold_tick": 1325, "returned_peer": 1, "return_tick": 1234}
        slow = "[net-lockstep] slow machine peer 1 at frame 1300 : it runs 52 ticks a second\n"
        write_peer(self.root, "host", 1, 1000 / 60, 17.5, TICKS + 1, slow)
        write_peer(self.root, "client", 2, 1000 / 60, 8.0, TICKS + 1)
        excused, kept = soak.excused_return_holds(self.root, [row])
        self.assertEqual((len(excused), len(kept)), (1, 0))
        write_peer(self.root, "host", 1, 1000 / 60, 8.7, TICKS + 1, slow)
        excused, kept = soak.excused_return_holds(self.root, [row])
        self.assertEqual((len(excused), len(kept)), (0, 1), "an engine whose sim fits is not a slow machine")
        write_peer(self.root, "host", 1, 1000 / 60, 17.5, TICKS + 1)
        excused, kept = soak.excused_return_holds(self.root, [row])
        self.assertEqual((len(excused), len(kept)), (0, 1), "a hold the engine did not call a slow machine's stays a defect")


if __name__ == "__main__":
    unittest.main(verbosity=2)
