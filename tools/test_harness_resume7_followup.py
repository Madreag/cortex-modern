"""The reader's additional counterexamples; Python stand-ins and temporary Git trees only."""
import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock,patch

import acceptance_collection as collection
import acceptance_sections as sections
import acceptance_relay_policy as policy
import run_tools_suites
from test_harness_resume5 import built,REPO
from test_harness_resume6 import module,LEAD
from test_inventory_oracle_evidence import INVENTORY,run_split


class FollowupRulings(unittest.TestCase):
    def test_t6_coturn_alternative_is_a_directory_backend(self):
        scenario=json.loads((REPO/'tools/e2e/mp-direct-vs-relay.json').read_text())
        runs={run['name']:run for run in scenario['runs']}
        self.assertIn('relay-coturn',runs,'the alternative still declares a fixed-pair arm')
        arm=runs['relay-coturn']
        self.assertEqual(arm['directory_turn_config_path'],policy.COTURN_CONFIG)
        self.assertTrue(all(peer['settings']['NetworkHostRelayMode']=='Directory' for peer in arm['peers']))
        self.assertTrue(all(row['credential_source'].endswith('directory-coturn.json') for row in policy.arms('hold') if row['backend']=='coturn'))

    def test_t7_contract_reads_all_four_driver_runs(self):
        row=next(row for row in built()['plan']['rows'] if row['id']=='gap.mp-relay-cloudflare')
        self.assertEqual(row['relay_contract']['runs'],['cloudflare','coturn','fixed','automatic'])
        self.assertEqual(row['relay_contract'].get('fixed_proof'),'fixed')

    def test_t8_section_dispatches_nonwindow_rows_despite_a_reservation(self):
        rows=[dict(id=name,share='X',section=3,runner='direct',window_required=False,engine_boxes={'EDITH':1},box='EROL-PC') for name in ('a','b')]
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);collection.write(root/'acceptance-plan.json',dict(rows=rows))
            collection.write(root/'split-plan.json',dict(section_receipts={'3':{}},exe_sha256='b'*64))
            dispatched=[]
            options=SimpleNamespace(root=root,repo=REPO,inventory=INVENTORY,section=3,dry_run=False)
            with patch.object(collection,'wait_for_window',return_value={'state':'AWAITING'}),patch.object(collection,'Share',return_value=Mock()),patch.object(collection,'run',side_effect=lambda _,name,*args:dispatched.append(name) or 0),contextlib.redirect_stdout(io.StringIO()):
                sections.run_section(options)
            self.assertEqual(dispatched,['a','b'],'the whole-section wait suppressed remote work')

    def test_t8_user_hold_ends_a_wait_within_one_sleep(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);marker=root/'marker'
            marker.write_text(json.dumps(dict(token='lane',pid=os.getpid(),stream_root=str(root/'other'))))
            result=built(marker=marker);collection.write(root/'acceptance-plan.json',result['plan'])
            schedule=result['split_plan'];schedule.update(collection_id='unit',source_sha='a'*40,exe_sha256='b'*64,sequence={'index':1})
            collection.write(root/'split-plan.json',schedule)
            collection.start_section(root,3,marker=marker,inventory_root=INVENTORY,optional_boxes={})
            sleeps=[];clock=[0]
            def hold(seconds):
                sleeps.append(seconds);clock[0]+=seconds
                marker.write_text(json.dumps(dict(token='USER-AT-DESK',pid=os.getpid())))
            with contextlib.redirect_stdout(io.StringIO()):
                receipt=collection.wait_for_window(root,3,marker=marker,timeout=9000,sleep_fn=hold,now_fn=lambda:clock[0])
            self.assertLessEqual(len(sleeps),1,'a user hold consumed the reservation budget')
            self.assertEqual(receipt['state'],'AWAITING');self.assertEqual(receipt['reason'],run_split.WINDOW_REASON)
            from test_acceptance_resume3 import build_plan
            reader=build_plan.acceptance_manifest
            spec=next(row for row in result['plan']['rows'] if row['section']==3 and row.get('window_required'))
            decision=reader.section_choice(spec,collection.read(root/'split-plan.json'),reader.Evidence(root))
            self.assertEqual(decision['reason'],run_split.WINDOW_REASON,'the row reader changed the user hold into a lane reservation')

    def test_t9_live_target_refuses_before_checkout(self):
        refresh=module(INVENTORY/'refresh_windows.py','refresh_guard_unit')
        state=dict(head='a'*40,differing_files=[],line_ending_only=[],untracked_tools=[])
        for occupied in ('engine','lane'):
            with self.subTest(occupied=occupied),patch.object(refresh,'scan_tree',return_value=state),patch.object(refresh,'git',return_value=b'b'*40),patch.object(refresh,'guard_state',return_value={'engines':[7] if occupied=='engine' else [],'markers':['lane'] if occupied=='lane' else []},create=True):
                with self.assertRaisesRegex(ValueError,'engine|lane|occupied'):
                    refresh.native(dict(action='checkout',repo='D:/Projects/ally-build',tip='b'*40,bundle='unused'))

    def test_t9_bundle_contains_only_changes_since_box_head(self):
        refresh=module(INVENTORY/'refresh_windows.py','refresh_bundle_unit')
        create=getattr(refresh,'create_bundle',None)
        self.assertTrue(callable(create),'refresh still bundles full HEAD history')
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);repo=root/'repo';repo.mkdir()
            def git(*args):return subprocess.check_output(['git','-C',str(repo),*args],stderr=subprocess.DEVNULL).decode().strip()
            git('init');git('config','user.name','Test');git('config','user.email','test@example.invalid')
            (repo/'base.bin').write_bytes(os.urandom(1024*1024));git('add','.');git('commit','-m','Base');base=git('rev-parse','HEAD')
            (repo/'change.txt').write_text('small change\n');git('add','.');git('commit','-m','Tip');tip=git('rev-parse','HEAD')
            git('bundle','create',str(root/'full.bundle'),'HEAD')
            proof=create(repo,base,tip,root/'incremental.bundle')
            self.assertLess(proof['bytes'],(root/'full.bundle').stat().st_size//10)
            self.assertEqual(proof['range'],base+'..'+tip)

    def test_t10_compile_inputs_outside_source_require_a_build(self):
        gate=module(INVENTORY/'merge_gate.py','merge_inputs_unit')
        for filename in ('RTEA.vcxproj','meson.build','external/dependency.cpp'):
            with self.subTest(filename=filename),tempfile.TemporaryDirectory() as folder:
                root=Path(folder);tree=root/'repo';tree.mkdir()
                def git(*args):return subprocess.check_output(['git','-C',str(tree),*args],stderr=subprocess.DEVNULL,text=True).strip()
                git('init','-b',gate.WAVE);git('config','user.name','Test');git('config','user.email','test@example.invalid')
                (tree/'base.txt').write_text('base');git('add','.');git('commit','-m','Base');base=git('rev-parse','HEAD')
                path=tree/filename;path.parent.mkdir(exist_ok=True);path.write_text('compile input')
                git('add','.');git('commit','-m','Compile input');tip=git('rev-parse','HEAD');git('reset','--hard',base)
                with patch.object(gate,'check_report',return_value=(root/'report','RUNS\nfixture\n')),patch.object(gate,'MX',root/'mx'),contextlib.redirect_stdout(io.StringIO()):
                    code=gate.main([str(tree),tip,'--merge-number','1','--lane','unit','--no-build','--dry-run','--keep-scratch'])
                self.assertEqual(code,3,filename+' was allowed without a build')

    def test_t11_inventory_json_and_merge_gate_use_lf(self):
        for name in ('boxes.json','merge_gate.py'):
            with self.subTest(name=name):self.assertFalse(b'\r\n' in (INVENTORY/name).read_bytes(),name+' contains CRLF')

    def test_t12_both_relay_suites_are_registered(self):
        suites=dict(run_tools_suites.SUITES)
        self.assertEqual(suites.get('relay-gate'),['relay_gate_test.py'])
        self.assertEqual(suites.get('relay-cloudflare'),['relay_cloudflare_test.py'])

if __name__=='__main__':unittest.main()
