import copy
import tempfile
from pathlib import Path
import unittest
from feel import report


class FullStateTests(unittest.TestCase):
    def document(self):
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'stdout.log'
            path.write_text('[fullstate-context] tick=600 round=1 label=sample path=/run/process-1/round-1/capture-1/sample\n'
                '[fullstate] tick=600 hash=0123456789abcdef sections=header:0123456789abcdef,scene:0123456789abcdef round=1\n'
                '[fullstate-scope] tick=600 round=1 label=sample per_peer=graph.1,camera\n')
            return report.parse_fullstate([path])

    def test_equal_shared_samples_keep_exclusions(self):
        document=self.document()
        result=report.compare_fullstate_histories({p:copy.deepcopy(document) for p in 'abc'},[(1,600,'sample')])
        self.assertTrue(result['passed'])
        self.assertEqual(document['samples'][0]['scope']['per_peer'],['graph.1','camera'])

    def test_missing_promised_sample_fails(self):
        document=self.document()
        result=report.compare_fullstate_histories({p:copy.deepcopy(document) for p in 'abc'},[(1,600,'sample'),(1,1200,'sample')])
        self.assertFalse(result['passed']); self.assertEqual(len(result['missing']),3)

    def test_missing_scope_is_not_an_exclusion(self):
        document=self.document(); document['samples'][0]['scope_valid']=False
        self.assertFalse(report.compare_fullstate_histories({p:copy.deepcopy(document) for p in 'abc'},[(1,600,'sample')])['passed'])

    def test_earlier_divergent_capture_is_preserved(self):
        peers={p:self.document() for p in 'abc'}
        bad=copy.deepcopy(peers['c']['samples'][0]); bad['sections']['scene']='ffffffffffffffff'
        peers['c']['samples'].insert(0,bad)
        result=report.compare_fullstate_histories(peers,[(1,600,'sample')])
        self.assertFalse(result['passed']); self.assertEqual(result['differences'][0]['sections'],['scene'])


if __name__=='__main__': unittest.main()
