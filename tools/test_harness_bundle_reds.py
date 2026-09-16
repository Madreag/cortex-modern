"""One detecting unit test per named harness-bundle RED.

Each case asserts the changed behaviour and records what the base tree did.
"""
from __future__ import annotations

import importlib
import io
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE / "a7"))
sys.path.insert(0, str(HERE / "contracts"))

from generate_observer import DEFAULT_INVENTORY, hand_fields, main as generate_main, parse_args
from run_a7_group import main as a7_main
from run_selftests import engine_executable
from test_match_overlay import probe_script


class UiProbeRouting(unittest.TestCase):
    def test_both_call_sites_share_compare_sim_traces(self):
        """Base tree routed the UI-probe replay through the strict comparer."""
        text = (HERE / "run_interp_e2e.ps1").read_text(encoding="utf-8")
        sites = [line.strip() for line in text.splitlines() if "python $rowCompare" in line]
        self.assertEqual(len(sites), 2)
        self.assertIn('$rowCompare = Join-Path $repo "tools\\compare_sim_traces.py"', text)
        self.assertIn('$rowCompareArgs = @("--min-ticks", "1")', text)
        self.assertNotIn("compare_e2e_simgated_active.py", text)


class WoundIdCapture(unittest.TestCase):
    def test_wound_id_is_captured_before_removal(self):
        """Base tree printed wound.UniqueID after RemoveWounds deleted the emitter."""
        text = (HERE / "fixtures" / "sound_query_gate.lua").read_text(encoding="utf-8")
        wounds = text[text.index('mode == "wounds"'):text.index('elseif mode == "rng"')]
        self.assertLess(wounds.index("local woundId = wound.UniqueID"), wounds.index("arm:RemoveWounds(1)"))
        self.assertIn("woundId", wounds[wounds.index("arm:RemoveWounds(1)"):])


class WireRefusalDelay(unittest.TestCase):
    def test_wire_refusal_places_at_the_rendezvous(self):
        """Base tree used the client hold delay for wire-refusal and missed the 120-tick editor cap."""
        text = (HERE / "net_activity_launch.py").read_text(encoding="utf-8")
        self.assertIn('if options.variant == "wire-refusal":\n                    delay = 0', text)


class GenerateObserverRepo(unittest.TestCase):
    def test_repo_is_required(self):
        """Base tree had no generate_observer in this worktree and no --repo switch."""
        with self.assertRaises(SystemExit):
            parse_args([])
        options = parse_args(["--repo", str(REPO)])
        self.assertEqual(options.repo, Path(str(REPO)))

    def test_import_does_not_overwrite_the_header(self):
        header = REPO / "Source" / "System" / "ContractAudit.h"
        before = header.read_bytes()
        importlib.reload(sys.modules["generate_observer"])
        self.assertEqual(header.read_bytes(), before)

    def test_refuses_to_drop_hand_fields(self):
        """ENGINE 94 is a separate lane; dropped Field/Visit lines must abort the write."""
        import hashlib
        import json

        existing = 'void Visit(const Actor::DeferredWaypoint& object, const std::string& path) {\nField(path + ".ACraft.m_OffWireHatchTick", object.m_OffWireHatchTick);\n'
        generated = 'void Visit(const Actor& object, const std::string& path) {\n'
        self.assertTrue(hand_fields(existing) - hand_fields(generated))
        inventory = json.loads(Path(DEFAULT_INVENTORY).read_text(encoding="utf-8"))
        with tempfile.TemporaryDirectory() as tmp:
            tree = Path(tmp)
            for data in inventory["classes"].values():
                src = REPO / data["path"]
                if not src.is_file():
                    continue
                dest = tree / data["path"]
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(src.read_bytes())
            dest_header = tree / "Source" / "System" / "ContractAudit.h"
            dest_header.parent.mkdir(parents=True, exist_ok=True)
            dest_header.write_text(existing, encoding="utf-8")
            inventory["header_hashes"] = {
                rel: hashlib.sha256((tree / rel).read_bytes()).hexdigest()
                for rel in inventory.get("header_hashes", {})
                if (tree / rel).is_file()
            }
            inv_path = tree / "native-fields.json"
            inv_path.write_text(json.dumps(inventory), encoding="utf-8")
            before = {path: path.read_bytes() for path in tree.rglob("*") if path.is_file()}
            buf = io.StringIO()
            with redirect_stdout(buf):
                code = generate_main(["--repo", str(tree), "--inventory", str(inv_path)])
            self.assertEqual(code, 1)
            self.assertIn("refusing overwrite: generated header drops hand fields", buf.getvalue())
            after = {path: path.read_bytes() for path in tree.rglob("*") if path.is_file()}
            self.assertEqual(before, after)


