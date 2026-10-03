"""Acceptance entry conditions and the declared host-loss dependency; no engines."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from test_inventory_oracle_evidence import INVENTORY

PLAN_DIRECTORY = INVENTORY / 'acceptance-v1'
if PLAN_DIRECTORY.is_dir():
    sys.path.insert(0, str(PLAN_DIRECTORY))
    import build_plan
    import render as acceptance_render

BASH = Path('C:/Program Files/Git/bin/bash.exe') if os.name == 'nt' else Path(shutil.which('bash') or '/missing-bash')


@unittest.skipUnless(PLAN_DIRECTORY.is_dir(), 'external acceptance plan is absent')
class AcceptanceResume3(unittest.TestCase):
    @unittest.skipUnless(BASH.is_file(), 'bash is absent')
    def test_y04_missing_repo_refuses_once_before_any_preflight_work(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            guard = root / 'guard.sh'
            guard.write_text("set -T\ntrap 'case \"$BASH_COMMAND\" in SELF=*|TIP=*|mkdir*) "
                             "printf \"unexpected work before argument refusal\\n\" >&2; exit 97;; esac' DEBUG\n", encoding='utf-8')
            env = {key: value for key, value in os.environ.items() if key != 'ACCEPTANCE_REPO'}
            env['BASH_ENV'] = guard.as_posix()
            for run in (1, 2, 3):
                with self.subTest(run=run):
                    script = root / f'RUN-{run}.sh'
                    script.write_bytes(acceptance_render.RUN_TEMPLATE.replace('@N@', str(run)).encode('utf-8'))
                    result = subprocess.run([str(BASH), str(script)], capture_output=True, text=True, env=env, timeout=15)
                    self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                    lines = (result.stdout + result.stderr).splitlines()
                    self.assertEqual(len(lines), 1, lines)
                    self.assertIn('ACCEPTANCE_REPO', lines[0])
                    self.assertIn('required', lines[0])

    def test_y05_hl4_gap_names_the_engine_schedule_action_and_receipts(self):
        row = next(row for row in build_plan.cross_rows() if row['id'] == 'cross.hl4')
        self.assertIn('G-HL4', row['gaps'], row['gaps'])
        text = build_plan.GAP_TEXT['G-HL4']
        self.assertIn('OWNER: engineer', text)
        for name in ('moderation-ban', 'scheduled_hold', 'moderation_action', 'moderation_terminal', 'moderation_snapshot'):
            self.assertIn(name, text)


if __name__ == '__main__':
    unittest.main()
