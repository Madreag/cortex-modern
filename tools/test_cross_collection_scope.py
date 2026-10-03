"""The exact ruled scopes remain bounded and their required replacements stay binding."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

import cross_report
from feel.test_attempt_requirements import attempt
from feel.host_loss import host_loss_evidence
from test_acceptance_resume import INVENTORY
if INVENTORY.is_dir():
    import extract_defects


@unittest.skipUnless(INVENTORY.is_dir(), 'external inventory tools are absent')
class CollectionScope(unittest.TestCase):
    def test_short_memory_scope_does_not_hide_a_required_history_failure(self):
        args = attempt()
        args[0].update(scenario='match', acceptance_row=17, ticks=1201, faults=[])
        for peer in args[2].values():
            peer.update(memory=dict(passed=False), memory_by_incarnation={'0':dict(passed=False)},
                        memory_census={'0':dict(status='NOT COVERED')})
        judged = cross_report.judge_attempt(*args)
        result = dict(judged, passed=judged['v1_passed'], manifest=args[0], peers=args[2],
                      host_loss=host_loss_evidence(args[0], {}, {}, {}, []))
        cross_report.annotate_applicability(result)
        self.assertTrue(extract_defects.file_verdict(result, 'result.json'))
        result['oracles']['live_hashes']['status'] = 'FAIL'
        self.assertFalse(extract_defects.file_verdict(result, 'result.json'))

    def test_hl4_replacements_and_survivor_memory_remain_required(self):
        fields = ('live_hashes', 'full_state', 'native_completion', 'feel', 'memory', 'exits', 'pace', 'workload', 'fault_effects')
        result = dict(passed=True, manifest=dict(faults=[dict(action='host-kill')]),
            oracles={key:dict(status='FAIL', reason='ordinary all-peer aggregate') for key in fields},
            host_loss=dict(status='PASS', passed=True, ranges=[{}], survivors=['a', 'b'],
                           collection_checks=dict(survivor_memory=True), history=dict(passed=True)),
            peers={name:dict(memory=dict(passed=name != 'c'), pace=dict(passed=name != 'c'),
                             native=dict(passed=name != 'c'), exits=[dict(passed=name != 'c')]) for name in ('a', 'b', 'c')},
            comparison=dict(passed=False), fullstate=dict(passed=False), measured_workload=dict(passed=False))
        cross_report.annotate_applicability(result)
        self.assertTrue(extract_defects.file_verdict(result, 'result.json'))
        changed = copy.deepcopy(result); changed['host_loss']['history']['passed'] = False
        self.assertFalse(extract_defects.file_verdict(changed, 'result.json'))
        changed = copy.deepcopy(result); changed['peers']['a']['memory']['passed'] = False
        self.assertFalse(extract_defects.file_verdict(changed, 'result.json'))

    def test_l4p_needs_actual_packet_effects_resize_and_sustained_duration(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            names = ('erol', 'edith', 'mac', 'linux')
            peers = {name:dict(fragments=[name]) for name in names}
            receipt = dict(round=7, peer=2, jitter_ms=60, jitter_packets=9, lag_ms=200, delayed_packets=20,
                           loss_percent=5, dropped_packets=1)
            for index, name in enumerate(names, 1):
                path = root/name/'engine/stdout.log'; path.parent.mkdir(parents=True)
                path.write_text(f'[net-lockstep] start round=7 frame=1 local_peer={index}\n'
                    '[net-match] delay change peer=2 frame=650 delay=14 revision=1\n'
                    + ('[net-fake-link] '+json.dumps(receipt)+'\n' if name == 'edith' else ''))
            manifest = dict(ticks=12001, faults=[dict(id='l4p', peer='edith', lag_ms=200, jitter_ms=60, percent=5)])
            recovery = [dict(id='l4p', passed=True, requested_duration_consistent=True)]
            self.assertTrue(cross_report.l4p_effects(root, manifest, peers, recovery)['passed'])
            self.assertFalse(cross_report.l4p_effects(root, manifest, peers, [])['passed'])
            path = root/'edith/engine/stdout.log'; original = path.read_text()
            path.write_text(original.replace('"dropped_packets": 1', '"dropped_packets": 0'))
            self.assertFalse(cross_report.l4p_effects(root, manifest, peers, recovery)['passed'])
            path.write_text(original)
            path = root/'linux/engine/stdout.log'; path.write_text(path.read_text().replace('delay=14', 'delay=13'))
            self.assertFalse(cross_report.l4p_effects(root, manifest, peers, recovery)['passed'])


if __name__ == '__main__':
    unittest.main()
