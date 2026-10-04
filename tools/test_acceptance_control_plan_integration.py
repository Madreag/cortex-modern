"""Synthetic plan/manifest integration; no native engine or remote box is used."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

from test_acceptance_resume import fixture, write
from test_acceptance_pipeline_controls import INVENTORY
from acceptance_collection import Share
from acceptance_control_pair import ordinary_verdict, injection_receipt
from acceptance_pipeline_controls import SHARE, augment, collect_final, finish_required, judge_final, remove_c3_result
from acceptance_mod import sha256

PROPOSAL = Path('D:/Projects/reviews/takeover-20260909/grok-workers/astra-acceptance-rows-20261002/proposals/controls-0b')


@unittest.skipUnless((PROPOSAL/'acceptance_manifest.py').is_file(), 'owner integration proposal unavailable')
class ControlPlanIntegration(unittest.TestCase):
    def prepare(self, parent, policy_v2=False):
        root = parent/'run'; root.mkdir()
        plan_path, requirements = fixture(root, item='21')
        plan, schedule = augment(json.loads(plan_path.read_text()), json.loads((root/'split-plan.json').read_text()))
        repo = Path(__file__).resolve().parents[1]
        plan['generated'] = dict(repo=str(repo), controls_sources={name: sha256(repo/'tools'/name)
            for name in ('acceptance_control_pair.py', 'acceptance_pipeline_controls.py')})
        if policy_v2:
            plan['box_policy'] = dict(version=2)
            for row in plan['rows']:
                if row['share'] != SHARE:
                    row.update(section=1, box='EROL-PC', driver_box='EROL-PC',
                               engine_boxes={'EROL-PC': 1}, window_required=False)
        exe = json.loads((root/'EROL-PC.identity.json').read_text())['exe_sha256']
        schedule['exe_sha256'] = exe
        write(root/'acceptance-plan.json', plan); write(root/'split-plan.json', schedule)
        if policy_v2:
            frozen = Path('D:/mx/astra-acceptance-rows-20261002/resume-3/frozen-tools-o/tools/acceptance_collection.py')
            spec = importlib.util.spec_from_file_location('frozen_section_receipt_unit', frozen)
            collector = importlib.util.module_from_spec(spec); spec.loader.exec_module(collector)
            for section in ('0b', 1):
                collector.start_section(root, section, marker=parent/'absent-unit-marker',
                                        inventory_root=INVENTORY, optional_boxes={})
        share = Share(root, SHARE, INVENTORY)
        for control in ('C1', 'C2', 'C3'):
            command_id = 'control.'+control
            share.begin(command_id, ['synthetic-unit-fixture-only; no engine executed'])
            native = share.out/command_id; native.mkdir()
            texts = dict(host='', client='')
            if control == 'C1':
                texts = dict(host='[net-test] live perturb frame=240\n', client='[lockstep] desync at frame 240\n')
            if control == 'C2':
                texts = dict(host='[net-match] hold peer=2 frame=244 AI in control\n', client='[net-test] live stall frame=240 ms=5000\n')
            for who, text in texts.items():
                (native/who).mkdir(); (native/who/'stdout.log').write_text(text)
            product = ordinary_verdict(dict(passed=control != 'C1'), texts)
            product.update(case=command_id, collection_id=share.run_id, source_sha=share.source)
            write(native/'run-result.json', product)
            write(native/'identity.json', dict(status='PASS', box='EROL-PC', platform='unit',
                collection_id=share.run_id, source_sha=share.source, executable_sha256=exe,
                build=dict(commit=share.source, executable_sha256=exe)))
            write(native/'injection.json', dict(native_mode=True, collection_id=share.run_id, source_sha=share.source,
                executable_sha256=exe, **injection_receipt(texts),
                records={who: dict(pid=100+i, exit_code=0, timed_out=False) for i, who in enumerate(texts)},
                log_sha256={who: sha256(native/who/'stdout.log') for who in texts}))
            log = share.receipt_dir(command_id)/'driver-stdout.log'; log.write_text('Synthetic unit fixture; not native credit.\n')
            if control == 'C3': remove_c3_result(share, parent/'C3-retained')
            finish_required(share, command_id, int(not product['passed']), log)
        collect_final(share, root/'controls/LIST', INVENTORY)
        gate = judge_final(root, INVENTORY)
        self.assertTrue(gate['passed'], gate)
        write(root/'controls/gate.json', gate)
        spec = importlib.util.spec_from_file_location('proposed_controls_manifest', PROPOSAL/'acceptance_manifest.py')
        reader = importlib.util.module_from_spec(spec); spec.loader.exec_module(reader)
        return root, requirements, reader

    def test_expected_control_failures_do_not_turn_green_products_red(self):
        with tempfile.TemporaryDirectory() as folder:
            root, requirements, reader = self.prepare(Path(folder))
            document = reader.build_manifest(root/'acceptance-plan.json', requirements)
            self.assertTrue(document['passed'], document)
            self.assertTrue(document['controls']['passed'])
            self.assertEqual([row['status'] for row in document['rows'] if row['share'] == SHARE], ['DIAGNOSTIC']*3)

    def test_missing_gate_prevents_final_green(self):
        with tempfile.TemporaryDirectory() as folder:
            root, requirements, reader = self.prepare(Path(folder))
            (root/'controls/gate.json').unlink()  # Synthetic test temp file only.
            document = reader.build_manifest(root/'acceptance-plan.json', requirements)
            self.assertFalse(document['passed'])
            self.assertTrue(any('section 0b' in reason for reason in document['errors']))

    def test_real_version2_section_receipts_accept_string_section_0b(self):
        with tempfile.TemporaryDirectory() as folder:
            root, requirements, reader = self.prepare(Path(folder), policy_v2=True)
            document = reader.build_manifest(root/'acceptance-plan.json', requirements)
            self.assertTrue(document['passed'], document)
            self.assertEqual(json.loads((root/'sections/0b.json').read_text())['section'], '0b')


if __name__ == '__main__':
    unittest.main()
