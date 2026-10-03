"""Earlier native round capacity survives a process restart in the retained cross run."""
import contextlib
import io
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

import cross_report
from feel.test_fullstate_obligations import cross_fixture


class NativeRoundHistory(unittest.TestCase):
    def test_capacity_reads_every_retained_incarnation_report(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); manifest = cross_fixture(root)
            for spec in manifest['specs']:
                first = cross_report.peer_root(root, manifest, spec)
                second = first.with_name('incarnation-1')
                shutil.copytree(first, second)
                for directory, round_id in ((first, 7), (second, 8)):
                    (directory/'match-report.json').write_text(json.dumps(dict(
                        pace=dict(sim_ms_per_tick=20, wall_tps=50),
                        lockstep=dict(round_id=round_id, local_capacity_tps=50, sim_tick_ms=1000/60))))
            with patch.object(cross_report, 'write_page'), patch.object(cross_report, 'write_index'), \
                 patch.object(cross_report, 'write_rerun_command', return_value='unit'), contextlib.redirect_stdout(io.StringIO()):
                result = cross_report.build_report(root)
            self.assertEqual(set(result['relative_capacity']['rounds']), {'7', '8'})
            self.assertTrue(all(row['complete'] for row in result['relative_capacity']['rounds'].values()))


if __name__ == '__main__':
    unittest.main()