class PeerReportFlags(unittest.TestCase):
    def test_heal_autosave_and_e2e_pass_peer_report_flags(self):
        """Base tree compared snapshots without --peer-report-a/-b and --cross-process."""
        heal = (HERE / "heal_driver" / "recovery_e2e.py").read_text(encoding="utf-8")
        autosave = (HERE / "test_autosave.py").read_text(encoding="utf-8")
        e2e = (HERE / "run_interp_e2e.ps1").read_text(encoding="utf-8")
        self.assertNotIn("D:/Projects/control-build", heal)
        self.assertIn("parents[2]", heal)
        self.assertIn("comparer missing:", heal)
        for text in (heal, autosave, e2e):
            self.assertIn("--peer-report-a", text)
            self.assertIn("--peer-report-b", text)
            self.assertIn("--cross-process", text)
        self.assertIn('peer checkpoint compare failed: {first_fail}', autosave)

    def test_missing_comparer_records_a_failed_snapshot_check(self):
        """Base tree skipped snapshots_sim_identical when the comparer path was absent."""
        sys.path.insert(0, str(HERE / "heal_driver"))
        import recovery_e2e
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            copied = [out / "p5snap_p1.ccsave", out / "p5snap_p2.ccsave"]
            for path in copied:
                path.write_bytes(b"x")
            checks = []

            def check(name, ok, detail, evidence):
                checks.append({"name": name, "status": "pass" if ok else "fail",
                               "detail": detail, "evidence": evidence})

            with patch.object(recovery_e2e, "SNAPSHOT_COMPARE", out / "missing_compare.py"):
                recovery_e2e.record_snapshot_compare(out, copied, out / "a.json", out / "b.json", check)
        row = next(item for item in checks if item["name"] == "snapshots_sim_identical")
        self.assertEqual(row["status"], "fail")
        self.assertIn("comparer missing:", row["detail"])


class OverlayToastHoldBanner(unittest.TestCase):
    def test_toast_hold_banner_carries_widget_and_banners(self):
        """Base tree used expected_visible None on auto/always toast-hold-banner shots."""
        for mode in ("auto", "always"):
            steps = probe_script("Host", (1920, 1080), {"leave": True}, mode)
            shots = [step for step in steps
                     if step.get("op") == "screenshot" and "toast-hold-banner" in step.get("name", "")]
            self.assertTrue(shots)
            for shot in shots:
                self.assertIsNotNone(shot.get("widget"))
                self.assertTrue(shot.get("banners"))


class FeelMeasureOutStamp(unittest.TestCase):
    def test_out_is_required_and_stamp_is_mst(self):
        """Base tree used a hardcoded scratch root and stamped with a date(1) shell-out."""
        import feel_measure
        self.assertRegex(feel_measure.stamp(), r"^\d{4}-\d{2}-\d{2} \d{2}:\d{2} MST$")
        text = (HERE / "feel_measure.py").read_text(encoding="utf-8")
        self.assertIn("datetime.now(MST)", text)
        self.assertNotIn("value-observations", text)
        self.assertNotIn("stage2/feel-measurement", text)
        buf = io.StringIO()
        with patch("sys.stderr", buf):
            with self.assertRaises(SystemExit):
                feel_measure.parse_args([])
        self.assertIn("required: --out", buf.getvalue())


class PosixSelftestBinary(unittest.TestCase):
    def test_windows_and_posix_engine_names(self):
        """Base tree always hashed Cortex Command.exe, which is not the Mac binary name."""
        repo = Path("/repo")
        env = {key: value for key, value in os.environ.items() if key != "CCCP_TEST_BINARY"}
        with patch.dict(os.environ, env, clear=True):
            with patch("run_selftests.sys.platform", "win32"):
                self.assertEqual(engine_executable(repo).resolve(), (repo / "Cortex Command.exe").resolve())
            with patch("run_selftests.sys.platform", "darwin"):
                self.assertEqual(engine_executable(repo).resolve(),
                                 (repo.resolve() / "build-gns" / "CortexCommand"))


class SequentialA7Arms(unittest.TestCase):
    def test_group_runs_one_arm_then_returns_nonzero(self):
        """Base tree had no group runner; the first delivery always returned 0."""
        with tempfile.TemporaryDirectory() as tmp:
            harness = Path(tmp) / "harness"
            harness.mkdir()
            (harness / "run_a7_mac.py").write_text("print('ok')\n", encoding="utf-8")
            out = Path(tmp) / "out"
            order = []

            def fake_run(command, cwd=None, env=None):
                order.append(command[-1])
                return SimpleNamespace(returncode=0 if command[-1] == "first" else 1)

            with patch("run_a7_group.subprocess.run", side_effect=fake_run):
                code = a7_main([
                    "--harness", str(harness), "--repo", str(tmp), "--manifest", str(tmp),
                    "--build-root", str(tmp), "--out", str(out), "first", "second",
                ])
            self.assertEqual(order, ["first", "second"])
            self.assertEqual(code, 1)


if __name__ == "__main__":
    unittest.main()
