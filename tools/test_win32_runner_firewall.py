"""A box whose firewall is off needs no inbound allow rule; a box whose firewall is on still does."""
import sys
import unittest
from pathlib import Path
from unittest.mock import MagicMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
if sys.platform == 'win32':
    import win32_test_runner as runner
else:
    import ctypes
    with patch.object(ctypes, 'WinDLL', create=True, return_value=MagicMock()):
        import win32_test_runner as runner

EXE = r"D:\nowhere\inventory-build\Cortex Command.exe"


class FakeKey:
    def __init__(self, values):
        self.values = values

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


class FakeWinreg:
    """A registry with the three profile keys, optional policy keys and no rules for EXE."""

    HKEY_LOCAL_MACHINE = object()

    def __init__(self, profiles, policies=None):
        self.keys = {}
        for path, value in zip(runner.FIREWALL_PROFILE_KEYS, profiles):
            self.keys[path] = {"EnableFirewall": value}
        for path, value in (policies or {}).items():
            self.keys[path] = {"EnableFirewall": value}
        self.keys[runner.FIREWALL_RULES_KEY] = {}

    def OpenKey(self, _root, path):
        if path not in self.keys:
            raise OSError(2, "missing key", path)
        return FakeKey(self.keys[path])

    def QueryValueEx(self, key, name):
        if name not in key.values:
            raise OSError(2, "missing value", name)
        return key.values[name], 4

    def EnumValue(self, key, index):
        raise OSError(259, "no more data")


def with_registry(fake):
    return patch.dict(sys.modules, {"winreg": fake})


class FirewallOff(unittest.TestCase):
    def test_every_profile_off_needs_no_rule(self):
        with with_registry(FakeWinreg([0, 0, 0])):
            self.assertIs(runner.firewall_disabled_everywhere(), True)
            self.assertIs(runner.firewall_allows_inbound(EXE), True)

    def test_a_policy_value_wins_over_the_profile(self):
        policies = {path: 0 for path in runner.FIREWALL_POLICY_KEYS}
        with with_registry(FakeWinreg([1, 1, 1], policies)):
            self.assertIs(runner.firewall_disabled_everywhere(), True)
            self.assertIs(runner.firewall_allows_inbound(EXE), True)

    def test_one_profile_on_still_wants_the_rule(self):
        with with_registry(FakeWinreg([0, 1, 0])):
            self.assertIs(runner.firewall_disabled_everywhere(), False)
            self.assertIs(runner.firewall_allows_inbound(EXE), False)

    def test_unreadable_profiles_fall_back_to_the_rule_store(self):
        fake = FakeWinreg([0, 0, 0])
        del fake.keys[runner.FIREWALL_PROFILE_KEYS[1]]
        with with_registry(fake):
            self.assertIsNone(runner.firewall_disabled_everywhere())
            self.assertIs(runner.firewall_allows_inbound(EXE), False)


if __name__ == "__main__":
    unittest.main()
