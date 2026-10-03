import json
import os
from pathlib import Path
import tempfile
import unittest

from acceptance_image_report import read_run, report
from acceptance_mod import sha256


class ImageReport(unittest.TestCase):
    def setUp(self):
        retained = os.environ.get("CC_ACCEPTANCE_TEST_ROOT")
        if retained:
            self.root = Path(tempfile.mkdtemp(prefix="image-report-test-", dir=retained))
        else:
            temporary = tempfile.TemporaryDirectory()
            self.addCleanup(temporary.cleanup)
            self.root = Path(temporary.name)
        self.digest = "e"*64
        self.run = self.root/"scene-00"
        self.scene = dict(module="Base.rte", name="Fixture Scene")
        self.dump = self.root/"native-lobby.json"
        self.write(self.dump, dict(activity_table=[dict(preset="Persistent World", module="Base.rte", scenes=[self.scene])]))
        self.listing = self.root/"scenes.json"
        self.write(self.listing, dict(source=str(self.dump), scenes=[self.scene], configuration="Final", executable_sha256=self.digest))
        for peer in ("host", "bootstrap", "client"):
            self.write(self.run/peer/"launch.json", dict(exe_sha256=self.digest, runner="win32_test_runner.py", headless_env="1",
                       private_desktop="fixture", exit_code=1 if peer == "client" else 0, timed_out=False))
        self.write(self.run/"host-report.json", dict(service=dict(runner=dict(match_config=dict(activity_preset="Persistent World",
                   persistent_world=True, scene_name=self.scene["name"], rules=dict(activity_module="Base.rte", scene_module="Base.rte"))))))
        self.archive = self.run/"host/runtime/Autosaves/world.ccsave"
        self.archive.parent.mkdir(parents=True)
        self.archive.write_bytes(b"archive-fixture")
        self.offer = dict(tick=300, digest=sha256(self.archive), path=str(self.archive), bytes=self.archive.stat().st_size)
        self.host_log = self.run/"host/stdout.log"
        self.host_log.write_text("[net-world] offer "+json.dumps(self.offer)+"\n", encoding="utf-8")
        (self.run/"client/stdout.log").write_text("[net-match] state transfer complete: 30 bytes\n", encoding="utf-8")

    def write(self, path, value):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value), encoding="utf-8")

    def test_real_byte_measurement_does_not_claim_join_or_label_success(self):
        result = report(self.listing, [self.run], self.root/"out")
        self.assertTrue(result["byte_table_complete"])
        self.assertEqual(result["completed_engine_runs"], 0)
        self.assertFalse(result["passed"])
        self.assertTrue(any("progress label receipts missing" in error for error in result["failures"]))
        self.assertIn("FAILED AFTER TRANSFER", (self.root/"out/image-sizes.md").read_text(encoding="utf-8"))

    def test_archive_mutation_is_rejected_even_if_summary_was_green(self):
        self.archive.write_bytes(b"changed-fixture")
        with self.assertRaises(ValueError):
            read_run(self.run, self.digest)

    def test_native_scene_and_native_lobby_must_agree(self):
        value = json.loads((self.run/"host-report.json").read_text())
        value["service"]["runner"]["match_config"]["scene_name"] = "Not offered"
        self.write(self.run/"host-report.json", value)
        with self.assertRaises(ValueError):
            report(self.listing, [self.run], self.root/"out")

    def test_missing_peer_or_different_binary_cannot_supply_release_receipt(self):
        path = self.run/"client/launch.json"
        value = json.loads(path.read_text())
        value["exe_sha256"] = "a"*64
        self.write(path, value)
        with self.assertRaises(ValueError):
            read_run(self.run, self.digest)

    def test_clocked_bytes_cannot_disagree_with_statechunk_receipt(self):
        self.write(self.run/"client/acceptance-transfer.json", dict(received_bytes=31))
        with self.assertRaises(ValueError):
            read_run(self.run, self.digest)


if __name__ == "__main__":
    unittest.main(verbosity=2)
