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

    def test_a_labelled_capture_is_parsed_under_its_label(self):
        # l4p-42: '[fullstate-landed]' hash lines matched no pattern, so every post-return sample went unread.
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'stdout.log'
            path.write_text('[fullstate-context] tick=989 round=1 label=landed path=/run/process-1/round-1/capture-4/landed\n'
                '[fullstate-landed] tick=989 hash=0123456789abcdef sections=header:0123456789abcdef,scene:0123456789abcdef round=1\n'
                '[fullstate-scope] tick=989 round=1 label=landed per_peer=graph.1,camera\n')
            document=report.parse_fullstate([path])
        self.assertEqual([tuple(s['key']) for s in document['samples']],[(1,989,'landed')])
        self.assertTrue(document['samples'][0]['scope_valid'])

    def test_a_landed_sample_is_owed_by_every_playing_peer(self):
        # l4p-42: the host's post-return samples were never compared. A peer missing one fails; a peer held over that tick does not owe it.
        document=self.document()
        landed=copy.deepcopy(document); landed['samples'][0]['key']=[1,600,'landed']
        result=report.compare_fullstate_histories({'a':copy.deepcopy(landed),'b':copy.deepcopy(landed),'c':copy.deepcopy(document)},[(1,600,'landed')])
        self.assertFalse(result['passed']); self.assertEqual([m['peer'] for m in result['missing']],['c'])
        held=copy.deepcopy(document); held['holds']=[(1,500,700)]
        result=report.compare_fullstate_histories({'a':copy.deepcopy(landed),'b':copy.deepcopy(landed),'c':held},[(1,600,'landed')])
        self.assertEqual(result['missing'],[])

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
        """Every submitted capture has its own context, including the one replaced before writing."""
        lines=[]
        submissions = set(ticks)
        replacements = {}
        for tick, replaced in coalesced:
            round_id = next((r for r, t in reversed(ticks) if t <= tick), 1)
            submissions.update(((round_id, tick), (round_id, replaced)))
            replacements[round_id, tick] = replaced
        for capture, (round_id, tick) in enumerate(sorted(submissions), 1):
            lines.append(f'[fullstate-context] tick={tick} round={round_id} label=sample path=/run/process-1/round-{round_id}/capture-{capture}/sample')
            if (round_id, tick) in replacements:
                lines.append(f'[fullstate-coalesced] tick={tick} replaced={replacements[round_id, tick]} writing=0 waiting_bound=1')
            if (round_id, tick) in ticks:
                lines += [f'[fullstate] tick={tick} hash=0123456789abcdef sections=header:0123456789abcdef,scene:0123456789abcdef round={round_id}',
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

    def held_peer(self,ticks,hold,reclaim,round_id=1,peer=2):
        """A peer's log with its own hold of its seat at `hold` and its return at `reclaim`."""
        document=self.peer(ticks)
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'stdout.log'
            lines=[f'[net-lockstep] start round={round_id} frame=1 local_peer={peer} peers=2 input_delay=4',
                   f'[net-lockstep] hold of this seat at {hold} revision=10 incarnation=1 state=Running',
                   f'[net-match] private catch-up complete frame={reclaim} in_place=1 from={hold - 2}',
                   f'[net-match] seat-reclaimed peer={peer} frame={reclaim} live_actors=2']
            path.write_text('\n'.join(lines)+'\n')
            document['holds']=report.parse_fullstate([path])['holds']
        return document

    def test_a_held_peers_samples_inside_its_own_hold_are_expected_absent(self):
        expected=[(1,60,'sample'),(1,120,'sample'),(1,180,'sample'),(1,240,'sample')]
        peers=dict(host=self.peer([(1,60),(1,120),(1,180),(1,240)]),client=self.held_peer([(1,60),(1,240)],hold=102,reclaim=230))
        result=report.compare_fullstate_histories(peers,expected)
        self.assertTrue(result['passed'],result)
        self.assertEqual(result['held_samples'],[dict(peer='client',key=(1,120,'sample')),dict(peer='client',key=(1,180,'sample'))])
        self.assertEqual(result['compared_samples'],2)

    def test_a_hold_excuses_neither_another_peer_nor_a_tick_outside_it(self):
        expected=[(1,60,'sample'),(1,120,'sample'),(1,240,'sample')]
        peers=dict(host=self.peer([(1,60),(1,240)]),client=self.held_peer([(1,60)],hold=102,reclaim=230))
        result=report.compare_fullstate_histories(peers,expected)
        self.assertFalse(result['passed'])
        self.assertEqual(result['missing'],[dict(peer='host',key=(1,120,'sample')),dict(peer='client',key=(1,240,'sample'))])
        other=report.compare_fullstate_histories(dict(host=self.peer([(2,60),(2,240)]),client=self.held_peer([(2,60),(2,240)],hold=102,reclaim=230)),
                                                 [(2,60,'sample'),(2,120,'sample'),(2,240,'sample')])
        self.assertEqual(other['missing'],[dict(peer='host',key=(2,120,'sample')),dict(peer='client',key=(2,120,'sample'))],'the hold was in round 1')

    def test_a_hold_that_never_returns_excuses_nothing(self):
        document=self.held_peer([(1,60)],hold=102,reclaim=230)
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'stdout.log'
            path.write_text('[net-lockstep] start round=1 frame=1 local_peer=2 peers=2 input_delay=4\n'
                            '[net-lockstep] hold of this seat at 102 revision=10 incarnation=1 state=Running\n')
            document['holds']=report.parse_fullstate([path])['holds']
        self.assertEqual(document['holds'],[])

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
