"""Headless launches cannot inherit the user's display driver or activate a Mac app."""
from __future__ import annotations

import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import posix_test_runner


class CaptureEnvironmentTest(unittest.TestCase):
    def construct(self, platform, extra=None, recording=False):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            with patch.object(posix_test_runner.sys, "platform", platform), \
                 patch.object(posix_test_runner, "hop_mode", return_value=False):
                args = ["dummy", "-headless", *(["-record-video", str(root / "video")] if recording else [])]
                run = posix_test_runner.IsolatedRun(args, root, root / "out", env=extra)
                try:
                    return dict(run.env), dict(run.record["env_set"])
                finally:
                    run.close()

    def test_inherited_autologin_display_cannot_select_x11(self):
        with patch.dict(os.environ, {"DISPLAY": ":0", "SDL_VIDEODRIVER": "x11", "CCCP_HEADLESS": "0"}):
            env, recorded = self.construct("linux")
        self.assertEqual(env["SDL_VIDEODRIVER"], "offscreen")
        self.assertEqual(recorded["SDL_VIDEODRIVER"], "offscreen")
        self.assertEqual(env["CCCP_HEADLESS"], "1")

    def test_direct_runner_caller_cannot_select_a_desktop_driver(self):
        env, recorded = self.construct("linux", {"SDL_VIDEODRIVER": "wayland"})
        self.assertEqual(env["SDL_VIDEODRIVER"], "offscreen")
        self.assertEqual(recorded["SDL_VIDEODRIVER"], "offscreen")

    def test_mac_direct_runner_uses_hidden_background_context(self):
        with patch.dict(os.environ, {"SDL_VIDEODRIVER": "x11", "SDL_MAC_BACKGROUND_APP": "0"}):
            env, recorded = self.construct("darwin", {"SDL_MAC_BACKGROUND_APP": "0"})
        self.assertEqual(env["SDL_VIDEODRIVER"], "cocoa")
        self.assertEqual(env["SDL_MAC_BACKGROUND_APP"], "1")
        self.assertEqual(recorded["SDL_MAC_BACKGROUND_APP"], "1")

    def test_recording_context_updates_do_not_wait_for_the_main_thread(self):
        env, recorded = self.construct("darwin", recording=True)
        self.assertEqual(env["SDL_MAC_OPENGL_ASYNC_DISPATCH"], "1")
        self.assertEqual(recorded["SDL_MAC_OPENGL_ASYNC_DISPATCH"], "1")

    def test_other_runs_keep_the_context_dispatch_policy(self):
        with patch.dict(os.environ, {"SDL_MAC_OPENGL_ASYNC_DISPATCH": "0"}):
            env, recorded = self.construct("darwin")
        self.assertEqual(env["SDL_MAC_OPENGL_ASYNC_DISPATCH"], "0")
        self.assertNotIn("SDL_MAC_OPENGL_ASYNC_DISPATCH", recorded)


if __name__ == "__main__":
    unittest.main()
