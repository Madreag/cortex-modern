"""The Linux peer joins the four-way plan without changing legacy rosters."""

import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import cross_peers


class LinuxPeerTests(unittest.TestCase):
    def plan(self, host='erol', roster='four-way'):
        return cross_peers.make_plan(cross_peers.parse_args([
            '--lane', 'cross-linux-selftest', '--mac-guard', '/tmp/cross-mac-ready',
            '--host', host, '--roster', roster, '--dry-run', '--ticks', '1201']))

    def test_linux_has_its_own_route_guard_and_ports(self):
        manifest = json.loads((cross_peers.HERE / 'cross_peers/boxes.json').read_text())
        boxes = {box['name']: box for box in manifest['boxes']}
        self.assertIn('Linux', boxes)
        box = boxes['Linux']
        self.assertEqual((box['kind'], box['ssh'], box['python']), ('posix-ssh', '3090', '/usr/bin/python3'))
        self.assertEqual(box['tree'], '/home/erol/cortex-workers/opus-run-cross-linux-20260928/repo')
        self.assertEqual(box['executable'], box['tree'] + '/build-gcc/CortexCommand')
        self.assertEqual(box['environment']['DISPLAY'], ':0')
        self.assertTrue(box['guard_file'].startswith('/home/erol/cortex-workers/opus-run-cross-linux-20260928/'))
        peer = next(peer for peer in manifest['instances'] if peer['name'] == 'linux')
        self.assertNotIn(box['directory_port'], range(peer['port_block'][0], peer['port_block'][1] + 1))
        self.assertTrue(all(peer['port_block'] != other['port_block'] for other in manifest['instances'] if other['name'] != 'linux'))

    def test_four_way_is_four_humans_in_supported_pvp_mode(self):
        for host in ('erol', 'linux'):
            plan = self.plan(host)
            self.assertEqual({spec['peer'] for spec in plan['specs']}, {'erol', 'edith', 'mac', 'linux'})
            self.assertEqual([spec['peer'] for spec in plan['specs'] if spec['role'] == 'host'], [host])
            for spec in plan['specs']:
                flags = spec['flags']
                for flag, value in (('-net-match-peers', '4'), ('-net-match-humans', '4'),
                                    ('-net-match-mode', 'pvp-skirmish'), ('-net-match-cpu-slots', '0')):
                    self.assertEqual(flags[flags.index(flag) + 1], value)
                self.assertEqual(spec['ticks'], 1201)
                self.assertEqual(spec['env']['CCCP_HEADLESS'], '1')

    def test_legacy_rosters_keep_their_three_human_boxes(self):
        for roster in ('three-way', 'allies', 'ai-heavy', 'mixed'):
            self.assertEqual({spec['peer'] for spec in self.plan(roster=roster)['specs']}, {'erol', 'edith', 'mac'})

    def test_each_declared_box_is_a_distinct_machine(self):
        receipts = {name: {'machine_id': name} for name in ('EROL-PC', 'EDITH', 'Mac', 'Linux')}
        cross_peers.require_distinct_machines(receipts)
        receipts['Linux']['machine_id'] = 'Mac'
        with self.assertRaisesRegex(RuntimeError, 'distinct'):
            cross_peers.require_distinct_machines(receipts)


if __name__ == '__main__':
    unittest.main()
