import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import test_minimized_peer as minimized
from compare_sim_traces import CORE


class MinimizedEvidence(unittest.TestCase):
    def execute(self, *, missing_schema=False, missing_tick=False):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder) / 'run'
            class EngineFixture:
                def __init__(self, name): self.name = name
                def start(self):
                    (root / self.name).mkdir()
                    (root / self.name / 'stdout.log').write_text(
                        '[selftest] window tick=660 minimized=1 hidden=1\n[selftest] window tick=2460 minimized=0 hidden=1\n')
                    rows = [dict(tick=tick, round=7, wall_ms=tick*1000/60,
                                 **({} if missing_schema else dict(subsystems=dict.fromkeys(CORE | {'controller'}, '0'))))
                            for tick in range(1, minimized.TICKS+1) if not (missing_tick and tick==300)]
                    (root / f'{self.name}-live.jsonl').write_text('\n'.join(map(json.dumps, rows)))
                    return self
                def finish(self): return dict(exit_code=0)
                def close(self): pass
            with patch.object(sys, 'argv', ['test_minimized_peer', '--out', str(root)]), \
                 patch.object(minimized, 'make_run', side_effect=lambda repo, flags, out, *a, **k: EngineFixture(out.name)), \
                 contextlib.redirect_stdout(io.StringIO()):
                minimized.main()
            return json.loads((root / 'result.json').read_text())

    def test_missing_schema_cannot_compare_equal(self):
        self.assertFalse(self.execute(missing_schema=True)['checks']['hashes_equal'])

    def test_missing_tick_has_no_unproved_allowance(self):
        self.assertFalse(self.execute(missing_tick=True)['checks']['hashes_equal'])

    def test_complete_hashes_pass_but_hidden_minimize_cannot_certify_fullscreen(self):
        result = self.execute()
        self.assertTrue(result['checks']['hashes_equal'])
        self.assertFalse(result['pass'])
        self.assertEqual(result['details']['fullscreen_minimize']['status'], 'NOT COVERED')


if __name__ == '__main__':
    unittest.main()
