"""Remote execution and shakedown regressions, without engine launches."""
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import acceptance_remote as remote
import acceptance_identity
import test_menu_readback as readback
from test_inventory_oracle_evidence import extract_defects, run_split


class AcceptanceRuntimeEdges(unittest.TestCase):
    def test_shipped_inventory_suites_are_used_on_the_native_box(self):
        import importlib.util
        import os
        import run_tools_suites
        with tempfile.TemporaryDirectory() as folder, patch.dict(os.environ,CC_INVENTORY_DIR=folder):
            spec=importlib.util.spec_from_file_location('native_inventory_suite_fixture',run_tools_suites.__file__)
            module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
            self.assertEqual(module.INVENTORY,Path(folder))
            suite=next(argv for name,argv in module.SUITES if name=='inventory-run-split')
            self.assertEqual(Path(suite[0]).parent,Path(folder))

    def test_remote_build_receipt_failure_prevents_the_command(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            payload = root/'payload.json'
            payload.write_text(json.dumps(dict(kind='driver', repo=str(root), source_sha='a'*40,
                executable='never-executed', exe_sha256='b'*64, box='Z13', collection_id='unit',
                identity=str(root/'identity.json'), timeout=60, commands=[['never-executed']])))
            with patch.object(remote.subprocess, 'check_output', return_value='a'*40), \
                 patch.object(remote, 'sha256', return_value='b'*64), \
                 patch.object(acceptance_identity, 'identity', return_value=dict(status='INCOMPLETE', errors=['missing build receipt'])), \
                 patch.object(remote.subprocess, 'run', return_value=SimpleNamespace(returncode=0)) as launch:
                with self.assertRaisesRegex(ValueError, 'identity'):
                    remote.execute_payload(payload)
                launch.assert_not_called()

    def test_remote_numeric_exit_is_a_native_integer_in_both_receipts(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); (root/'S1').mkdir()
            share = SimpleNamespace(label='S1', box=SimpleNamespace(name='Z13'), ids=['case'])
            options = SimpleNamespace(out=root)
            results = {}
            run_split.finish_share(share, options, results, '1')
            terminal = json.loads((root/'S1/terminal.json').read_text())
            self.assertIs(type(terminal['exit_code']), int)
            self.assertIs(type(results['S1']), int)
            self.assertEqual(terminal['exit_code'], 1)

    def test_suite_not_applicable_count_is_not_a_failed_check(self):
        result = dict(status='PASS', counts=dict(passed=22, failed=0, not_applicable=10))
        self.assertTrue(extract_defects.file_verdict(result, 'result.json'))
        self.assertFalse(extract_defects.verdict_of(dict(not_applicable=True, required=True)))

    def test_optional_probe_does_not_call_a_compiling_box_absent_after_fifteen_seconds(self):
        import subprocess
        box = SimpleNamespace(name='Z13', hostname='EROL-TABLET', optional=True, local=False, ssh='z13')
        def response(argv, **kw):
            if kw['timeout'] < 51.3:
                raise subprocess.TimeoutExpired(argv, kw['timeout'])
            return SimpleNamespace(returncode=0, stdout='EROL-TABLET\n')
        with patch.object(run_split.subprocess, 'run', side_effect=response) as probe:
            self.assertEqual(run_split.probe_optional_boxes([box]), {'Z13': True})
            self.assertEqual(probe.call_count, 1)

    def test_readback_capture_payload_is_retained_without_bloating_verdicts(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            native = dict(case='landing', size='640x360', **{'pass':False}, error='native assertion',
                          captures=[dict(path='frame.png', controls=['x'*1000]*9000)])
            compact = getattr(readback, 'retain_capture_detail', lambda path, value: value)(root, native)
            self.assertLess(len(json.dumps(compact)), 10000)
            self.assertFalse(compact['pass'])
            self.assertEqual(compact['error'], 'native assertion')
            detail = compact['capture_detail']
            raw = root/detail['path']
            self.assertEqual(readback.sha(raw), detail['sha256'])
            self.assertEqual(json.loads(raw.read_text().splitlines()[0]), native['captures'][0])


if __name__ == '__main__':
    unittest.main()
