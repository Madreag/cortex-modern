"""Additional persistence boundaries; real pipe I/O uses Python only, never an engine."""
import json
import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import uuid
import time

import e2e_video as video
import run_sim_test

class PrivateInputEdges(unittest.TestCase):
    def test_capture_staging_and_metadata_never_write_private_input(self):
        from acceptance_relay_policy import CredentialBook
        login=('fixture-'+uuid.uuid4().hex,uuid.uuid4().hex)
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);received=[];writes=[]
            original_text,original_bytes=Path.write_text,Path.write_bytes
            def check(path,data):
                self.assertFalse(any(value.encode() in data for value in login),path.name+'/prospective harness write contains a login')
                writes.append(path.name)
            def write_text(path,text,*args,**kwargs):
                check(path,text.encode());return original_text(path,text,*args,**kwargs)
            def write_bytes(path,data,*args,**kwargs):
                check(path,data);return original_bytes(path,data,*args,**kwargs)
            def factory(repo,args,out,timeout,env):
                self.assertTrue(os.environ.get('CC_TEST_TURN_USER')==login[0],'runner construction did not inherit the private environment')
                cwd=out/'runtime';(cwd/'Userdata').mkdir(parents=True)
                (cwd/'Userdata/Settings.ini').write_text('SettingsMan\n')
                (out/'runtime.json').write_text(json.dumps(dict(settings_overrides={})))
                handle=SimpleNamespace(cwd=cwd,out=out,argv=['python-reader',*args],close=lambda:None,poll=lambda:0)
                def start():
                    with open(handle.argv[handle.argv.index('-menu-script')+1],'rb') as incoming:received.append(incoming.read())
                    self.assertTrue(os.environ.get('CC_TEST_TURN_USER')==login[0],'private inherited launch environment differed')
                    (cwd/'LogConsole.txt').write_text('native stand-in complete\n')
                handle.start=start;handle.finish=lambda:dict(exit_code=0)
                return handle
            options=SimpleNamespace(dry_run=False,size='640x360',port=49400,fps=30,repo=root/'repo',
                tokens=dict(TURN_USER=login[0],TURN_PASS=login[1]),setting=[],render_cap=60,scratch_root=root,relay_book=CredentialBook())
            scenario=dict(path='fixture.json',checklist=[])
            run=dict(name='fixed',peers=[dict(name='host',menu_script='fixture.txt',
                settings=dict(NetworkTurnUser='{TURN_USER}',NetworkTurnPass='{TURN_PASS}'),env=dict(CC_TEST_TURN_USER='{TURN_USER}'))])
            with patch.object(video,'make_run',side_effect=factory),patch.object(video,'scenario_text',return_value='set_text TextNetworkRelayUser {TURN_USER}\nset_text TextNetworkRelayPass {TURN_PASS}\n'),patch.object(video,'find_ffmpeg',return_value=None),patch.object(video,'note_footprint',return_value=0),patch.object(Path,'write_text',write_text),patch.object(Path,'write_bytes',write_bytes):
                result=video.run_one(options,scenario,run,0,root/'capture')
                video.write_json(root/'capture.json',dict(scenario_definition=run,runs=[result],command=['--token','TURN_USER='+login[0]]))
            self.assertTrue(received and all(value.encode() in received[0] for value in login),'the private input stream lost a component')
            self.assertTrue({'menu.txt','Settings.ini','runtime.json','capture.json'}<=set(writes))

    def test_runtime_copy_clears_inherited_personal_relay_before_first_write(self):
        login=('fixture-'+uuid.uuid4().hex,uuid.uuid4().hex)
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);repo=root/'repo';(repo/'Userdata').mkdir(parents=True);out=root/'run';out.mkdir()
            (repo/'Userdata/Settings.ini').write_text('SettingsMan\nNetworkTurnUser='+login[0]+'\nNetworkTurnPass='+login[1]+'\n')
            with patch.object(run_sim_test.subprocess,'run'):
                runtime=run_sim_test.prepare_runtime(repo,out)
            self.assertFalse(any(value in (runtime/'Userdata/Settings.ini').read_text() for value in login),
                             'Settings.ini/inherited personal relay was copied into a private runtime')

    def test_dry_capture_never_serializes_login_settings(self):
        login=('fixture-'+uuid.uuid4().hex,uuid.uuid4().hex)
        with tempfile.TemporaryDirectory() as folder:
            options=SimpleNamespace(dry_run=True,size='640x360',port=49400,fps=30,repo=Path(folder),
                tokens=dict(TURN_USER=login[0],TURN_PASS=login[1]),setting=[],render_cap=60)
            scenario=dict(path='fixture.json',checklist=[])
            run=dict(name='fixed',peers=[dict(name='host',menu_script='fixture.txt',settings=dict(NetworkTurnUser='{TURN_USER}',NetworkTurnPass='{TURN_PASS}'))])
            with patch.object(video,'scenario_text',return_value='set_text TextNetworkRelayUser {TURN_USER}\nset_text TextNetworkRelayPass {TURN_PASS}\n'):
                result=video.run_one(options,scenario,run,0,Path(folder)/'out')
            self.assertFalse(any(value in json.dumps(result) for value in login),'capture.json/dry settings contain a login component')

    @unittest.skipUnless(os.name=='nt','Windows named-pipe implementation')
    def test_menu_byte_stream_is_readable_without_a_staged_menu_file(self):
        from relay_private import MenuStream
        text='set_text TextNetworkRelayUser fixture-'+uuid.uuid4().hex+'\n'
        with MenuStream(text) as stream:
            with open(stream.path,'rb') as incoming:received=incoming.read()
            self.assertTrue(received==text.encode(),'engine-style byte reader received different menu bytes')
            self.assertFalse(Path(stream.path).is_file(),'the private input became a regular file')

    @unittest.skipUnless(os.name=='nt','Windows named-pipe implementation')
    def test_large_menu_stream_survives_a_slow_byte_reader(self):
        from relay_private import MenuStream
        data=('set_text TextNetworkRelayUser fixture-'+uuid.uuid4().hex+'\n')*15000
        with MenuStream(data) as stream:
            chunks=[]
            with open(stream.path,'rb',buffering=0) as incoming:
                while chunk:=incoming.read(4096):chunks.append(chunk);time.sleep(.0001)
            self.assertTrue(b''.join(chunks)==data.encode(),'the slow reader received incomplete private menu bytes')

if __name__=='__main__':unittest.main()
