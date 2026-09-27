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
