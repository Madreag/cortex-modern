"""An unpacked package uses the Windows runner for single-player smoke only."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

import package_windows
import test_package_evidence as package_fixtures
import win32_test_runner as runner


class PackageRunnerScope(unittest.TestCase):
    def test_unpacked_network_arguments_are_refused_even_with_a_firewall_rule(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); package = root/'package'; package.mkdir()
            exe = package/'Cortex Command.exe'; exe.write_bytes(b'synthetic executable; never executed')
            (package/'MANIFEST.json').write_text('{}')
            runtime = root/'runtime'; (runtime/'Data/Base.rte').mkdir(parents=True); (runtime/'Userdata').mkdir()
            out = root/'out'; out.mkdir()
            run = object.__new__(runner.IsolatedRun)
            run.argv = [str(exe), '-net-session-selftest']; run.cwd = runtime; run.out = out
            run.record = dict(startup_checks=[]); run._save = Mock()
            with patch.object(runner, 'firewall_allows_inbound', return_value=True):
                with self.assertRaisesRegex(runner.StartupCheckError, 'package.*single.player'):
                    run._startup_checks()

    def test_package_verification_requires_the_actual_runner_receipt(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); package_fixtures.PackageEvidence().fixture(root)
            capture = root/'smoke/capture.json'; data = json.loads(capture.read_text())
            for run in data['runs']:
                for peer in run['peers']: peer['record']['runner'] = 'bare process'
            capture.write_text(json.dumps(data))
            self.assertFalse(package_windows.verify_evidence(root)['passed'])


if __name__ == '__main__':
    unittest.main()
