"""The runner's CPU mask and per-engine memory limit come from the environment, then the box manifest, then the
defaults, and a launch record carries both. No engine starts: the stand-in child is this Python."""

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import win32_test_runner as runner

GIB = 1024 ** 3


class Limits(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.manifest = Path(self.scratch.name) / "boxes.json"
        self.manifest.write_text(json.dumps({"boxes": [
            {"name": "EROL-PC", "runner": {"affinity_mask": "0x0000FFFF", "engine_memory_gb": 12}},
            {"name": "EDITH", "runner": {"affinity_mask": None, "engine_memory_gb": 12}},
            {"name": "BARE"}]}), encoding="utf-8")

    def tearDown(self):
        self.scratch.cleanup()

    def limits(self, box, environ=None, physical=48 * GIB, system_mask=0xFFFFFFFF, manifest=None):
        return runner.box_runner_limits(environ or {}, manifest or self.manifest, box, physical, system_mask)

    def test_the_manifest_masks_erol_pc_and_sizes_each_engine(self):
        limits = self.limits("erol-pc")
        self.assertEqual(limits["affinity_mask"], "0x0000ffff")
        self.assertEqual(limits["affinity_source"], "manifest erol-pc")
        self.assertEqual(limits["engine_memory_limit_bytes"], 12 * GIB)

    def test_edith_and_unlisted_boxes_get_no_mask(self):
        self.assertIsNone(self.limits("EDITH")["affinity_mask"])
        bare = self.limits("BARE", physical=16 * GIB)
        self.assertIsNone(bare["affinity_mask"])
        self.assertEqual(bare["engine_memory_limit_bytes"], 8 * GIB)

    def test_a_32_gb_box_without_a_manifest_gets_12_gb_per_engine(self):
        limits = self.limits("EDITH", physical=34305019904, manifest=Path(self.scratch.name) / "absent.json")
        self.assertEqual(limits["engine_memory_limit_bytes"], 12 * GIB)
        self.assertIsNone(limits["affinity_mask"])
        self.assertIn("absent", limits["manifest"])

    def test_the_environment_wins(self):
        limits = self.limits("EROL-PC", {"CC_RUNNER_AFFINITY_MASK": "none", "CC_RUNNER_JOB_MEMORY_GB": "6"})
        self.assertIsNone(limits["affinity_mask"])
        self.assertEqual(limits["engine_memory_limit_bytes"], 6 * GIB)
        self.assertEqual(self.limits("BARE", {"CC_RUNNER_AFFINITY_MASK": "0xF0"})["affinity_mask"], "0x000000f0")

    def test_a_mask_outside_the_system_is_not_applied(self):
        limits = self.limits("EROL-PC", system_mask=0xFFFF0000)
        self.assertIsNone(limits["affinity_mask"])
        self.assertIn("not applied", limits["affinity_source"])
        self.assertEqual(self.limits("EROL-PC", system_mask=0xFF)["affinity_mask"], "0x000000ff")

    def test_a_launch_records_and_applies_both(self):
        root = Path(self.scratch.name)
        box = os.environ.get("COMPUTERNAME", "")
        self.manifest.write_text(json.dumps({"boxes": [{"name": box, "runner": {"affinity_mask": "0x3", "engine_memory_gb": 2}}]}),
                                 encoding="utf-8")
        # The box's own runner variables (a box's user environment or a run's payload) would outrank this manifest.
        environ = {key: value for key, value in os.environ.items() if not key.startswith("CC_RUNNER_")}
        with patch.dict(os.environ, {**environ, "CC_RUNNER_BOX_MANIFEST": str(self.manifest)}, clear=True):
            record = runner.run([sys.executable, "-c", "import time; time.sleep(0.2)"], root, root / "run", timeout=30,
                                startup_checks=False)
        launch = json.loads((root / "run" / "launch.json").read_text(encoding="utf-8"))
        self.assertEqual(record["exit_code"], 0)
        self.assertEqual(launch["affinity_mask"], "0x00000003")
        self.assertEqual(launch["process_affinity_after_assign"], "0x00000003")
        self.assertEqual(launch["engine_memory_limit_bytes"], 2 * GIB)
        self.assertEqual(launch["runner_limits"]["box"], box)


if __name__ == "__main__":
    unittest.main()
