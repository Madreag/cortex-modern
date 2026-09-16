"""Detect toast-hold-banner shots that omitted the widget expectation.

Base tree passed expected_visible None with empty checks, so auto/always
toast-hold-banner shots never judged the widget or the two banner rows.
"""
import tempfile
import unittest
from pathlib import Path

from PIL import Image

from test_match_overlay import probe_script, probe_shot_oracle


class OverlayExpectations(unittest.TestCase):
    def test_missing_widget_expectation_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "probe.png"
            Image.new("RGB", (1920, 1080), (0, 0, 0)).save(path)
            result = probe_shot_oracle(path, (1920, 1080), None, None)
            self.assertFalse(result["pass"])
            self.assertEqual(result["reason"], "missing widget expectation")
            self.assertEqual(result["checks"], {})

    def test_toast_hold_banner_steps_carry_widget_and_banners(self):
        """Base tree used shot(..., None) on auto/always toast-hold-banner steps."""
        for mode in ("auto", "always"):
            steps = probe_script("Host", (1920, 1080), {"leave": True}, mode)["steps"]
            shots = [step for step in steps
                     if step.get("op") == "screenshot" and "toast-hold-banner" in step.get("name", "")]
            self.assertTrue(shots, f"toast-hold-banner shot missing for mode={mode}")
            for shot in shots:
                self.assertIsNotNone(shot.get("widget"), "widget")
                self.assertTrue(shot.get("banners"), "banners")


if __name__ == "__main__":
    unittest.main()
