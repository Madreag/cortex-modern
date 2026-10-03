"""Counterexamples for the three-machine spectator timing proof."""
from copy import deepcopy
import json
from pathlib import Path
import unittest
from unittest.mock import patch

from acceptance_clock_brackets import PLACEMENT, SEATED, collect_brackets
from acceptance_rows import judge
from test_acceptance_rows import good, hashes, peer


def bracketed():
    value=good('spectator')
    value.update(seated=list(SEATED),spectator='spectator',clock_box=None)
    value['peers']={name:{**peer(1,2401,PLACEMENT[name].lower()),
                         'first_wall_ms':10_000_000*(index+1), 'last_wall_ms':10_000_000*(index+1)+40_000}
                    for index,name in enumerate(SEATED)}
    value['peers']['spectator']=dict(box='linux',completed=True)
    value['throttle'].update(process='spectator',pid=200,first_tick=6000,last_tick=6059,closed=True)
    value['watch'].update(first=1201,last=6061,hashes=hashes(1201,6061))
    value['promotion'].update(activation_tick=8000,input_created_tick=8001,input_tick=8002)
    proof=dict(method='native-probe-brackets-v1',placement=deepcopy(PLACEMENT),events=[],
               native_cost=deepcopy(value['throttle']),
               armed_dump=dict(process='spectator',pid=200,sim_tick=5900),
               cost_dump=dict(process='spectator',pid=200,sim_cost=deepcopy(value['throttle'])),peers={})
    for index,name in enumerate(SEATED):
        proof['peers'][name]=dict(begin=dict(process=name,pid=100+index,sim_tick=2),
                                 end=dict(process=name,pid=100+index,sim_tick=2400))
    order=[('crawl-start',name) for name in SEATED]+[('crawl-armed','spectator'),('crawl-closed','spectator')]+[('crawl-end',name) for name in SEATED]
    proof['events']=[dict(sequence=index+1,kind=kind,peer=name,sha256='a'*64,**({'signal_sha256':'b'*64} if kind=='crawl-closed' else {}))
                     for index,(kind,name) in enumerate(order)]
    value['clock_brackets']=proof
    return value


class NativeClockBrackets(unittest.TestCase):
    def test_collector_binds_each_ordered_marker_to_its_retained_native_bytes(self):
        fixture=bracketed(); proof=fixture['clock_brackets']
        def read(path,**kwargs):
            if path.name=='clock-order.json': return json.dumps(proof['events'])
            if path.name=='crawl-armed.ownership.json': return json.dumps(proof['armed_dump'])
            if path.name=='crawl-complete.ownership.json': return json.dumps(proof['cost_dump'])
            name=path.parent.parent.name.removesuffix('-stage')
            return json.dumps(proof['peers'][name]['begin' if path.name.startswith('crawl-start') else 'end'])
        def digest(path): return 'b'*64 if path.name=='crawl-complete.json' else 'a'*64
        with patch.object(Path,'read_text',read), patch('acceptance_clock_brackets.file_digest',side_effect=digest):
            result=collect_brackets('/virtual',dict(placement=PLACEMENT),fixture['throttle'])
        self.assertEqual(result['peers'],proof['peers'])
        with patch.object(Path,'read_text',read), patch('acceptance_clock_brackets.file_digest',return_value='c'*64):
            with self.assertRaisesRegex(ValueError,'differs'):
                collect_brackets('/virtual',dict(placement=PLACEMENT),fixture['throttle'])

    def test_independent_native_clocks_pass_only_with_the_complete_ordered_proof(self):
        result=judge('spectator',bracketed())
        self.assertTrue(result['passed'],result['failures'])

    def test_missing_or_reordered_native_probe_is_red(self):
        for change in ('missing','sequence','early-end','duplicate'):
            with self.subTest(change=change):
                value=bracketed(); events=value['clock_brackets']['events']
                if change=='missing': events.pop()
                elif change=='sequence': events[0]['sequence']=2
                elif change=='early-end': events[0],events[5]=events[5],events[0]
                else: events[-1]=deepcopy(events[-2])
                self.assertFalse(judge('spectator',value)['passed'])

    def test_late_acknowledgement_or_different_process_is_red(self):
        for change in ('late','pid','clock-box','unclosed','cost-copy'):
            with self.subTest(change=change):
                value=bracketed(); proof=value['clock_brackets']
                if change=='late': proof['armed_dump']['sim_tick']=6000
                elif change=='pid': proof['peers']['seated-one']['end']['pid']=999
                elif change=='clock-box': value['peers']['seated-one']['box']='linux'
                elif change=='unclosed': value['throttle']['closed']=False
                else: proof['native_cost']['last_tick']=6001
                self.assertFalse(judge('spectator',value)['passed'])

    def test_brackets_cannot_omit_the_enclosing_tick_or_mask_a_slow_seat(self):
        for change in ('short','boundary','wait','hold','rate','digest'):
            with self.subTest(change=change):
                value=bracketed(); seat=value['peers']['seated-two']
                if change=='short': seat['last_wall_ms']=seat['first_wall_ms']+29999
                elif change=='boundary': seat['first']=2
                elif change=='wait': seat['timing'][0]['max_wait_ms']=50.001
                elif change=='hold': seat['holds']=[42]
                elif change=='rate': seat['timing'][0]['elapsed_ms']=2400*1000/59.49
                else: value['clock_brackets']['events'][0]['sha256']=''
                self.assertFalse(judge('spectator',value)['passed'])


if __name__=='__main__':
    unittest.main()
