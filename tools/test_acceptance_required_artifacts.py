"""Every required native verdict belongs to a command, including separate child files."""
import json
from pathlib import Path
import tempfile
import unittest

from test_acceptance_resume import INVENTORY, fixture, write
from acceptance_collection import Share
if INVENTORY.is_dir():
    import acceptance_manifest as reader


@unittest.skipUnless(INVENTORY.is_dir(), 'external inventory tools are absent')
class RequiredArtifacts(unittest.TestCase):
    def test_unowned_required_failure_blocks_the_collection_manifest(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); plan, requirements = fixture(root)
            path = root/'S1/required-build/result.json'
            write(path, dict(passed=False, required=True, reason='native required build failed'))
            collection = root/'S1/DEFECTS.json'; value = json.loads(collection.read_text())
            value.update(hard_count=1, defects=[dict(kind='verdict', path=str(path), quoted_line='native required build failed')])
            value['evidence_sha256'][str(path)] = reader.digest(path); write(collection, value)
            self.assertFalse(reader.build_manifest(plan, requirements)['passed'])

    def test_native_child_failure_cannot_be_hidden_by_a_green_main_product(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); plan, requirements = fixture(root)
            document = json.loads(plan.read_text()); document['rows'][0]['product']['path'] = 'S1/case/result.json'
            write(root/'acceptance-plan.json', document)
            schedule = json.loads((root/'split-plan.json').read_text()); schedule['shares'][0]['box'] = 'EROL-PC'
            write(root/'split-plan.json', schedule)
            share = Share(root, 'S1', INVENTORY)
            share.begin('case', ['synthetic-fixture'])
            write(root/'S1/case/child-result.json', dict(passed=False, required=True, reason='native required child failed'))
            log = root/'stdout.log'; log.write_text('synthetic retained child\n')
            share.finish('case', 0, log); share.collect()
            row = reader.build_manifest(root/'acceptance-plan.json', requirements)['rows'][0]
            self.assertEqual(row['product_verdict'], 'FAIL', row)


if __name__ == '__main__':
    unittest.main()
