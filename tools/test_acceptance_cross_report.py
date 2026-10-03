import json
import os
from pathlib import Path
import tempfile
import unittest

from acceptance_cross_report import build_report, native_prerequisites


class CrossReceiptReport(unittest.TestCase):
    def native(self):
        native = dict(exit_code=0, setup_error="", runtime_error="",
                      desync_check=dict(mismatches=0, compares=40, compare_margin=1),
                      service=dict(runner=dict(match_config=dict(peer_count=4))))
        return native, dict(exit_code=0, timed_out=False, exe_sha256="e"*64), dict(executable_sha256="e"*64)

    def test_matching_live_history_cannot_erase_native_desync(self):
        native, record, preflight = self.native()
        self.assertEqual(native_prerequisites(native, record, "", preflight, 4), [])
        native["desync_check"]["mismatches"] = 1
        self.assertIn("native desync check missing or failed", native_prerequisites(native, record, "", preflight, 4))

    def test_wrong_loaded_binary_cannot_use_another_preflight(self):
        native, record, preflight = self.native()
        record["exe_sha256"] = "f"*64
        self.assertIn("running binary differs from or lacks its preflight identity",
                      native_prerequisites(native, record, "", preflight, 4))

    def test_engine_errors_survive_a_zero_exit_and_equal_hashes(self):
        native, record, preflight = self.native()
        result = native_prerequisites(native, record, "ready\n[net-ui-probe] FAIL input not applied\n", preflight, 4)
        self.assertIn("engine findings in stdout.log lines 2", result)

    def test_native_peer_count_is_not_inferred_from_the_manifest(self):
        native, record, preflight = self.native()
        native["service"]["runner"]["match_config"]["peer_count"] = 3
        self.assertIn("native adopted peer count differs from the row", native_prerequisites(native, record, "", preflight, 4))

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
