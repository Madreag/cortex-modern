import json
from pathlib import Path
import tempfile
import unittest
from feel import report


class CommandStart(unittest.TestCase):
    def test_p12_missing_first_frame_is_not_a_concrete_effective_start(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'commands').mkdir()
            (root / 'commands/stdout.log').write_text('')
            (root / 'commands/launch.json').write_text(json.dumps(dict(exit_code=0, evidence_complete=True)))
            (root / 'replay-report.json').write_text(json.dumps(dict(ok=True, last_frame=1200)))
            _, complete = report.remote_commands(root / 'commands/stdout.log', 1)
        self.assertFalse(complete)

    def test_only_integer_start_receipts_are_accepted(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'commands').mkdir()
            (root / 'commands/stdout.log').write_text('')
            (root / 'commands/launch.json').write_text(json.dumps(dict(exit_code=0, evidence_complete=True)))
            for first, effective, expected in [(True, None, False), (1.0, None, False), ('1', None, False),
                                               (1, None, True), (600, 600, True), (600, 600.0, False)]:
                with self.subTest(first=first, effective=effective):
                    (root / 'replay-report.json').write_text(json.dumps(dict(ok=True, first_frame=first, last_frame=1200)))
                    _, complete = report.remote_commands(root / 'commands/stdout.log', 1, effective_start=effective)
                    self.assertEqual(complete, expected)


if __name__ == '__main__':
    unittest.main()
