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
