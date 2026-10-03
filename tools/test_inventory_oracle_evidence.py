import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch


INVENTORY = Path('D:/Projects/reviews/takeover-20260909/grok-workers/lead-tools/inventory')
if INVENTORY.is_dir():
    sys.path.insert(0, str(INVENTORY))
    import extract_defects
    import run_split
    import run_stream
    import merge_defects


@unittest.skipUnless(INVENTORY.is_dir(), 'external inventory tools are absent')
class InventoryOracleEvidence(unittest.TestCase):
    def test_p23_parent_pass_cannot_suppress_required_child_failure(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'result.json').write_text(json.dumps(dict(passed=True, checks=dict(
                required_restore=dict(required=True, passed=False, reason='restored state differs')))))
            result = extract_defects.Extractor(root, True, None).run()
        self.assertGreater(result['hard_count'], 0, result)

    def test_p24_split_without_terminal_results_fails(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            box = SimpleNamespace(name='local', local=True)
            share = SimpleNamespace(label='S1', box=box, ids=['S1.unit'], whole=True, minutes=1, kept_here={})
            thread = Mock()
            with patch.object(run_split, 'load_manifest', return_value=([box], {})), \
                 patch.object(run_split, '_SplitContext', return_value=object()), \
                 patch.object(run_split.run_stream, 'build_streams', return_value={'S1': []}), \
                 patch.object(run_split.run_stream, 'exe_sha', return_value='a' * 64), \
                 patch.object(run_split, 'plan', return_value=([share], [])), \
                 patch.object(run_split, 'print_plan', return_value={}), patch.object(run_split, 'git', return_value='tip'), \
                 patch.object(run_split.threading, 'Thread', return_value=thread), contextlib.redirect_stdout(io.StringIO()):
                code = run_split.main(['--repo', str(Path(__file__).resolve().parents[1]), '--out', folder,
                    '--streams', 'S1', '--reference', folder, '--exe', 'a' * 64])
        self.assertNotEqual(code, 0, 'the scheduled S1 share had no terminal result')

    def test_p25_unreadable_required_result_is_incomplete(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'case').mkdir()
            (root / 'case/result.json').write_text('{"passed":')
            (root / 'progress.json').write_text(json.dumps(dict(stream='S1', state='done', commands=[
                dict(id='S1.case', driver='unit.py', dir=str(root / 'case'), state='done', exit_code=0)])))
            result = extract_defects.Extractor(root, True, None).run()
        self.assertTrue(result['hard_count'] or result.get('collection_complete') is False, result)

    def test_required_status_and_oversized_file_are_incomplete(self):
        for value in ('MISS', 'INCOMPLETE', 'UNKNOWN', 'FAIL', 'NOT COVERED'):
            with self.subTest(status=value), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                (root / 'result.json').write_text(json.dumps(dict(passed=True, checks=dict(x=dict(required=True, status=value)))))
                result = extract_defects.Extractor(root, True, None).run()
                self.assertGreater(result['hard_count'], 0, result)
                self.assertFalse(result['collection_complete'])
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'result.json').write_bytes(b' ' * (extract_defects.MAX_JSON_BYTES + 1))
            result = extract_defects.Extractor(root, True, None).run()
            self.assertFalse(result.get('collection_complete', True))

    def test_local_share_exception_has_terminal_failure(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            share = SimpleNamespace(label='S1', stream='S1', ids=['S1.unit'], box=SimpleNamespace(name='local'))
            options = SimpleNamespace(yield_to=[], repo=root, out=root)
            results = {}
            with patch.object(run_split, 'stream_argv', return_value=[]), patch.object(run_split.subprocess, 'call', side_effect=OSError('unit failure')):
                try:
                    run_split.run_local_queue([share], options, Mock(), results)
                except OSError:
                    pass
            self.assertIn('S1', results)
            self.assertNotEqual(results['S1'], 0)
            self.assertTrue((root / 'S1/terminal.json').is_file())

    def test_merge_missing_collection_receipt_cannot_pass(self):
        result = merge_defects.merge([('required', dict(defects=[], observations=[]))], [])
        self.assertFalse(result['collection_complete'], result)


if __name__ == '__main__':
    unittest.main()
