"""Deployment currency must stop a merge even when it skips a build."""
import argparse
import contextlib
import hashlib
import io
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from test_harness_resume6 import module
from test_inventory_oracle_evidence import INVENTORY


class DeployedRegister(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.repo=self.root/'wave';self.repo.mkdir()
        def git(*args):return subprocess.check_output(['git','-C',str(self.repo),*args],stderr=subprocess.DEVNULL,text=True).strip()
        self.git=git
        git('init');git('config','user.name','Test');git('config','user.email','test@example.invalid')
        git('commit','--allow-empty','-m','Deployment fixture base')
        (self.repo/'artifact.txt').write_bytes(b'current\n');git('add','.');git('commit','-m','Current deployment input')
        git('branch','-M','stage2/fixgroup-6-lead-wave-a');self.head=git('rev-parse','HEAD')
        self.native=self.root/'native.txt';self.native.write_bytes(b'current\n')
        self.register=self.root/'register.json';self.state=self.root/'state.json'
        self.row=dict(id='fixture-file',box='fixture',path=str(self.native),transport=dict(kind='local'),
            source=dict(kind='wave-file',path='artifact.txt'),
            expected=dict(wave_sha=self.head,sha256=hashlib.sha256(b'current\n').hexdigest()),
            refresh=dict(argv=['python','-c','pass']),proof=dict(kind='hash',command='hash the native artifact'))
        self.save()

    def save(self):
        self.register.write_text(json.dumps(dict(schema=1,wave_repo=str(self.repo),wave_sha=self.head,rows=[self.row])))

    def checker(self):
        path=INVENTORY/'deployed_check.py'
        self.assertTrue(path.is_file(),'the deployed artifact currency checker is absent')
        return module(path,'deployed_register_unit')

    def check(self,checker):
        output=io.StringIO()
        with contextlib.redirect_stdout(output):
            code=checker.main(['--register',str(self.register),'--state',str(self.state),'--wave-repo',str(self.repo)])
        return code,output.getvalue()

    def test_hash_difference_is_named_stale_and_preserves_first_seen(self):
        checker=self.checker();self.native.write_bytes(b'old\n')
        first,text=self.check(checker)
        self.assertEqual(first,1);self.assertIn('fixture-file',text);self.assertIn('stale since',text)
        original=json.loads(self.state.read_text())['rows']['fixture-file']['stale_since']
        second,_=self.check(checker)
        self.assertEqual(second,1)
        self.assertEqual(json.loads(self.state.read_text())['rows']['fixture-file']['stale_since'],original)

    def test_stopped_directory_cannot_pass_a_round_trip(self):
        checker=self.checker()
        with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
        self.row['id']='fixture-directory';self.row['proof']=dict(kind='directory-round-trip',url=f'http://127.0.0.1:{port}',
            registration=dict(network_protocol_version=5,lockstep_codec_version=1,controller_frame_version=1),command='create/list/delete')
        self.save();code,text=self.check(checker)
        self.assertEqual(code,1);self.assertIn('fixture-directory',text);self.assertIn('round trip',text)
        self.assertIn('stale since',text)

    def test_all_current_artifacts_exit_zero(self):
        checker=self.checker();code,text=self.check(checker)
        self.assertEqual(code,0);self.assertIn('fixture-file: current',text);self.assertIn('boxes current: yes',text)

    def test_custom_register_state_stays_beside_that_register(self):
        checker=self.checker();outside=self.root/'unrelated-state.json'
        with patch.object(checker,'DEFAULT_STATE',outside),contextlib.redirect_stdout(io.StringIO()):
            code=checker.main(['--register',str(self.register),'--wave-repo',str(self.repo)])
        self.assertEqual(code,0)
        self.assertFalse(outside.exists(),'a custom deployment register wrote another register state')
        self.assertTrue((self.register.parent/'deployed_status.json').is_file())

    def test_native_git_text_endings_do_not_hide_a_content_or_binary_change(self):
        checker=self.checker();tools=self.repo/'tools';tools.mkdir();(tools/'sample.py').write_bytes(b'x=1\n')
        self.git('add','.');self.git('commit','-m','Tracked text fixture')
        clone=self.root/'native-clone'
        subprocess.check_output(['git','clone','-q','-c','core.autocrlf=true',str(self.repo),str(clone)],stderr=subprocess.DEVNULL)
        native=clone/'tools';target=native/'sample.py';target.write_bytes(b'x=1\r\n')
        self.row.update(path=str(native),engine_tree=str(clone),source=dict(kind='wave-tree',path='tools',format='git',eol_mode='native-git-text'))
        self.save();code,_=self.check(checker)
        self.assertEqual(code,0,'a native Git text newline convention was called stale')
        target.write_bytes(b'x=2\r\n');self.assertEqual(self.check(checker)[0],1)
        target.write_bytes(b'x=1\0\r\n');self.assertEqual(self.check(checker)[0],1)

    def test_no_build_merge_gate_cannot_pass_a_stale_artifact(self):
        self.native.write_bytes(b'old\n')
        gate=module(INVENTORY/'merge_gate.py','deployed_merge_unit')
        options=argparse.Namespace(tree=self.repo,sha=self.head,merge_number=1,lane='fixture',dry_run=False,brief=None,
            no_build=True,suite=False,mac_gates=False,exe=None,keep_scratch=True,deployed_register=self.register)
        executed=[]
        def run(argv,**kwargs):
            executed.append(argv)
            if any(str(arg).endswith('deployed_check.py') for arg in argv):
                result=subprocess.run(argv,capture_output=True,text=True)
                if result.returncode:raise gate.GateError('deployed artifact check failed')
                return result
            return subprocess.CompletedProcess(argv,0)
        with patch.object(gate,'check_report',return_value=(self.root/'REPORT.txt','RUNS REQUESTED\nfixture\n')),patch.object(gate,'run',side_effect=run),patch.object(gate,'MX',self.root/'scratch'),contextlib.redirect_stdout(io.StringIO()):
            try:code=gate.gate(options)
            except gate.GateError:code=3
        self.assertNotEqual(code,0,'--no-build passed without proving the stale deployed artifact')
        self.assertTrue(any(any(str(arg).endswith('deployed_check.py') for arg in argv) for argv in executed))

    def test_wave_input_change_during_native_proof_cannot_pass(self):
        checker=self.checker();original=checker.prove
        def changed(*args,**kwargs):
            result=original(*args,**kwargs)
            (self.repo/'artifact.txt').write_bytes(b'changed during proof\n')
            return result
        with patch.object(checker,'prove',side_effect=changed):code,text=self.check(checker)
        self.assertEqual(code,1,'a wave input changed while the check still claimed current')
        self.assertIn('changed during',text)

    def test_refresh_changed_reproves_the_native_bytes(self):
        import sys
        checker=self.checker()
        (self.repo/'artifact.txt').write_bytes(b'next\n');self.git('add','.');self.git('commit','-m','Next artifact')
        self.row['refresh']['argv']=[sys.executable,'-c',
            'import pathlib,sys;pathlib.Path(sys.argv[1]).write_bytes(pathlib.Path(sys.argv[2]).read_bytes())',
            str(self.native),str(self.repo/'artifact.txt')]
        self.save()
        argv=['--register',str(self.register),'--state',str(self.state),'--wave-repo',str(self.repo),'--refresh-changed']
        with contextlib.redirect_stdout(io.StringIO()):code=checker.main(argv)
        self.assertEqual(code,0);self.assertEqual(self.native.read_bytes(),b'next\n')
        self.assertEqual(json.loads(self.register.read_text())['rows'][0]['expected']['wave_sha'],self.git('rev-parse','HEAD'))
        (self.repo/'artifact.txt').write_bytes(b'third\n');self.git('add','.');self.git('commit','-m','Third artifact')
        data=json.loads(self.register.read_text());data['rows'][0]['refresh']['argv']=[sys.executable,'-c','pass']
        self.register.write_text(json.dumps(data))
        with contextlib.redirect_stdout(io.StringIO()):code=checker.main(argv)
        self.assertEqual(code,1,'a successful refresh command hid unchanged native bytes')

    def test_native_receipt_can_carry_only_an_identical_build_input_commit(self):
        checker=self.checker()
        source=self.repo/'Source';source.mkdir();(source/'engine.cpp').write_bytes(b'one\n')
        self.git('add','.');self.git('commit','-m','Compiled input')
        compiled=self.git('rev-parse','HEAD');receipt=self.root/'build.json'
        receipt.write_text(json.dumps(dict(commit=compiled,executable_sha256=hashlib.sha256(self.native.read_bytes()).hexdigest())))
        self.row.update(source=dict(kind='native-build',receipt=str(receipt)),
                        proof=dict(kind='build-identity',receipt=str(receipt),executable=str(self.native),command='native build identity'))
        (self.repo/'artifact.txt').write_bytes(b'new harness\n');self.git('add','.');self.git('commit','-m','Harness only')
        self.save();code,_=self.check(checker)
        self.assertEqual(code,0,'an identical compiled input was treated as a stale binary after a harness-only merge')
        (source/'engine.cpp').write_bytes(b'two\n');self.git('add','.');self.git('commit','-m','Engine change')
        self.assertEqual(self.check(checker)[0],1,'a receipt for different compiled input passed')

    def test_build_receipt_requires_a_new_matching_successful_ledger(self):
        gate=module(INVENTORY/'merge_gate.py','deployed_ledger_unit')
        out=self.root/'build';out.mkdir();exe=self.repo/'Cortex Command.exe';exe.write_bytes(b'binary fixture')
        self.git('add','.');self.git('commit','-m','Native build fixture')
        head=self.git('rev-parse','HEAD');ledger=out/'exe-ledger.jsonl'
        entry=dict(head_full=head,repo=str(self.repo),label='fixture',configuration='Final',dirty_files=0,
                   exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest())
        (out/'build-fixture.log').write_text('build completed\n')
        ledger.write_text(json.dumps(entry)+'\n')
        with self.assertRaises(gate.GateError):gate.publish_build_receipt(self.repo,out,'fixture',ledger.read_bytes())
        wrong=dict(entry,exe_sha256='0'*64);ledger.write_text(json.dumps(wrong)+'\n')
        with self.assertRaises(gate.GateError):gate.publish_build_receipt(self.repo,out,'fixture',b'')
        self.assertFalse((self.repo/'tools/cross_peers/build.json').exists())
        ledger.write_text(json.dumps(entry)+'\n')
        receipt=gate.publish_build_receipt(self.repo,out,'fixture',b'')
        self.assertEqual(receipt['commit'],head);self.assertEqual(receipt['executable_sha256'],entry['exe_sha256'])

    def test_held_box_has_no_native_probe_or_refresh(self):
        checker=self.checker();boxes=self.root/'boxes.json'
        boxes.write_text(json.dumps(dict(boxes=[dict(name='fixture',optional_note='HELD BY THE USER')])) )
        with patch.object(checker,'native_probe',side_effect=AssertionError('held box contacted')),\
                patch.object(checker,'refreshed',side_effect=AssertionError('held box refreshed')),\
                contextlib.redirect_stdout(io.StringIO()) as output:
            code=checker.main(['--register',str(self.register),'--state',str(self.state),'--wave-repo',str(self.repo),
                               '--boxes',str(boxes),'--refresh-stale','--refresh-changed'])
        self.assertEqual(code,0);self.assertIn('HELD BY THE USER',output.getvalue())
        self.assertFalse(json.loads(self.state.read_text())['rows']['fixture-file']['contacted'])


if __name__=='__main__':unittest.main()
