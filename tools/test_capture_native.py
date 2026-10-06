"""A native capture may only reuse the exact clean build it declares."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import capture_native
import e2e_video
import spread_peers


class NativeReceiptTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        self.git("init", "-q")
        (self.repo / "source.cpp").write_text("before\n")
        self.git("add", "source.cpp")
        self.git("-c", "user.name=Detector", "-c", "user.email=detector@example.invalid", "commit", "-qm", "Input")
        self.head = self.git("rev-parse", "HEAD").strip()
        self.exe = self.repo / "build/engine"
        self.exe.parent.mkdir()
        self.exe.write_bytes(b"native bytes")
        self.log = self.root / "build/build.log"
        self.log.parent.mkdir()
        self.log.write_bytes(b"compiled clean inputs\n")
        self.receipt = self.log.with_name("build.json")
        self.receipt.write_text(json.dumps({"commit": self.head, "build_exit_code": 0,
            "executable_sha256": hashlib.sha256(self.exe.read_bytes()).hexdigest(), "build_log": str(self.log),
            "build_log_sha256": hashlib.sha256(self.log.read_bytes()).hexdigest()}))

    def git(self, *arguments):
        return subprocess.check_output(["git", "-C", str(self.repo), *arguments], text=True)

    def verify(self):
        return capture_native.verify(self.repo, self.receipt, self.head, "build/engine")

    def test_exact_clean_build_is_accepted(self):
        self.assertTrue(self.verify()["verified"])

    def test_changed_source_is_refused(self):
        (self.repo / "source.cpp").write_text("uncommitted change\n")
        with self.assertRaisesRegex(RuntimeError, "uncommitted inputs"):
            self.verify()

    def test_wrong_commit_is_refused(self):
        with self.assertRaisesRegex(RuntimeError, "frozen case commit"):
            capture_native.verify(self.repo, self.receipt, "0" * 40, "build/engine")

    def test_changed_binary_is_refused(self):
        self.exe.write_bytes(b"other bytes")
        with self.assertRaisesRegex(RuntimeError, "executable differs"):
            self.verify()

    def test_changed_build_log_is_refused(self):
        self.log.write_bytes(b"failed build\n")
        with self.assertRaisesRegex(RuntimeError, "build log differs"):
            self.verify()

    def test_executable_outside_repo_is_refused(self):
        with self.assertRaisesRegex(RuntimeError, "escapes"):
            capture_native.verify(self.repo, self.receipt, self.head, "../other/engine")

    def test_reviewed_capture_does_not_require_windows(self):
        options = SimpleNamespace(spread=True, peer_boxes="host=Mac,seat2=Linux", size=None, capture_peer="host",
                                  port=48010, repo=self.repo, pool_dispatcher=None, pool_registry=None)
        definition = {"peers": [{"name": "host"}, {"name": "client"}]}
        scenario = {"size": "960x540", "timeout_s": 170}
        with patch.object(spread_peers, "run_case", return_value={"driver_result": {}}) as launch:
            e2e_video.run_one(options, scenario, definition, 0, self.root / "capture")
        peers = launch.call_args.args[2]
        self.assertEqual([peer.os for peer in peers], ["any", "any"])
        self.assertFalse(any(peer.reviewed for peer in peers))


if __name__ == "__main__":
    unittest.main()
