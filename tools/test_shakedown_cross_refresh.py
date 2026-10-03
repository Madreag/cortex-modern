"""A remote Windows host is selected by the plan and dispatched through its task, never the PC engine."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock,patch

import cross_peers
import edith_cross
import test_directory_ice_join as directory
from test_harness_resume5 import built,REPO
from test_inventory_oracle_evidence import INVENTORY


class ShakedownCrossRefresh(unittest.TestCase):
    def test_s06_plan_host_refresh_and_task_launch_with_pc_held(self):
        path=REPO/'tools/acceptance_cross_refresh.py'
        self.assertTrue(path.is_file(),'the refresh must resolve its Windows host from the plan row')
        spec=importlib.util.spec_from_file_location('cross_refresh_unit',path)
        helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);manifest=json.loads((REPO/'tools/cross_peers/boxes.json').read_text())
            box=next(box for box in manifest['boxes'] if box['name']=='EROL-PC')
            box.update(name='Z13',kind='windows-task',ssh='z13',runner='cortex-session1',task_script='D:/mx/session1/run.ps1',directory_port=49985)
            box.pop('guard_file',None);box.pop('exclusive_marker',None)
            for peer in manifest['instances']:
                if peer['box']=='EROL-PC':peer.update(box='Z13',name='z13')
            manifest['driver']=dict(name='EROL-PC',kind='coordinator',directory_port=49918)
            boxes=root/'boxes.json';boxes.write_text(json.dumps(manifest))
            plan_path=root/'plan.json';plan_path.write_text(json.dumps(built()['plan']))
            with patch.dict(os.environ,CC_CROSS_PEERS_LANE='unit',CC_CROSS_PEERS_MAC_GUARD=str(root/'mac-ready')):
                selected=helper.resolve(plan_path,'cross.match-host-z13',boxes)
            self.assertEqual(selected['host_box'],'Z13')
            self.assertEqual(selected['driver']['name'],'EROL-PC')
            options=cross_peers.parse_args(['--boxes',str(boxes),'--host','z13','--scenario','match','--roster','four-way',
                '--out',str(root/'run'),'--lane','unit','--mac-guard',str(root/'mac-ready'),'--dry-run'])
            plan=cross_peers.make_plan(options)
            calls=[]
            class LaunchObserved(BaseException):pass
            def command(argv,**kwargs):
                calls.append(argv)
                if argv[0]=='scp' and 'preflight.json' in str(argv[-1]):
                    target=Path(argv[-1]);name=target.parent.name
                    target.write_text(json.dumps(dict(machine_id=name,content={},modules={},fixture={},executable_sha256='b'*64,
                        build=dict(commit=plan['driver_commit'],executable_sha256='b'*64))))
                if any('Get-ScheduledTask' in str(arg) for arg in argv):return 'Ready'
                if any('Start-ScheduledTask' in str(arg) for arg in argv):raise LaunchObserved()
                return ''
            process=Mock();process.poll.return_value=None
            with patch.object(cross_peers,'SCRATCH',root),patch.object(cross_peers,'command',side_effect=command), \
                 patch.object(cross_peers,'stage_remote'),patch.object(cross_peers,'remote_exists',return_value=True), \
                 patch.object(cross_peers,'inventory_guard',return_value='USER-AT-DESK'), \
                 patch.object(cross_peers,'acquire_reservation',side_effect=AssertionError('PC game reservation requested')), \
                 patch.object(cross_peers.subprocess,'Popen',return_value=process),patch.object(cross_peers.time,'sleep'), \
                 patch.object(edith_cross,'make_cert',return_value=(root/'cert',root/'key','pin')), \
                 patch.object(directory,'start_service',return_value=process),contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(LaunchObserved):cross_peers.run_plan(plan,root/'run')
            launches=[argv for argv in calls if any('Start-ScheduledTask' in str(arg) for arg in argv)]
            self.assertEqual(len(launches),1)
            self.assertEqual(launches[0][1],'z13')
            self.assertFalse(any('windows-local'==box['kind'] for box in plan['boxes']))
            for script in (REPO/'tools/linux/cross_refresh_boxes.sh',INVENTORY/'cross_refresh_boxes.sh'):
                self.assertIn('acceptance_cross_refresh.py',script.read_text())
                self.assertIn('--plan',script.read_text())


if __name__=='__main__':unittest.main()
