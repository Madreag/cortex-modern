import json
import os
from pathlib import Path
import tempfile
import unittest

from acceptance_cross_report import build_report


class CrossReceiptReport(unittest.TestCase):
    def test_empty_run_cannot_be_green_from_its_plan(self):
        retained = os.environ.get("CC_ACCEPTANCE_TEST_ROOT")
        if retained:
            root = Path(tempfile.mkdtemp(prefix="cross-report-test-", dir=retained))
        else:
            temporary = tempfile.TemporaryDirectory()
            self.addCleanup(temporary.cleanup)
            root = Path(temporary.name)
        names = ("erol", "edith", "mac", "linux")
        manifest = dict(acceptance_row="mod-match", ticks=1201, preflights={}, driver_findings=[],
                        boxes=[dict(name=n, kind="windows-local" if n == "erol" else "posix-ssh") for n in names],
                        specs=[dict(peer=n, box=n, own=str(root/n/"incarnation-0"), root=str(root)) for n in names])
        (root/"manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        result = build_report(root)
        self.assertFalse(result["v1_passed"])
        self.assertIn("preflight: distinct real machine receipts missing", result["failures"])
        self.assertIn("pc: native match outcome missing or failed", result["failures"])
        self.assertTrue((root/"acceptance-facts.json").is_file())


if __name__ == "__main__":
    unittest.main(verbosity=2)
