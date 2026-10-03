"""Receipt ownership, portable collection and permitted reruns; synthetic files only."""
import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from test_acceptance_resume import INVENTORY, fixture, write
if INVENTORY.is_dir():
    import acceptance_manifest as reader
    import run_stream
from acceptance_collection import Share
from acceptance_identity import identity
import cross_report
from feel.test_attempt_requirements import attempt


@unittest.skipUnless(INVENTORY.is_dir(), 'external inventory tools are absent')
class CollectionReceipts(unittest.TestCase):
    def owned_share(self, root, passed=True):
        plan, requirement = fixture(root, passed=passed, code=0 if passed else 1)
        document = json.loads(plan.read_text())
        document['rows'][0].update(product=dict(path='S1/case/result.json'), collection=dict(path='S1/DEFECTS.json'),
                                   terminal=dict(path='S1/terminal.json'))
        write(root/'acceptance-plan.json', document)
        schedule = json.loads((root/'split-plan.json').read_text())
        schedule['shares'][0]['box'] = 'EROL-PC'; write(root/'split-plan.json', schedule)
        share = Share(root, 'S1', INVENTORY)
        share.begin('case', ['synthetic-fixture'])
        log = root/'S1/case/driver-stdout.log'; log.write_text('synthetic product receipt\n')
        share.finish('case', 0 if passed else 1, log)
        share.collect()
        return root/'acceptance-plan.json', requirement, share

    def test_outside_share_uses_the_same_terminal_ownership(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); plan, requirement, _ = self.owned_share(root)
            result = reader.build_manifest(plan, requirement)
            self.assertTrue(result['passed'], result)
            terminal = root/'S1/terminal.json'; changed = json.loads(terminal.read_text())
            changed['collection_id'] = 'another-run'; write(terminal, changed)
            self.assertFalse(reader.build_manifest(plan, requirement)['passed'])

    def test_failed_product_is_complete_and_red(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); plan, requirement, _ = self.owned_share(root, passed=False)
            row = reader.build_manifest(plan, requirement)['rows'][0]
            self.assertEqual(row['status'], 'FAIL', row)
            self.assertTrue(row['collection_complete'])

    def test_missing_native_product_has_an_owned_failed_receipt(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); plan, requirement = fixture(root)
            spec = json.loads(plan.read_text()); spec['rows'][0]['product']['path'] = 'S1/case/not-written.json'
            write(root/'acceptance-plan.json', spec)
            schedule = json.loads((root/'split-plan.json').read_text()); schedule['shares'][0]['box'] = 'EROL-PC'; write(root/'split-plan.json', schedule)
            share = Share(root, 'S1', INVENTORY)
            share.begin('case', ['synthetic-fixture'])
            log = root/'log.txt'; log.write_text('driver failed before producing its native verdict\n')
            share.finish('case', 1, log); share.collect()
            row = reader.build_manifest(root/'acceptance-plan.json', requirement)['rows'][0]
            self.assertEqual(row['product_verdict'], 'FAIL', row)
            self.assertTrue(row['collection_complete'])

    def test_recorded_harness_block_can_precede_one_green_rerun(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); plan, requirement = fixture(root, passed=False, rerun=True)
            spec = json.loads(plan.read_text()); spec['rows'][0]['product']['path'] = str(root/'S1/case/result.json'); write(plan, spec)
            first = root/'S1/case/result.json'
            write(first, dict(passed=False, failure_class='harness', engine_started=False, reason='marker held before launch'))
            observed = root/'S1/case/harness-condition.json'
            write(observed, dict(kind='marker-held', observed=True, observed_before_attempt=True, product_failure=False, details=dict(pid=123)))
            collection = root/'S1/DEFECTS.json'; document = json.loads(collection.read_text())
            document['evidence_sha256'][str(first)] = reader.digest(first)
            document['commands'][0]['attempts'][0]['harness_condition'] = dict(kind='marker-held', evidence=dict(path=str(observed)), sha256=reader.digest(observed))
            write(collection, document)
            result = reader.build_manifest(plan, requirement)
            self.assertTrue(result['passed'], result)
            self.assertEqual([row['status'] for row in result['rows'][0]['attempts']], ['FAIL', 'PASS'])
            write(first, dict(passed=False, failure_class='engine', engine_started=True, reason='wall TPS below bar'))
            document['evidence_sha256'][str(first)] = reader.digest(first); write(collection, document)
            self.assertFalse(reader.build_manifest(plan, requirement)['passed'])

    def test_same_tip_picture_approval_resolves_native_pending_review(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); plan, requirement = fixture(root)
            product = root/'S1/case/result.json'
            write(product, dict(scenario='unit', verdict='agent-review-required', checklist=[dict(id='scene', frames=[1, 2], probe='pass')]))
            collection = root/'S1/DEFECTS.json'; document = json.loads(collection.read_text())
            document['evidence_sha256'][str(product)] = reader.digest(product); write(collection, document)
            self.assertTrue(reader.build_manifest(plan, requirement)['passed'])
            write(root/'review.json', dict(state='AWAITING REVIEW'))
            self.assertFalse(reader.build_manifest(plan, requirement)['passed'])

    def test_unknown_box_never_falls_back_to_a_platform_guess(self):
        with self.assertRaisesRegex(ValueError, 'unmapped-node'):
            reader.canonical_box('unmapped-node')

    def test_posix_identity_requires_measured_hash_and_matching_build(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); exe = root/'CortexCommand'; exe.write_bytes(b'synthetic executable; never executed')
            build = root/'build.json'; write(build, dict(commit='a'*40, executable_sha256=reader.digest(exe)))
            record = identity('Mac', root, exe, build, 'a'*40, 'run')
            self.assertEqual(record['status'], 'PASS', record)
            exe.write_bytes(b'changed fixture')
            self.assertEqual(identity('Mac', root, exe, build, 'a'*40, 'run')['status'], 'INCOMPLETE')

    def test_cross_soak_uses_explicit_three_way_roster(self):
        args = attempt(); manifest = args[0]
        manifest['roster'] = 'three-way'
        manifest['boxes'] = [row for row in manifest['boxes'] if row['name'] != 'Linux']
        manifest['instances'] = [row for row in manifest['instances'] if row['name'] != 'linux']
        manifest['specs'] = [row for row in manifest['specs'] if row['peer'] != 'linux']
        manifest['preflights'].pop('Linux'); args[2].pop('linux')
        self.assertTrue(cross_report.judge_attempt(*args)['v1_passed'])
        manifest['roster'] = 'four-way'
        self.assertFalse(cross_report.judge_attempt(*args)['v1_passed'])

    def test_native_selftest_product_failure_is_not_replaced_by_a_quiet_retry(self):
        import run_selftests
        with tempfile.TemporaryDirectory() as folder, \
             patch.object(run_selftests, 'box_state', return_value=dict(engines=[], builds=[])), \
             patch.object(run_selftests, 'wait_for_quiet_box', return_value=dict(quiet=True, engines=[], builds=[])), \
             patch.object(run_selftests, 'run_row', side_effect=[dict(pass_=False, **{'pass': False}, reason='state differs'), dict(pass_=True, **{'pass': True})]) as called:
            result = run_selftests.run_tail_row(None, None, 'net-match', Path(folder), None)
        self.assertFalse(result['pass'])
        self.assertEqual(called.call_count, 1)


if __name__ == '__main__':
    unittest.main()
