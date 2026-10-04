"""The declared roster and both section variants cover the complete acceptance set."""
import contextlib
import copy
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from test_harness_resume5 import built,REPO
from test_acceptance_resume3 import build_plan,acceptance_render
from test_inventory_oracle_evidence import INVENTORY
import check_plan


class AcceptancePlanVariants(unittest.TestCase):
    def test_declaration_is_stable_under_the_run_chains_instrument_environment(self):
        from test_inventory_oracle_evidence import run_stream
        with patch.object(run_stream,'NO_FULLSTATE',False): before=built()['plan']
        with patch.object(run_stream,'NO_FULLSTATE',True): after=built()['plan']
        self.assertTrue(build_plan.comparable(before)==build_plan.comparable(after),
                        'the RUN chain environment must not change its predeclared arguments')

    def test_plan_check_refuses_a_game_role_change_even_when_paths_are_unchanged(self):
        plan=built()['plan'];altered=copy.deepcopy(plan)
        altered['rows'][0]['engine_boxes']={'EROL-PC':1}
        altered['rows'][0]['window_required']=False
        self.assertNotEqual(build_plan.comparable(plan),build_plan.comparable(altered))

    def test_matrix_references_follow_the_sections_and_the_new_host_rotation(self):
        result=built()
        matrix=check_plan.matrix(result['plan'],REPO)
        self.assertTrue(matrix['passed'],matrix['problems'])

    def test_full_synthetic_derivation_covers_the_new_count(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);plan=root/'plan'
            with contextlib.redirect_stdout(io.StringIO()): acceptance_render.write_all(built(),plan)
            process=subprocess.run([sys.executable,'-B',str(INVENTORY/'acceptance-v1/synthetic_evidence.py'),
                '--plan',str(plan/'acceptance-plan.json'),'--schedule',str(plan/'split-plan.json'),'--out',str(root/'complete')],
                capture_output=True,text=True,timeout=120)
            self.assertEqual(process.returncode,0,(process.stdout+process.stderr)[-3000:])
            result=json.loads((root/'complete/derivation.json').read_text())
            planned=built()['plan']; controls=planned.get('controls',{}).get('ids',[])
            self.assertEqual(result['counts']['PASS'],len(planned['rows'])-len(controls))
            self.assertEqual(sorted((row['id'],row['status']) for row in result['row_errors']),[(cid,'DIAGNOSTIC') for cid in sorted(controls)])

    def test_without_window_defers_exactly_the_window_rows_and_nothing_else(self):
        from synthetic_evidence import materialize
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);plan=root/'plan';built_plan=built()
            with contextlib.redirect_stdout(io.StringIO()): acceptance_render.write_all(built_plan,plan)
            result=materialize(plan/'acceptance-plan.json',plan/'split-plan.json',root/'without',without_window=True)
            self.assertTrue(result['passed'],result['errors']+result['row_errors'])
            expected={(row['share'],row['id']) for row in built_plan['plan']['rows'] if row['window_required']}
            self.assertEqual({(row['share'],row['id']) for row in result['deferred']},expected)
            self.assertEqual({row['reason'] for row in result['deferred']},{'deferred to the EROL-PC window'})
            self.assertEqual(result['counts']['PASS'],result['rows']-len(expected))


if __name__ == '__main__':
    unittest.main()
