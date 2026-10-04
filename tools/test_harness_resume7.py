"""Five credential sink counterexamples, synthetic pairs in temporary directories only."""
import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock,patch
import uuid

import e2e_video as video
import edith_cross as cross
import acceptance_relay_policy as policy
import turn_relay_rows as turn
from feel import relay_join

def pair():return 'fixture-'+uuid.uuid4().hex,uuid.uuid4().hex+uuid.uuid4().hex
def has_login(path,login):return any(value.encode() in path.read_bytes() for value in login)

class RelayNeverPersists(unittest.TestCase):
    def test_t1_menu_staging_keeps_placeholders_not_login_values(self):
        login=pair()
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            with patch.object(video,'scenario_text',return_value='set_text TextNetworkRelayUser {TURN_USER}\nset_text TextNetworkRelayPass {TURN_PASS}\n'),patch.object(video,'find_ffmpeg',return_value=None):
                video.stage_peer({'checklist':[]},{'name':'host','menu_script':'private.menu.txt'},root,dict(TURN_USER=login[0],TURN_PASS=login[1]))
            self.assertFalse(has_login(root/'menu.txt',login),'menu.txt/menu substitutions contain a login component')
            self.assertIn('{TURN_USER}',(root/'menu.txt').read_text())
            self.assertIn('{TURN_PASS}',(root/'menu.txt').read_text())

    def test_t2_selftest_receipts_never_copy_either_login_component(self):
        login=pair()
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)/'row';seen=[]
            def make_run(repo,args,out,timeout,env):
                seen.append(env);out.mkdir(parents=True)
                (out/'stdout.log').write_text('[net-p2p-selftest] hold user='+login[0]+'\n[net-p2p-selftest] PASS\n')
                (out/'launch.json').write_text(json.dumps(dict(env=env)))
                return SimpleNamespace(start=lambda:None,finish=lambda:{'exit_code':0,'timed_out':False},close=lambda:None)
            with patch.dict(os.environ,{},clear=False),patch.object(sys,'argv',['turn','hold','--turn','localhost:3478','--out',str(root)]),patch.object(turn,'read_login',return_value=login),patch.object(turn,'make_run',side_effect=make_run),patch.object(turn.time,'sleep'),contextlib.redirect_stdout(io.StringIO()):
                code=turn.main()
            self.assertFalse(has_login(root/'result.json',login),'result.json/copied hold line contains a login component')
            self.assertFalse(has_login(root/'engine/launch.json',login),'launch.json/explicit environment contains a login component')
            self.assertFalse(has_login(root/'engine/stdout.log',login),'stdout.log/raw GNS username was not swept')
            self.assertEqual(code,1,'an observed native credential leak must remain a failed run')
            self.assertFalse(any(key in env for env in seen for key in ('CC_TEST_TURN_USER','CC_TEST_TURN_PASS')))

    def test_t3_compare_finally_leaves_no_login_in_seeded_settings(self):
        login=pair()
        class EndAfterStage(BaseException):pass
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)/'compare'
            def make_run(repo,args,out,timeout,env):
                cwd=out/'runtime';(cwd/'Userdata').mkdir(parents=True)
                def stop():raise EndAfterStage()
                return SimpleNamespace(cwd=cwd,out=out,start=stop,close=lambda:None)
            def seed(cwd,settings):
                (Path(cwd)/'Userdata/Settings.ini').write_text('\n'.join(key+'='+str(value) for key,value in settings.items()))
            @contextlib.contextmanager
            def directory(*args,**kwargs):
                book=kwargs.get('secret_book')
                if book:book.add_offer({'iceServers':[dict(username=login[0],credential=login[1])]})
                yield dict(DIRECTORY_PIN='pin',DIRECTORY_URL='127.0.0.1:49492')
            with patch.dict(os.environ,CC_TEST_TURN_USER=login[0],CC_TEST_TURN_PASS=login[1]),patch.object(sys,'argv',['compare','--turn','turn:localhost:3478','--out',str(root)]),patch.object(relay_join,'make_run',side_effect=make_run),patch.object(relay_join,'patch_settings',side_effect=seed),patch.object(relay_join,'stage_baseline'),patch.object(relay_join,'start_service',return_value=Mock()),patch.object(relay_join,'require_turn_reachable',return_value=0),patch.object(relay_join.threading,'Thread',return_value=Mock()),patch('e2e.directory.serve',side_effect=directory),patch.object(policy,'directory_config',return_value={'backend':'coturn','static_auth_secret':'fixture-only'}):
                with self.assertRaises(EndAfterStage):relay_join.main()
            self.assertFalse(has_login(root/'host/runtime/Userdata/Settings.ini',login),'Settings.ini/relay compare seed contains a login component after finally')

    def test_t4_cross_relay_settings_use_directory_without_a_login(self):
        login=pair()
        settings=cross.network_settings('relay','here','pin',login)
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'Settings.ini';path.write_text('\n'.join(key+'='+str(value) for key,value in settings.items()))
            self.assertFalse(has_login(path,login),'remote Settings.ini/fixed relay fields contain a login component')
        self.assertEqual(settings['NetworkHostRelayMode'],'Directory')

    def test_t5_empty_secret_book_cannot_pass_a_public_row(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);(root/'native.log').write_text('a public directory match was recorded\n')
            scan=policy.scan_retained(root,policy.CredentialBook())
            self.assertFalse(scan['passed'],'secret-scan.json/empty book falsely certifies a public row')

if __name__=='__main__':unittest.main()
