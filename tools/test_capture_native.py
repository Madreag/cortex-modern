"""A native capture may only reuse the exact clean build it declares."""
import hashlib
import json
import os
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
        options = SimpleNamespace(spread=False, peer_boxes="host=Mac,seat2=Linux", size=None, capture_peer="host",
                                  port=e2e_video.PORT_LO + 10, repo=self.repo, pool_dispatcher=None, pool_registry=None)
        definition = {"peers": [{"name": "host"}, {"name": "client"}]}
        scenario = {"size": "960x540", "timeout_s": 170}
        with patch.object(spread_peers, "run_case", return_value={"driver_result": {}}) as launch:
            e2e_video.run_one(options, scenario, definition, 0, self.root / "capture")
        peers = launch.call_args.args[2]
        self.assertEqual([peer.os for peer in peers], ["any", "any"])
        self.assertEqual([peer.reviewed for peer in peers], [True, False])
        self.assertTrue(all(peer.readback and not peer.share_ok for peer in peers))


class CaptureTransportTests(unittest.TestCase):
    def test_without_mapping_keeps_shared_transport_untouched(self):
        with patch.dict(os.environ, {"CORTEX_CAPTURE_NATIVE_BUILDS": ""}), \
             patch.object(spread_peers, "installed_pool") as installed:
            with capture_native.reuse_native_builds(spread_peers):
                pass
        installed.assert_not_called()

    def test_verified_reuse_preserves_guards_and_restores_factory(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            mapping = root / "builds.json"
            mapping.write_text(json.dumps({"Linux": {"repo": "/caller/repo", "receipt": "/caller/build/build.json"}}))
            verified = {"repo": "/caller/repo", "exe": "/caller/repo/build-gcc/CortexCommand",
                        "commit": "frozen", "executable_sha256": "exact", "verified": True}

            class SharedTransport:
                def __init__(self, **kwargs):
                    self.sources = {"pool_worker.py": "unchanged native capacity and ownership guards"}
                    self.control_id = "original"
                    self.work = root / "work"
                    self.calls = []

                def native_build(self, box, claim, request):
                    self.calls.append(("compile", box["name"]))

                def rpc(self, box, action, values, timeout):
                    self.calls.append((action, box["name"]))

                def guarded_run(self, command, timeout):
                    self.calls.append(("verify", command))
                    return json.dumps(verified).encode()

                def guard(self):
                    return "original guard"

            module = SimpleNamespace(Transport=SharedTransport)
            box = {"name": "Linux", "python": "python3", "ssh": "named-linux", "exe": "build-gcc/CortexCommand"}
            claim = {"control": "/control", "head": "frozen"}
            with patch.dict(os.environ, {"CORTEX_CAPTURE_NATIVE_BUILDS": str(mapping)}), \
                 patch.object(spread_peers, "installed_pool", return_value=(None, module, None)):
                with capture_native.reuse_native_builds(spread_peers, registry="named-catalog"):
                    backend = module.Transport()
                    factory = module.Transport
                    with capture_native.reuse_native_builds(spread_peers):
                        self.assertIs(module.Transport, factory)
                    self.assertIs(backend.guard.__func__, SharedTransport.guard)
                    self.assertEqual(backend.sources["pool_worker.py"], "unchanged native capacity and ownership guards")
                    self.assertEqual(backend.sources["capture_native.py"], Path(capture_native.__file__).read_text())
                    backend.native_build(box, claim, {"run_id": "capture"})
                    self.assertEqual(claim["native_build"], verified)
                    self.assertEqual(backend.calls[0], ("renew", "Linux"))
                    self.assertEqual(backend.calls[1][0], "verify")
                    self.assertEqual(json.loads((backend.work / "capture-native-build.json").read_text()), verified)
                    backend.native_build({"name": "Other"}, {}, {})
                    self.assertEqual(backend.calls[-1], ("compile", "Other"))
                self.assertIs(module.Transport, SharedTransport)

    def test_failure_restores_shared_factory(self):
        module = SimpleNamespace(Transport=type("SharedTransport", (), {}))
        original = module.Transport
        with patch.dict(os.environ, {"CORTEX_CAPTURE_NATIVE_BUILDS": "caller-map"}), \
             patch.object(spread_peers, "installed_pool", return_value=(None, module, None)):
            with self.assertRaisesRegex(RuntimeError, "case failure"):
                with capture_native.reuse_native_builds(spread_peers):
                    raise RuntimeError("case failure")
        self.assertIs(module.Transport, original)


if __name__ == "__main__":
    unittest.main()
