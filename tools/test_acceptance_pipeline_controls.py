"""Counterexamples for ordinary row scoring and final-list-only control credit."""
import unittest
import copy
import json
from pathlib import Path
import tempfile
from unittest.mock import Mock, patch

from acceptance_control_pair import ordinary_verdict, injection_receipt, flags, guard_pair
from acceptance_pipeline_controls import (SHARE, augment, collect_final, finish_required,
                                          judge_final, remove_c3_result)
from acceptance_collection import Share, start
from acceptance_mod import sha256

INVENTORY = Path('D:/Projects/reviews/takeover-20260909/grok-workers/lead-tools/inventory')


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2)+'\n', encoding='utf-8')


class OrdinaryControlVerdict(unittest.TestCase):
    def test_only_c1_host_is_perturbed_and_only_c2_client_stalls(self):
        for control in ('C1', 'C2', 'C3'):
            for who in ('host', 'client'):
                args = flags(control, who, Path('native'), 49720)
                self.assertEqual('-determinism-selftest-perturb' in args, control == 'C1' and who == 'host')
                self.assertEqual('-net-test-live-stall' in args, control == 'C2' and who == 'client')
                self.assertNotIn('-net-match-e2e-resync', args)

    def test_second_peer_accounts_only_for_own_pid(self):
        import cross_peers as cross
        box = dict(name='EROL-PC', kind='windows-local')
        with patch.object(cross, 'box_load', return_value=[]) as load, \
             patch.object(cross, 'engine_pid', return_value=123), \
             patch.object(cross, 'owns_reservation', return_value=True), \
             patch.object(cross, 'inventory_guard', return_value=None), \
             patch.object(cross, 'assert_box_guard') as guard:
            guard_pair(box, {'host': Mock()}, {})
            load.assert_called_once_with([123])
            guard.assert_called_once_with(dict(name='EROL-PC', kind='windows-task'))

    def test_foreign_native_load_is_never_excused_for_controls(self):
        import cross_peers as cross
        with patch.object(cross, 'box_load', return_value=[dict(Name='cl.exe', ProcessId=444)]), \
             patch.object(cross, 'assert_box_guard') as guard:
            with self.assertRaisesRegex(RuntimeError, 'foreign native workload'):
                guard_pair(dict(name='EROL-PC'), {}, {})
            guard.assert_not_called()

    def test_recovered_pair_hold_is_still_red_in_zero_hold_row(self):
        pair = dict(passed=True, simulation=dict(first_divergence=None),
                    held=dict(held=True), hold_lines=dict(host=['hold'], client=[]))
        texts = dict(host='[net-match] hold peer=2 frame=244 AI in control\n',
                     client='[net-test] live stall frame=240 ms=5000\n')
        result = ordinary_verdict(pair, texts)
        self.assertFalse(result['passed'])
        self.assertIn('forced hold at tick=244', result['reason'])
        self.assertIn('stall tick=240', result['reason'])

    def test_unexcused_divergence_keeps_its_measured_tick(self):
        pair = dict(passed=False, simulation=dict(first_divergence=240))
        result = ordinary_verdict(pair, dict(host='[net-test] live perturb frame=240\n',
                                            client='[lockstep] desync at frame 240\n'))
        self.assertFalse(result['passed'])
        self.assertIn('divergence at tick=240', result['reason'])

    def test_requested_stall_is_not_firing_evidence(self):
        receipt = injection_receipt(dict(host='', client='argv: -net-test-live-stall 240:5000\n'))
        self.assertEqual(receipt['stall']['client'], [])

    def test_uninjected_clean_pair_still_passes(self):
        result = ordinary_verdict(dict(passed=True), dict(host='', client=''))
        self.assertTrue(result['passed'])

    def test_generic_uninjected_hold_is_not_excused(self):
        result = ordinary_verdict(dict(passed=True),
                                  dict(host='[net-match] hold peer=2 frame=244 AI in control\n', client=''))
        self.assertFalse(result['passed'])
        self.assertIn('unscheduled hold at tick=244', result['reason'])


