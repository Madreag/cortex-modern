"""Nonrequired evidence keeps the same meaning in a list and a dictionary."""
import unittest
import json
from pathlib import Path
import tempfile
from test_acceptance_resume import INVENTORY

if INVENTORY.is_dir():
    import extract_defects


@unittest.skipUnless(INVENTORY.is_dir(), 'external inventory tools are absent')
class DiagnosticLists(unittest.TestCase):
    def test_extraction_keeps_the_same_scoped_diagnostic_contract(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            scoped = dict(required=False, reason='1201-tick match: shorter than the 120 s warm-up',
                          census=dict(status='NOT COVERED', passed=False))
            (root/'result.json').write_text(json.dumps(dict(passed=True, memory=scoped)))
            result = extract_defects.Extractor(root, True, None).run()
            self.assertTrue(result['collection_complete'], result['collection_errors'])
            self.assertEqual(result['hard_count'], 0)
            scoped['census']['required'] = True
            (root/'result.json').write_text(json.dumps(dict(passed=True, memory=scoped)))
            self.assertGreater(extract_defects.Extractor(root, True, None).run()['hard_count'], 0)

    def test_named_diagnostic_in_a_list_does_not_become_a_required_failure(self):
        diagnostic = dict(status='NOT COVERED', required=False,
                          reason='engineer receipt: fixture-only semantic event schema is not emitted')
        self.assertTrue(extract_defects.file_verdict(dict(passed=True, evidence=[diagnostic]), 'result.json'))

    def test_required_child_of_a_diagnostic_remains_binding(self):
        diagnostic = dict(status='NOT COVERED', required=False, reason='optional aggregate',
                          child=dict(status='FAIL', required=True, reason='required native receipt absent'))
        self.assertFalse(extract_defects.file_verdict(dict(passed=True, evidence=diagnostic), 'result.json'))


if __name__ == '__main__':
    unittest.main()
