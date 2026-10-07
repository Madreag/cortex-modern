"""Acceptance execution uses declared remote tasks and preserves local-only credentials."""
import contextlib
from datetime import datetime, timedelta, timezone
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

import acceptance_collection as collection
from test_acceptance_resume import fixture, write, INVENTORY
from test_harness_resume5 import built, REPO
from test_inventory_oracle_evidence import run_split, run_stream
from test_acceptance_resume3 import acceptance_render as render


class AcceptanceDispatch(unittest.TestCase):
    def test_cloudflare_driver_keeps_the_declared_remote_peer_roles(self):
        row=next(row for row in built()['plan']['rows'] if row['id']=='gap.mp-relay-cloudflare')
        self.assertIn('{REPO}/tools/relay_cloudflare_match.py',row['argv'])
        self.assertEqual(row['engine_boxes'],{'ALLY':1,'EDITH':1})
        self.assertEqual({ref['box'] for ref in row['identities']},{'ALLY','EDITH'})

    def test_four_k_rows_start_before_remote_capture_work_to_keep_one_short_window(self):
        import acceptance_sections as sections
        events=[]
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);write(root/'acceptance-plan.json',built()['plan'])
            write(root/'split-plan.json',dict(section_receipts={'4':{}},exe_sha256='a'*64))
            options=SimpleNamespace(root=root,repo=REPO,inventory=INVENTORY,section=4,dry_run=False,asan_repo='D:/z13-asan')
            with patch.object(sections.subprocess,'run',side_effect=lambda *a,**k:(events.append('split') or SimpleNamespace(returncode=0))), \
                 patch.object(collection,'Share',return_value=Mock()), \
                 patch.object(collection,'wait_for_window',return_value={'state':'READY'}), \
                 patch.object(collection,'run',side_effect=lambda share,cid,*args:(events.append(cid) or 0)), \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(sections.run_section(options),0)
        self.assertEqual(set(events[:2]),{'X.ui-3840x2160','X.lag-3840x2160-uncapped'},events)

    def test_relay_game_peers_are_reserved_after_their_ordinary_box_queues(self):
        events=[]
        class Thread:
            def __init__(self,target,name): self.target,self.name=target,name
            def start(self): self.target()
            def join(self): pass
        common=dict(repo=str(REPO),exe='never-run',tools_root=str(REPO/'tools'),scratch_root='unused',streams=['S1'])
        local=run_split.Box(name='EROL-PC',kind='local',**common)
        ally=run_split.Box(name='ALLY',kind='windows-task',ssh='ally',**common)
        commands=[run_stream.Command('S1.relay','tools/unit.py',[],engine_count=0,driver_only=True),
                  run_stream.Command('S1.ordinary','tools/unit.py',[],engine_count=1)]
        shares=[run_split.Share('S1',local,['S1.relay'],'driver',1,False),run_split.Share('S1',ally,['S1.ordinary'],'ally',1,False)]
        def ordinary(queue,options,rb,steps,results):
            events.append('ALLY')
            for share in queue: run_split.finish_share(share,options,results,0)
        def coordinator(queue,options,steps,results):
            self.assertIn('ALLY',events,'the Ally task must be released before the relay coordinator takes it')
            events.append('relay')
            for share in queue: run_split.finish_share(share,options,results,0)
        with tempfile.TemporaryDirectory() as folder, \
             patch.object(run_split,'load_manifest',return_value=([local,ally],{})), \
             patch.object(run_split.run_stream,'build_streams',return_value={'S1':commands}), \
             patch.object(run_split,'plan',return_value=(shares,[])),patch.object(run_split,'print_plan',return_value={}), \
             patch.object(run_split,'git',return_value='a'*40), \
             patch.object(run_stream,'exe_sha',return_value='a'*64), \
             patch.object(run_split,'load_remote_box',return_value=Mock()),patch.object(run_split.threading,'Thread',Thread), \
             patch.object(run_split,'run_remote_queue',side_effect=ordinary),patch.object(run_split,'run_local_queue',side_effect=coordinator), \
             contextlib.redirect_stdout(io.StringIO()):
            code=run_split.main(['--repo',str(REPO),'--exe','a'*64,'--streams','S1','--out',folder])
        self.assertEqual(code,0,events)
        self.assertEqual(events,['ALLY','relay'])

    def test_remote_hold_budget_covers_both_unchanged_six_minute_contracts(self):
        row=next(row for row in built()['plan']['rows'] if row['id']=='S1.turn-hold' and row['runner']=='run_split')
        command=row['execution'];seconds=int(command['args'][command['args'].index('--seconds')+1])
        self.assertGreaterEqual(command['timeout'],2*(seconds+180))

    def test_build_inputs_and_asan_receipt_follow_the_remote_sanitizer_tree(self):
        row = next(row for row in built()['plan']['rows'] if row['id']=='S4.win-asan-repair')
        self.assertEqual(row['section'],4)
        result=built();declaration=result['plan']
        with tempfile.TemporaryDirectory() as folder:
            plan=Path(folder)/'plan.json';write(plan,declaration)
            share=next(share for share in result['shares'] if row['id'] in share.ids)
            options=SimpleNamespace(exe='a'*64,confirming=True,asan_repo=Path('D:/z13-asan'),turn_conf=None,
                                    acceptance_plan=plan,collection_id='unit')
            argv=run_split.stream_argv(share,options,'D:/z13-runtime','D:/native-root',[])
            self.assertIn('--asan-receipt',argv)
            receipt=argv[argv.index('--asan-receipt')+1]
            self.assertIn('S4.win-asan-build/exe-ledger.jsonl',receipt.replace('\\','/'))

    def test_run_chain_has_no_global_user_marker_refusal_and_dispatches_each_section(self):
        text=render.RUN_TEMPLATE
        self.assertNotIn('[ ! -e "$MK" ] || refuse',text)
        self.assertIn('acceptance_sections.py',text)
        self.assertIn('for section in 1 2 3 4',text)
        self.assertIn('run_section 5',text)
        self.assertIn('run_section 6',text)

    def test_cross_capture_uses_the_remote_z13_host_and_finished_mac_stream(self):
        row = next(row for row in built()['plan']['rows'] if row['id'] == 'S6.cross-host-join')
        self.assertEqual(row['runner'], 'direct')
        self.assertIn('{REPO}/tools/acceptance_cross_capture.py', row.get('argv', []))
        self.assertIn('--host-box', row['argv'])
        self.assertEqual(row['argv'][row['argv'].index('--host-box')+1], 'Z13')

    def test_relay_native_commands_cannot_fall_back_to_local_engines(self):
        for row in built()['plan']['rows']:
            if row['runner'] == 'run_split' and (row['id'] in ('S1.turn-hold', 'S1.turn-renew', 'S1.relay-compare') or row.get('scenario') == 'mp-direct-vs-relay'):
                with self.subTest(row=row['id']):
                    execution = row['execution']
                    self.assertTrue(execution.get('driver_only'), row)
                    self.assertEqual(execution['engine_count'], 0)
                    self.assertEqual(execution['args'][execution['args'].index('--host-box')+1], 'ALLY')
                    self.assertEqual(execution['args'][execution['args'].index('--client-box')+1], 'EDITH')
                    self.assertEqual(row['engine_boxes'], {'ALLY':1, 'EDITH':1})

    def test_readback_pair_cases_are_not_declared_as_one_ally_engine(self):
        plan=built()['plan']
        for name in ('S2a.readback-all','S4.win-asan-repair'):
            row=next(row for row in plan['rows'] if row['id']==name)
            self.assertEqual(row['engine_boxes'],{'Z13':2},row)

    def test_native_readback_plan_names_both_engines_and_the_existing_sizes(self):
        with tempfile.TemporaryDirectory() as folder:
            argv = [sys.executable,'-B',str(REPO/'tools/test_menu_readback.py'),'--repo',str(REPO),
                    '--out',str(Path(folder)/'unused'),'--case','all','--size','640x360','--all-sizes','--port','48530','--dry-run']
            process = subprocess.run(argv,capture_output=True,text=True,timeout=30)
            self.assertEqual(process.returncode,0,process.stderr)
            plan = json.loads(process.stdout)
            self.assertEqual(plan['engine_count'],4)
            self.assertIn(['repair','640x360'],plan['cases'])
            self.assertIn(['in-match','1280x720'],plan['cases'])
            self.assertIn(['lobby-name','1920x1080'],plan['cases'])
            self.assertFalse(any(size=='3840x2160' for _,size in plan['cases']))
            self.assertFalse(any(Path(folder).iterdir()))

    def test_split_dry_run_accepts_the_frozen_section_assignment(self):
        result = built()
        with tempfile.TemporaryDirectory() as folder:
            plan = Path(folder)/'acceptance-plan.json';write(plan,result['plan'])
            with patch.object(run_split,'probe_optional_boxes',return_value={'Z13':True}), contextlib.redirect_stdout(io.StringIO()):
                try:
                    code = run_split.main(['--repo',str(REPO),'--exe','a'*64,'--acceptance-plan',str(plan),'--section','2','--dry-run'])
                except SystemExit as error:
                    self.fail(f'frozen section declaration refused: {error}')
            self.assertEqual(code,0)

    def test_remote_owned_row_does_not_run_its_driver_on_the_coordinator(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);plan,_=fixture(root,boxes=('Z13',))
            value=json.loads(plan.read_text())
            value['rows'][0].update(box='Z13',runner='remote-command',engine_boxes={'Z13':1},
                                   argv=[sys.executable,'-c','pass'],product=dict(path='S1/case/result.json'))
            write(root/'acceptance-plan.json',value)
            schedule=json.loads((root/'split-plan.json').read_text())
            schedule['shares'][0]['box']='Z13';write(root/'split-plan.json',schedule)
            share=collection.Share(root,'S1',INVENTORY)
            remote=SimpleNamespace(run_command=Mock(return_value=0))
            with patch.dict(sys.modules,{'acceptance_remote':remote}), \
                 patch.object(collection.subprocess,'run',return_value=SimpleNamespace(returncode=0)) as local:
                collection.run(share,'case',REPO,INVENTORY/'acceptance-v1')
            remote.run_command.assert_called_once()
            local.assert_not_called()

    def test_laptop_preparation_does_not_overwrite_its_development_tree(self):
        class AtShipment(Exception): pass
        remote=SimpleNamespace(reachable=lambda:None,mkdir=lambda path:None,scp_to=lambda *args:None,
                               task_state=lambda:'Ready',wait_task_idle=lambda **kw:None)
        rb=SimpleNamespace(RemoteBox=lambda *args,**kw:remote)
        box=run_split.Box(name='Z13',kind='windows-task',repo='D:/Projects/z13-build',
            exe='D:/Projects/z13-build/Cortex Command.exe',tools_root='D:/Projects/z13-build/tools',
            scratch_root='D:/mx',streams=['S2'],ssh='z13',task='cortex-session1',session_script='D:/mx/session1/run.ps1')
        def shipped(_rb,_remote,_repo,target,*args):
            self.assertNotEqual(target.repo,box.repo,'the inventory must not replace the engineer build')
            self.assertIn('inventory',target.repo)
            raise AtShipment()
        with tempfile.TemporaryDirectory() as folder:
            options=SimpleNamespace(out=Path(folder),repo=REPO,helpers=REPO,collect=False,task_wait_minutes=1)
            share=run_split.Share('S2',box,['S2.unit'],None,1,False)
            with patch.object(run_split,'environment_problem',return_value=None),patch.object(run_split,'ship_inputs',side_effect=shipped):
                with self.assertRaises(AtShipment):
                    run_split.run_remote_share(share,options,rb,Mock())

    def test_file_url_clone_drops_only_the_msys_translation_overrides(self):
        class AtClone(Exception): pass
        original=run_split.subprocess.run
        def command(argv,**kwargs):
            if 'clone' in argv:
                environment=kwargs.get('env',os.environ)
                self.assertNotIn('MSYS_NO_PATHCONV',environment)
                self.assertNotIn('MSYS2_ARG_CONV_EXCL',environment)
                self.assertEqual(environment.get('UNIT_KEEP_ENV'),'kept')
                raise AtClone()
            return original(argv,**kwargs)
        with tempfile.TemporaryDirectory() as folder:
            box=SimpleNamespace(repo='D:/Projects/unit')
            remote=SimpleNamespace(ssh=lambda *args,**kwargs:'')
            rb=SimpleNamespace(ps_quote=lambda value:repr(value))
            with patch.dict(os.environ,MSYS_NO_PATHCONV='1',MSYS2_ARG_CONV_EXCL='*',UNIT_KEEP_ENV='kept'), \
                 patch.object(run_split.subprocess,'run',side_effect=command):
                with self.assertRaises(AtClone):
                    run_split.sync_git(rb,remote,REPO,box,Path(folder),Path(folder),lambda message:None)

    def test_run_log_stamp_is_phoenix_time_in_git_bash(self):
        line=next(line for line in render.RUN_TEMPLATE.splitlines() if line.startswith('say()'))
        process=subprocess.run(['C:/Program Files/Git/bin/bash.exe','-c',line+'\nsay probe'],capture_output=True,text=True,timeout=10)
        self.assertEqual(process.returncode,0,process.stderr)
        stamp=re.search(r'\[([^]]+)\]',process.stdout)[1]
        observed=datetime.strptime(stamp,'%Y-%m-%d %I:%M:%S %p MST').replace(tzinfo=timezone(timedelta(hours=-7)))
        self.assertLess(abs((datetime.now(timezone(timedelta(hours=-7)))-observed).total_seconds()),60)


if __name__ == '__main__':
    unittest.main()
