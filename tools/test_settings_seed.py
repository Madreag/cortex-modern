"""The runners' Settings.ini overrides keep every property on its own line.

A property the runner first seeds empty ("SessionDirectoryUrl = ") and a scenario then sets was written onto the
next line, and that line's property was lost: the engine read an empty URL and no install key, and wrote both back
empty on exit (acceptance run 1: ui-surfaces, mp-direct-vs-relay, the relay compare).
"""
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent))

import posix_test_runner  # noqa: E402
import run_sim_test  # noqa: E402

SEEDED = "SettingsMan\n\tAutosaveSeconds = 0\n\tSessionDirectoryUrl = \n\tSessionDirectoryInstallKey = key0123456789abcd\n\tSessionDirectoryCertSha256 = \n\tNetworkPortMapEnable = 1\n"
VALUES = {"SessionDirectoryUrl": "127.0.0.1:49479", "SessionDirectoryCertSha256": "98022d8f998f4d93"}
EXPECTED = "SettingsMan\n\tAutosaveSeconds = 0\n\tSessionDirectoryUrl = 127.0.0.1:49479\n\tSessionDirectoryInstallKey = key0123456789abcd\n\tSessionDirectoryCertSha256 = 98022d8f998f4d93\n\tNetworkPortMapEnable = 1\n"


class SettingsSeedTest(unittest.TestCase):
    def test_windows_seed_keeps_an_empty_value_on_its_line(self):
        with tempfile.TemporaryDirectory() as root:
            (Path(root) / "Userdata").mkdir()
            ini = Path(root) / "Userdata" / "Settings.ini"
            ini.write_text(SEEDED, encoding="utf-8")
            run_sim_test.seed_settings(SimpleNamespace(cwd=root), VALUES)
            self.assertEqual(ini.read_text(encoding="utf-8"), EXPECTED)

    def test_posix_overrides_keep_an_empty_value_on_its_line(self):
        self.assertEqual(posix_test_runner.apply_settings_overrides(SEEDED, VALUES), EXPECTED)

    def test_no_runner_opens_a_port_on_the_network_router(self):
        # Every engine a driver launches starts with port mapping off; the port-map scenario turns it on for its fake gateway alone.
        self.assertEqual(run_sim_test.RUNTIME_SETTINGS.get("NetworkPortMapEnable"), "0")
        self.assertEqual(posix_test_runner.SETTINGS_OVERRIDES.get("NetworkPortMapEnable"), "0")

    def test_an_absent_property_is_appended_once(self):
        self.assertEqual(posix_test_runner.apply_settings_overrides("SettingsMan\n\tMuteMaster = 1\n", {"SkipIntro": "1"}),
                         "SettingsMan\n\tMuteMaster = 1\n\n\tSkipIntro = 1\n")


if __name__ == "__main__":
    unittest.main()