@unittest.skipUnless(INVENTORY.is_dir(), 'shared inventory is unavailable')
class RealCollectionCounterexamples(unittest.TestCase):
    """Synthetic inputs traverse the real Share and FINAL LIST implementations."""
    def prepare(self, directory):
        root = directory/'run'
        root.mkdir()
        source, exe = 'a'*40, 'b'*64
        plan, schedule = augment(dict(rows=[], generated=dict(head=source), schedule='split-plan.json'),
                                 dict(shares=[], source_sha=source))
        write(directory/'plan.json', plan)
        write(directory/'schedule.json', schedule)
        start(root, directory/'plan.json', directory/'schedule.json', source, exe,
              directory/'sequence.jsonl', INVENTORY)
        share = Share(root, SHARE, INVENTORY)
        for control in ('C1', 'C2', 'C3'):
            command_id = 'control.'+control
            share.begin(command_id, ['synthetic-unit-input-only; no engine executed'])
            native = share.out/command_id
            native.mkdir()
            texts = dict(host='', client='')
            pair = dict(passed=control != 'C1')
            if control == 'C1':
                texts = dict(host='[net-test] live perturb frame=240\n',
                             client='[lockstep] desync at frame 240\n')
                pair['simulation'] = dict(first_divergence=240)
            if control == 'C2':
                texts = dict(host='[net-match] hold peer=2 frame=244 AI in control\n',
                             client='[net-test] live stall frame=240 ms=5000\n')
            for who, text in texts.items():
                (native/who).mkdir()
                (native/who/'stdout.log').write_text(text)
            result = ordinary_verdict(pair, texts)
            result.update(case=command_id, control=control, collection_id=share.run_id, source_sha=source)
            write(native/'run-result.json', result)
            write(native/'identity.json', dict(status='PASS', collection_id=share.run_id,
                                              source_sha=source, executable_sha256=exe))
            write(native/'injection.json', dict(control=control, collection_id=share.run_id,
                source_sha=source, executable_sha256=exe, native_mode=True,
                records={who: dict(pid=100+i, exit_code=0, timed_out=False) for i, who in enumerate(texts)},
                **injection_receipt(texts),
                log_sha256={who: sha256(native/who/'stdout.log') for who in texts}))
            log = share.receipt_dir(command_id)/'driver-stdout.log'
            log.write_text('Synthetic test fixture; no native control credit.\n')
            if control == 'C3':
                remove_c3_result(share, directory/'retained-c3')
            finish_required(share, command_id, int(not result['passed']), log)
        collect_final(share, root/'controls/LIST', INVENTORY)
        return root, share

    def test_all_three_faults_reach_actual_final_list(self):
        with tempfile.TemporaryDirectory() as folder:
            root, _ = self.prepare(Path(folder))
            result = judge_final(root, INVENTORY)
            self.assertTrue(result['passed'], result)
            self.assertEqual(len(result['controls']), 3)
            self.assertTrue(all(row['final_line'].startswith('- A') for row in result['controls']))
            self.assertIn('result absent', result['controls'][2]['final_line'])

    def test_final_list_loss_cannot_be_repaired_from_native_logs(self):
        with tempfile.TemporaryDirectory() as folder:
            root, _ = self.prepare(Path(folder))
            from merge_defects import markdown
            path = root/'controls/LIST/DEFECTS-all.json'
            document = json.loads(path.read_text())
            document['defects'] = [row for row in document['defects']
                                   if 'control.C1' not in row.get('path', '')]
            write(path, document)
            (path.parent/'DEFECTS-all.md').write_text(markdown(document), encoding='utf-8')
            result = judge_final(root, INVENTORY)
            self.assertFalse(result['passed'])
            self.assertFalse(result['controls'][0]['passed'])

    def test_silent_stall_receipt_fails_even_when_list_mentions_hold(self):
        with tempfile.TemporaryDirectory() as folder:
            root, _ = self.prepare(Path(folder))
            path = root/SHARE/'control.C2/injection.json'
            document = json.loads(path.read_text()); document['stall']['client'] = []
            write(path, document)
            result = judge_final(root, INVENTORY)
            self.assertFalse(result['controls'][1]['passed'])
            self.assertIn('silent native stall/hold receipt', result['controls'][1]['errors'])

    def test_c3_cannot_remove_a_failed_result(self):
        with tempfile.TemporaryDirectory() as folder:
            root, share = self.prepare(Path(folder))
            product = share.product('control.C3')
            write(product, dict(passed=False, collection_id=share.run_id, source_sha=share.source))
            with self.assertRaisesRegex(ValueError, 'first pass natively'):
                remove_c3_result(share, Path(folder)/'another-preserved-copy')
            self.assertTrue(product.is_file())

    def test_plan_has_no_shared_mutation_or_duplicate_control_section(self):
        plan, schedule = dict(rows=[]), dict(shares=[])
        original = copy.deepcopy((plan, schedule))
        result = augment(plan, schedule)
        self.assertEqual((plan, schedule), original)
        with self.assertRaisesRegex(ValueError, 'already declared'):
            augment(*result)


if __name__ == '__main__':
    unittest.main()
