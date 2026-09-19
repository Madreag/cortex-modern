"""The sim's controller update opens and closes the producing pass around the right boundaries.

The pass holds the frame the wire committed for the tick aside while this machine samples its own
seats, and gives it back after the local frames are snapshotted: a call that goes missing, or that
ends the pass before the snapshot, silently puts produced input on the wire and into the sim.
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parent / "Source" / "Managers" / "MovableMan.cpp"
FUNCTION = "void MovableMan::UpdateControllers()"

BEGIN = "producingActors = BeginLockstepProducingPass(m_Actors, isLocalControllerActor);"
AI_PASS = "actor->GetController()->Update();"
SNAPSHOT = "SnapshotLockstepControllerFrames(m_Actors, true);"
END = "EndLockstepProducingPass(producingActors);"
QUEUE = "ScenarioRunner::QueueLockstepLocalControllerFrames(simTick, localFrames, &error)"


def function_body(text: str, signature: str) -> tuple[int, list[str]]:
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if line.startswith(signature):
            for end in range(index + 1, len(lines)):
                if lines[end] == "}":
                    return index + 1, lines[index:end + 1]
            raise AssertionError(f"{signature} has no closing brace in {SOURCE}")
    raise AssertionError(f"{signature} is gone from {SOURCE}")


def line_of(body: list[str], first_line: int, needle: str) -> list[int]:
    return [first_line + offset for offset, line in enumerate(body) if needle in line]


class ProducingPassCallSite(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.first_line, cls.body = function_body(SOURCE.read_text(encoding="utf-8"), FUNCTION)

    def sites(self, needle: str) -> list[int]:
        return line_of(self.body, self.first_line, needle)

    def test_the_pass_is_opened_once_before_the_local_ai_pass(self) -> None:
        begins = self.sites(BEGIN)
        updates = self.sites(AI_PASS)
        self.assertEqual(1, len(begins), f"{FUNCTION} opens the producing pass {len(begins)} times: {begins}")
        self.assertTrue(updates, f"{FUNCTION} no longer updates a controller; begin at {begins}")
        self.assertLess(begins[0], min(updates), f"the pass opens at {begins[0]}, after the local controller update at {min(updates)}")

    def test_the_pass_is_closed_once_after_the_local_frames_are_snapshotted(self) -> None:
        ends = self.sites(END)
        snapshots = self.sites(SNAPSHOT)
        self.assertEqual(1, len(ends), f"{FUNCTION} closes the producing pass {len(ends)} times: {ends}")
        self.assertEqual(1, len(snapshots), f"{FUNCTION} snapshots the local frames {len(snapshots)} times: {snapshots}")
        self.assertGreater(ends[0], snapshots[0], f"the pass closes at {ends[0]}, before the snapshot at {snapshots[0]}")

    def test_the_pass_is_closed_before_the_frames_are_queued(self) -> None:
        ends = self.sites(END)
        queues = self.sites(QUEUE)
        self.assertTrue(queues, f"{FUNCTION} no longer queues the local frames; end at {ends}")
        self.assertLess(ends[0], min(queues), f"the pass closes at {ends[0]}, after the queue at {min(queues)}")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], "-v", *sys.argv[1:]])
