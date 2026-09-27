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

    def peer(self,ticks,coalesced=()):
        """One peer's log: a sample for each (round, tick) and a coalesced line for each (tick, replaced)."""
        lines=[]
        for tick,replaced in coalesced:
            lines.append(f'[fullstate-coalesced] tick={tick} replaced={replaced} writing=0 waiting_bound=1')
        for round_id,tick in ticks:
            lines += [f'[fullstate-context] tick={tick} round={round_id} label=sample path=/run/capture-{tick}/sample',
                      f'[fullstate] tick={tick} hash=0123456789abcdef sections=header:0123456789abcdef,scene:0123456789abcdef round={round_id}',
                      f'[fullstate-scope] tick={tick} round={round_id} label=sample per_peer=graph.1']
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'stdout.log'
            path.write_text('\n'.join(lines)+'\n')
            return report.parse_fullstate([path])

    def test_a_coalesced_tick_is_expected_absent_for_its_peer(self):
        expected=[(1,60,'sample'),(1,120,'sample'),(1,180,'sample')]
        peers=dict(host=self.peer([(1,60),(1,180)],coalesced=[(180,120)]),client=self.peer([(1,60),(1,120),(1,180)]))
        result=report.compare_fullstate_histories(peers,expected)
        self.assertTrue(result['passed'],result)
        self.assertEqual(result['coalesced'],dict(host=1,client=0))
        self.assertEqual(result['coalesced_samples'],[dict(peer='host',key=(1,120,'sample'))])
        self.assertEqual(result['compared_samples'],2)

    def test_an_absence_without_a_coalesced_line_stays_a_miss(self):
        expected=[(1,60,'sample'),(1,120,'sample')]
        peers=dict(host=self.peer([(1,60)],coalesced=[(180,60)]),client=self.peer([(1,60),(1,120)]))
        result=report.compare_fullstate_histories(peers,expected)
        self.assertFalse(result['passed'])
        self.assertEqual(result['missing'],[dict(peer='host',key=(1,120,'sample'))])

    def test_one_coalesced_line_accounts_for_one_absence(self):
        expected=[(1,120,'sample'),(2,120,'sample'),(2,240,'sample')]
        peers=dict(host=self.peer([(2,240)],coalesced=[(240,120)]),client=self.peer([(1,120),(2,120),(2,240)]))
        result=report.compare_fullstate_histories(peers,expected)
        self.assertFalse(result['passed'])
        self.assertEqual(len(result['missing']),1)
        self.assertEqual(result['coalesced'],dict(host=1,client=0))

    def test_no_sample_both_peers_wrote_is_not_a_pass(self):
        expected=[(1,60,'sample'),(1,120,'sample')]
        peers=dict(host=self.peer([(1,60)],coalesced=[(180,120)]),client=self.peer([(1,120)],coalesced=[(120,60)]))
        result=report.compare_fullstate_histories(peers,expected)
        self.assertFalse(result['passed'])
        self.assertEqual(result['missing'],[])
        self.assertEqual(result['compared_samples'],0)

    def test_a_coalesced_sample_never_excuses_a_labelled_capture(self):
        expected=[(1,120,'canonical')]
        canonical='[fullstate-canonical] tick=120 hash=0123456789abcdef sections=header:0123456789abcdef round=1'
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'stdout.log'
            path.write_text(canonical+'\n[fullstate-scope] tick=120 round=1 label=canonical per_peer=\n'
                            '[fullstate-context] tick=120 round=1 label=canonical path=/run/c\n')
            client=report.parse_fullstate([path])
        peers=dict(host=self.peer([],coalesced=[(180,120)]),client=client)
        self.assertFalse(report.compare_fullstate_histories(peers,expected)['passed'])


if __name__=='__main__': unittest.main()
