"""Refresh resolution keeps both POSIX routes and measured build receipts."""

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import cross_peers


class RefreshTests(unittest.TestCase):
    def helper(self, folder):
        script = Path(__file__).with_name('cross_refresh_boxes.sh').read_text()
        code = script.split("<<'PYEOF'\n", 1)[1].split('\nPYEOF', 1)[0]
        path = Path(folder) / 'refresh_helper.py'
        path.write_text(code)
        spec = importlib.util.spec_from_file_location('refresh_helper_test', path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def test_resolve_keeps_mac_and_linux_as_separate_builds(self):
        with tempfile.TemporaryDirectory() as folder:
            helper = self.helper(folder)
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                helper.resolve(str(cross_peers.HERE), 'refresh-selftest', '/tmp/mac-ready')
            text = output.getvalue()
            self.assertIn('BOX_COUNT=4', text)
            self.assertIn('MAC_SSH=Erol-Mac', text)
            self.assertIn('LINUX_SSH=3090', text)
            self.assertIn('LINUX_PYTHON=/usr/bin/python3', text)
            self.assertIn('LINUX_EXECUTABLE=/home/erol/cortex-workers/opus-run-cross-linux-20260928/repo/build-gcc/CortexCommand', text)

    def test_receipt_records_the_measured_artifact_hash(self):
        with tempfile.TemporaryDirectory() as folder:
            helper = self.helper(folder)
            root = Path(folder)
            artifact = root / 'artifact'
            artifact.write_bytes(b'first build')
            first = helper.digest(artifact)
            artifact.write_bytes(b'second build')
            measured = helper.digest(artifact)
            self.assertNotEqual(first, measured)
            with contextlib.redirect_stdout(io.StringIO()):
                helper.receipt(root / 'build.json', 'tip', measured, 'gcc', 'ninja.log', 'built here')
            self.assertEqual(json.loads((root / 'build.json').read_text())['executable_sha256'], measured)


if __name__ == '__main__':
    unittest.main()
