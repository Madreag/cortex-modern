"""Native sanitizer identity and shipped input bytes remain bound to their own receipts."""
import hashlib
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest

from acceptance_save_inputs import stage
from test_acceptance_resume import INVENTORY
if INVENTORY.is_dir():
    import run_stream


@unittest.skipUnless(INVENTORY.is_dir(), 'external inventory tools are absent')
class NativeInputs(unittest.TestCase):
    def test_sanitizer_identity_measures_its_own_executable(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); exe = root/'Cortex Command.exe'
            exe.write_bytes(b'synthetic clang_rt.asan bytes, never executed')
            receipt = root/'exe-ledger.jsonl'
            receipt.write_text(json.dumps(dict(head_full='a'*40, configuration='Final ASan', dirty_files=0,
                exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest()))+'\n')
            ctx = SimpleNamespace(head='a'*40, asan_receipt=receipt, box='EROL-PC', collection_id='unit')
            self.assertIsNone(run_stream.asan_identity(ctx, root/'first', root))
            identity = json.loads((root/'first/identity.json').read_text())
            self.assertEqual(identity['exe_sha256'], hashlib.sha256(exe.read_bytes()).hexdigest())
            exe.write_bytes(b'changed __asan_init bytes, never executed')
            self.assertIn('differs', run_stream.asan_identity(ctx, root/'changed', root))
            self.assertEqual(json.loads((root/'changed/identity.json').read_text())['status'], 'INCOMPLETE')

    def test_save_staging_preserves_exact_bytes_and_reports_absence(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); source = root/'input.ccsave'
            source.write_bytes(b'synthetic input bytes; not historical-save acceptance evidence')
            result = stage(root/'staged', source, None)
            self.assertFalse(result['passed'])
            self.assertEqual((root/'staged/original7.ccsave').read_bytes(), source.read_bytes())
            self.assertFalse((root/'staged/fork0920.ccsave').exists())
            self.assertEqual(result['inputs'][0]['sha256'], hashlib.sha256(source.read_bytes()).hexdigest())
            self.assertIn('not installed', result['inputs'][1]['reason'])


if __name__ == '__main__':
    unittest.main()
