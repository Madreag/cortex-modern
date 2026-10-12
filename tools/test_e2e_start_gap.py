"""A one-box start gap launches each later player after the first, as the two-box runner does; no gap launches them together."""

import json
from pathlib import Path
import tempfile
import time
from types import SimpleNamespace
import unittest

import e2e_video as video


class FakeRun:
    """Stands in for a runner handle: records when the player started and finishes at once."""

    starts = {}

    def __init__(self, repo, args, out, timeout, env=None, **kwargs):
        self.argv, self.out, self.cwd = ["engine", *map(str, args)], Path(out), Path(out) / "runtime"
        (self.cwd / "Userdata").mkdir(parents=True)
        (self.cwd / "Userdata/Settings.ini").write_text("\tEnableVSync = 0\n", encoding="utf-8")
        self.record = {}

    def start(self):
        FakeRun.starts[self.out.name] = time.monotonic()
        return self

    def finish(self):
        return {"exit_code": 0}

    def close(self):
        pass


class StartGapTest(unittest.TestCase):
    def run_scene(self, gap):
        FakeRun.starts = {}
        root = Path(self.temporary.name) / f"gap-{gap}"
        scenario = {"name": "gap", "path": str(root / "gap.json"), "checklist": [],
                    "peers": [{"name": "host", "settings": {}}, {"name": "joiner", "settings": {}}]}
        options = SimpleNamespace(repo=Path(video.__file__).resolve().parents[1], size=None, port=video.PORT_LO, fps=30,
                                  setting=[], render_cap=60, headed=False, start_gap=gap, fullstate_every=0, tokens={},
                                  scratch_root=Path(self.temporary.name), remote_capture=SimpleNamespace(make_run=FakeRun))
        video._run_one(options, scenario, {"name": "run0", "peers": scenario["peers"]}, 0, root)
        return FakeRun.starts["joiner"] - FakeRun.starts["host"]

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)

    def test_players_start_together_without_a_gap(self):
        self.assertLess(self.run_scene(0), 0.5)

    def test_a_later_player_waits_for_the_gap(self):
        self.assertGreaterEqual(self.run_scene(1.5), 1.5)


if __name__ == "__main__":
    unittest.main()
