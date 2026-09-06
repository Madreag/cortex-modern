"""Regression controls for repeated and missing snapshot properties."""

import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile


if __package__:
    from . import compare_snapshots as checker
else:
    import compare_snapshots as checker

CHECKER = Path(checker.__file__)
BASE = "Activity = GameActivity\n\tActivityState = 3\nScene = Scene\n\tPresetName = Shared\nLuaStateGraph = 0|first\nLuaStateGraph = 1|second\n"


class SnapshotComparisonTests(unittest.TestCase):
    def compare(self, first, second):
        with tempfile.TemporaryDirectory() as directory:
            paths = [Path(directory) / name for name in ("a.ccsave", "b.ccsave")]
            for path, text in zip(paths, (first, second)):
                with zipfile.ZipFile(path, "w") as archive:
                    archive.writestr("Save.ini", text)
            with patch.object(sys, "argv", [str(CHECKER), *map(str, paths)]), contextlib.redirect_stdout(io.StringIO()):
                return checker.main()

    def test_matching(self):
        self.assertEqual(self.compare(BASE, BASE), 0)

    def test_every_graph_is_compared(self):
        for value in (BASE.replace("0|first", "0|changed"), BASE.replace("LuaStateGraph = 0|first\n", "")):
            with self.subTest(value=value):
                self.assertEqual(self.compare(BASE, value), 1)

    def test_required_blocks(self):
        for value in ("", BASE.replace("Scene = Scene\n\tPresetName = Shared\n", ""), BASE + "Scene = Scene\n"):
            with self.subTest(value=value):
                self.assertEqual(self.compare(BASE, value), 1)

    def test_only_filename_metadata_is_normalized(self):
        original = "\tPresetName = a\nLuaStateGraph = abc\n\tDescription = a\n"
        expected = "\tPresetName = SNAPSHOT\nLuaStateGraph = abc\n\tDescription = a\n"
        self.assertEqual(checker.normalize_snapshot_name(original, "a"), expected)


if __name__ == "__main__":
    unittest.main()
