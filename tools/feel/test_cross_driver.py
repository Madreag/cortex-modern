import copy
import json
from pathlib import Path
import tempfile
import unittest

import cross_peers
import cross_report


class CrossDriverTests(unittest.TestCase):
    def plan(self):
        return cross_peers.make_plan(cross_peers.parse_args(['--dry-run']))

    def test_closed_instance_sealing_preserves_nested_record_paths(self):
        from feel.records import open_record
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary); (root/'engine/feel').mkdir(parents=True)
            (root/'engine/feel/raw.jsonl').write_bytes(b'{"type":"frame"}\n')
            (root/'live.jsonl').write_bytes(b'{"tick":1}\n')
            cross_peers.seal_evidence(root)
            with open_record(root/'engine/feel/raw.jsonl','rb') as stream:
                self.assertEqual(stream.read(),b'{"type":"frame"}\n')
            index=[json.loads(line) for line in (root/'compressed-records.jsonl').read_text().splitlines()]
            self.assertIn('engine/feel/raw.jsonl',[r['original'].replace('\\','/') for r in index])

    def test_three_box_plan_has_private_instances_and_no_host_kill(self):
        plan = self.plan()
        self.assertEqual(len(plan['instances']), 3)
        self.assertEqual(len({s['own'] for s in plan['specs']}), 3)
        self.assertTrue(all('incarnation-0' in s['own'] for s in plan['specs']))
        self.assertTrue(all('-feel-measure' in s['flags'] for s in plan['specs']))
        self.assertFalse(any('-tick-hashes' in s['flags'] for s in plan['specs']))

    def test_manifest_accepts_n_boxes_without_a_peer_cap(self):
        original = json.loads((cross_peers.HERE / 'cross_peers/boxes.json').read_text())
        for n in range(3, 35):
            box = copy.deepcopy(original['boxes'][-1]); box['name'] = f'linux{n}'; box['ssh'] = f'linux{n}'
            original['boxes'].append(box)
            original['instances'].append(dict(name=f'peer{n}', box=box['name'], port_block=[49900,49904], seat='spectator'))
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)/'boxes.json'; path.write_text(json.dumps(original))
            self.assertEqual(len(cross_peers.load_boxes(path)['instances']), 35)

    def test_same_box_ports_cannot_overlap(self):
        original = json.loads((cross_peers.HERE / 'cross_peers/boxes.json').read_text())
        original['instances'].append(dict(name='extra', box='Mac', port_block=[49902,49906], seat='member'))
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)/'boxes.json'; path.write_text(json.dumps(original))
            with self.assertRaisesRegex(ValueError, 'share a port'): cross_peers.load_boxes(path)

    def test_chaos_seed_reproduces_choices(self):
        args = ['--scenario','chaos','--ticks','36000','--chaos-seed','71']
        first = cross_peers.make_plan(cross_peers.parse_args(args))
        second = cross_peers.make_plan(cross_peers.parse_args(args))
        self.assertEqual(first['faults'], second['faults'])
        self.assertIn('choices only',first['seed_scope'])

    def test_faults_follow_the_declared_restart_incarnation(self):
        plan=cross_peers.make_plan(cross_peers.parse_args(['--scenario','soak']))
        mac=[row for row in plan['faults'] if row['peer']=='mac']
        self.assertEqual([row['incarnation'] for row in mac],[0,1,1])
        spec=next(s for s in plan['specs'] if s['peer']=='mac')
        restarted=cross_peers.restart_spec(spec,dict(tick=400,budget_tick=14400,budget_base=14000,first_gameplay_tick=1))
        self.assertNotEqual(restarted['own'],spec['own'])
        old_ticket=spec['flags'][spec['flags'].index('-net-reconnect-ticket')+1]
        new_ticket=restarted['flags'][restarted['flags'].index('-net-reconnect-ticket')+1]
        self.assertEqual(old_ticket,new_ticket)
        self.assertEqual(restarted['incarnation'],1)

    def test_tail_keeps_partial_lines_and_rotated_parts(self):
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'events.jsonl'
            path.write_bytes(b'{"tick":1}\n{"tick":')
            tail=cross_peers.Tail(path)
            self.assertEqual(tail.read(),[dict(tick=1)])
            with path.open('ab') as stream: stream.write(b'2}\n')
            self.assertEqual(tail.read(),[dict(tick=2)])
            Path(str(path)+'.part1').write_text('{"tick":3}\n')
            self.assertEqual(tail.read(),[])
            self.assertEqual(tail.read(),[dict(tick=3)])

    def test_missing_every_peer_produces_failure_and_phone_report(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)/'run'; root.mkdir()
            plan=self.plan(); (root/'manifest.json').write_text(json.dumps(plan))
            result=cross_report.build_report(root)
            self.assertFalse(result['passed'])
            self.assertFalse(result['checks']['three_real_boxes'])
            page=(root/'report.html').read_text(encoding='utf-8')
            self.assertIn('name="viewport"',page)
            self.assertNotIn('<script src=',page)
            self.assertNotIn('<link ',page)
            self.assertEqual(len([r for r in result['requirements'] if isinstance(r['number'],int)]),71)
            self.assertTrue((root.parent/'index.html').is_file())


if __name__ == '__main__': unittest.main()
