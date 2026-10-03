"""The generated plan's archive references and unstarted schedule state."""
import json
from pathlib import Path
import tempfile
import unittest

from test_acceptance_resume import INVENTORY, fixture, write
if INVENTORY.is_dir():
    import acceptance_manifest as reader


@unittest.skipUnless(INVENTORY.is_dir(), 'external inventory tools are absent')
class CompleteSchema(unittest.TestCase):
    def test_missing_picture_review_keeps_a_green_product_pending(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); plan, requirements = fixture(root)
            document = json.loads(plan.read_text())
            document['rows'][0]['review'] = dict(path='not-yet-reviewed.json'); write(plan, document)
            row = reader.build_manifest(plan, requirements)['rows'][0]
            self.assertEqual(row['product_verdict'], 'PASS')
            self.assertEqual(row['review_state'], 'AWAITING REVIEW')
            self.assertEqual(row['status'], 'AWAITING REVIEW')

    def test_review_exit_three_can_be_resolved_by_its_bound_picture_review(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); plan, requirements = fixture(root, code=3)
            product = root/'S1/case/result.json'
            write(product, dict(scenario='synthetic', verdict='agent-review-required',
                checklist=[dict(id='native', probe='pass', state='AWAITING REVIEW')]))
            collection = root/'S1/DEFECTS.json'; document = json.loads(collection.read_text())
            document['evidence_sha256'][str(product)] = reader.digest(product); write(collection, document)
            result = reader.build_manifest(plan, requirements)
            self.assertTrue(result['passed'], result['rows'])

    def test_archive_uses_the_same_path_reference_as_the_generated_plan(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); plan, requirements = fixture(root)
            document = json.loads(plan.read_text())
            document['package']['archive'] = dict(path=document['package']['archive'])
            write(plan, document)
            result = reader.build_manifest(plan, requirements)
            self.assertTrue(result['passed'], result['approved_package'])

    def test_unstarted_empty_schedule_awaits_evidence_without_credit(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            requirement = root/'requirements.md'; requirement.write_text('1. synthetic required row\n')
            write(root/'schedule.json', dict(source_sha='a'*40, shares=[dict(label='S1', ids=['one'])]))
            write(root/'plan.json', dict(schedule=dict(path='schedule.json'), rows=[dict(id='one', share='S1',
                acceptance_ids=['1'], identities=[dict(path='identity.json', box='EROL-PC')],
                product=dict(path='result.json'), collection=dict(path='DEFECTS.json'))]))
            result = reader.build_manifest(root/'plan.json', requirement)
            self.assertEqual(result['rows'][0]['status'], 'AWAITING')
            self.assertFalse(result['passed'])


if __name__ == '__main__':
    unittest.main()
