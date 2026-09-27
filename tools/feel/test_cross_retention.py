import gzip
import json
from pathlib import Path
import tempfile
import unittest
import zlib
from feel.records import presentation_records
import cross_report


class PresentationRetention(unittest.TestCase):
    def fixture(self, root):
        raw=b'{"type":"frame","frame":91}\n{"type":"end","frames":91}\n'
        (root/'raw.4.jsonl.gz').write_bytes(gzip.compress(raw))
        document=dict(complete=True,retained_chunks=16,chunk_bytes=8*1024*1024,dropped_lines=90,total_lines=92,
            parts=[dict(path='raw.4.jsonl.gz',bytes=len(raw),lines=2,crc32=zlib.crc32(raw),first_sequence=90,last_sequence=91)])
        path=root/'raw.index.json'; path.write_text(json.dumps(document)); return path,document

    def test_retained_window_keeps_original_sequences(self):
        with tempfile.TemporaryDirectory() as temporary:
            path,_=self.fixture(Path(temporary))
            self.assertEqual([r['type'] for r in presentation_records(path)],['frame','end'])

    def test_bad_crc_gap_and_escape_are_red(self):
        for change in ('crc32','first_sequence','path'):
            with self.subTest(change=change),tempfile.TemporaryDirectory() as temporary:
                path,document=self.fixture(Path(temporary))
                document['parts'][0][change]='../escape.gz' if change=='path' else 0
                path.write_text(json.dumps(document))
                with self.assertRaises(ValueError): list(presentation_records(path))

    def test_declared_window_cannot_exceed_bound(self):
        with tempfile.TemporaryDirectory() as temporary:
            path,document=self.fixture(Path(temporary)); document['retained_chunks']=0
            path.write_text(json.dumps(document))
            with self.assertRaises(ValueError): list(presentation_records(path))

    def test_native_policy_must_match_declared_window_and_normal_exit_closes_it(self):
        manifest=dict(storage=dict(presentation_chunk_bytes=1024,presentation_retained_chunks=2))
        document=dict(chunk_bytes=1024,retained_chunks=2,parts=[{},{}],complete=True)
        self.assertTrue(cross_report.presentation_contract(manifest,document,True))
        self.assertFalse(cross_report.presentation_contract(manifest,dict(document,chunk_bytes=2048),True))
        self.assertFalse(cross_report.presentation_contract(manifest,dict(document,complete=False),True))
        self.assertTrue(cross_report.presentation_contract(manifest,dict(document,complete=False),False))
        self.assertFalse(cross_report.presentation_contract(manifest,{},False))


if __name__=='__main__': unittest.main()
