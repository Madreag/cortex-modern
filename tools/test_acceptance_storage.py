"""Retained output must stop before its cap, including final copy and report writes."""
from contextlib import ExitStack
import gzip
import io
from pathlib import Path
import tarfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import acceptance_runtime as runtime
import world_mod_cross as world

json_writer = runtime.write_json


class KeptBytes(io.BytesIO):
    def close(self):
        pass


def archive_bytes(name, data):
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode='w') as archive:
        member = tarfile.TarInfo(name)
        member.size = len(data)
        archive.addfile(member, io.BytesIO(data))
    return output.getvalue()


class RetainedStorage(unittest.TestCase):
    def extract(self, name, data, limit, rejected=True):
        original = archive_bytes(name, data)
        output = KeptBytes()
        root = Path('D:/mx/virtual-quota-fixture')
        with ExitStack() as stack:
            stack.enter_context(patch.object(runtime, 'retained_bytes', return_value=0))
            stack.enter_context(patch.object(Path, 'mkdir'))
            stack.enter_context(patch.object(Path, 'exists', return_value=False))
            stack.enter_context(patch.object(Path, 'open', return_value=output))
            stack.enter_context(patch.object(Path, 'stat', side_effect=lambda *a, **k: SimpleNamespace(st_size=len(output.getvalue()))))
            stack.enter_context(patch.object(gzip, 'open', side_effect=lambda *a, **k: gzip.GzipFile(fileobj=io.BytesIO(output.getvalue()), mode='rb')))
            metadata = stack.enter_context(patch.object(world, 'write_json'))
            with runtime.storage_scope(root, limit=limit) as budget:
                with tarfile.open(fileobj=io.BytesIO(original), mode='r:') as archive:
                    if rejected:
                        with self.assertRaises(runtime.StorageLimit):
                            world.extract_preserved(archive, root, compress_records=True)
                    else:
                        self.assertEqual(world.extract_preserved(archive, root, compress_records=True), 1)
                        self.assertEqual(budget.used, len(output.getvalue()))
            self.assertLess(len(output.getvalue()), limit)
            if rejected:
                metadata.assert_not_called()
            else:
                metadata.assert_called_once()
        self.assertEqual(original, archive_bytes(name, data))
        return output.getvalue()

    def test_final_plain_member_cannot_cross_the_cap(self):
        self.assertEqual(self.extract('native.bin', bytes(range(256)), 128), b'')

    def test_gzip_footer_is_admitted_before_it_is_written(self):
        data = bytes(range(256))*8
        encoded = KeptBytes()
        with gzip.GzipFile(filename='', fileobj=encoded, mode='wb', compresslevel=3, mtime=0) as stream:
            stream.write(data)
        complete = encoded.getvalue()
        retained = self.extract('native.jsonl', data, len(complete)-1)
        self.assertEqual(retained, complete[:-4])

    def test_within_budget_copy_remains_complete_and_lossless(self):
        data = b'{"native":true}\n'*100
        result = self.extract('native.jsonl', data, 4096, rejected=False)
        self.assertEqual(gzip.decompress(result), data)

    def test_report_is_refused_before_an_existing_file_is_truncated(self):
        root = Path('D:/mx/virtual-quota-fixture')
        with ExitStack() as stack:
            stack.enter_context(patch.object(runtime, 'retained_bytes', return_value=95))
            stack.enter_context(patch.object(Path, 'is_file', return_value=True))
            stack.enter_context(patch.object(Path, 'stat', return_value=SimpleNamespace(st_size=4)))
            text_write = stack.enter_context(patch.object(Path, 'write_text'))
            byte_write = stack.enter_context(patch.object(Path, 'write_bytes'))
            with runtime.storage_scope(root, limit=100):
                with self.assertRaises(runtime.StorageLimit):
                    json_writer(root/'result.json', dict(native_receipt='x'*100))
            text_write.assert_not_called()
            byte_write.assert_not_called()


if __name__ == '__main__':
    unittest.main(verbosity=2)
