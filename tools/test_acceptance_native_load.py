from copy import deepcopy
import os
from pathlib import Path
import unittest
from unittest.mock import patch

import acceptance_native_load as policy
import acceptance_remote_tasks as remote
from test_acceptance_local_host import local_profiles


class FunctionalCompilePolicy(unittest.TestCase):
    def test_compiler_exception_still_claims_the_physical_cross_marker(self):
        box=local_profiles()[0];box['compiler_overlap_row']='mod-match'
        compiler=dict(ProcessId=10,Name='cl.exe')
        with patch.dict(os.environ,{},clear=False), patch.object(Path,'exists',return_value=False), \
                patch.object(Path,'open') as opened, patch.object(policy.cross,'box_load',return_value=[compiler]), \
                patch.object(policy.cross,'owns_reservation',side_effect=[False,True]), \
                patch.object(policy.cross,'assert_box_guard'), patch.object(policy,'write_json') as proof, \
                patch.object(policy.cross,'acquire_reservation') as ordinary:
            claim=policy.acquire_reservation(box,'/virtual',1)
            self.assertEqual(os.environ['CCCP_FEEL_MATRIX_RUN'],claim['record']['token'])
            self.assertEqual(claim['marker'],str(Path('D:/mx/FEEL-MATRIX-RUNNING')))
            self.assertFalse(claim.get('borrowed'))
            self.assertEqual(proof.call_args.args[1]['measured_load'],[compiler])
        ordinary.assert_not_called()
        self.assertEqual(opened.call_args.args[0],'x')

    def test_only_pc_mod_rows_can_ignore_compiler_processes(self):
        compiler = dict(ProcessId=10,Name='MSBuild.exe')
        engine = dict(ProcessId=20,Name='Cortex Command.exe')
        good = dict(name='EROL-PC',kind='windows-local',compiler_overlap_row='mod-match')
        self.assertEqual(policy.blocking_load(good,[compiler,engine]),[engine])
        for update in ({'name':'EDITH'}, {'kind':'windows-task'}, {'compiler_overlap_row':'world-join'},
                       {'compiler_overlap_row':None}):
            self.assertEqual(policy.blocking_load({**good,**update},[compiler]),[compiler])
        self.assertEqual(policy.blocking_load(good,[{'Name':'other.exe'}]),[{'Name':'other.exe'}])

    def test_profile_cannot_attach_the_exception_to_timing_or_remote_rows(self):
        for row,index in (('world-join',0),('mod-match',1)):
            box=local_profiles(row)[index];box['compiler_overlap_row']='mod-match'
            with self.assertRaisesRegex(ValueError,'compiler overlap'):
                remote.validate_profile(box,'test',row)

    def test_permitted_load_keeps_native_timing_failure_and_raw_evidence(self):
        compiler=dict(ProcessId=10,Name='cl.exe')
        box=dict(name='EROL-PC',kind='windows-local',compiler_overlap_row='mod-match')
        value=dict(manifest=dict(acceptance_row='mod-match',boxes=[box],quiet_window=True,
                                preflights={'EROL-PC':{'load':[compiler]}}),
                   peers={'erol':dict(box='EROL-PC',samples=[dict(load=[compiler],same_box_instances=1)],
                                      feel_pass=False,feel_gated=False)})
        result=policy.apply_report_policy(deepcopy(value))['peers']['erol']
        self.assertTrue(result['feel_gated'])
        self.assertFalse(result['feel_pass'])
        self.assertEqual(result['feel_status'],'FAIL')
        self.assertEqual(result['samples'][0]['load'],[compiler])
        bad=deepcopy(value);bad['peers']['erol']['samples'][0]['load'].append(dict(Name='Cortex Command.exe'))
        self.assertFalse(policy.apply_report_policy(bad)['peers']['erol']['feel_gated'])
        bad=deepcopy(value);bad['manifest']['acceptance_row']='world-join'
        self.assertEqual(policy.apply_report_policy(bad),bad)


if __name__ == '__main__': unittest.main()
