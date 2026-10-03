"""Native session interfaces using fake runners, with no engines or network calls."""
import base64
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

import acceptance_peer_session as sessions
import acceptance_e2e_remote as remote_video
import acceptance_remote as dispatch
import run_sim_test
import e2e_video


class AcceptancePeerSession(unittest.TestCase):
    def test_credentials_use_the_pipe_and_are_absent_from_task_payloads(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            pair=sessions.Pair.__new__(sessions.Pair)
            box=SimpleNamespace(name='ALLY',repo='D:/inventory',exe='D:/inventory/game.exe')
            pair.boxes={'host':(box,Mock(),Mock(),root)}
            pair.source='a'*40;pair.executable='b'*64;pair.cid='one';pair.repo=root/'repo';pair.root=root
            pair.deliveries=[]
            channel=SimpleNamespace(address=r'\\.\pipe\unit')
            with patch.object(sessions,'Delivery',return_value=channel) as delivery, \
                 patch.object(dispatch,'task_payload',return_value=root/'done') as task:
                pair.launch('host',root/'native','relay-peer',dict(settings={}),dict(password='PRIVATE UNIT VALUE'))
            self.assertEqual(delivery.call_args.args[2],dict(password='PRIVATE UNIT VALUE'))
            payload=task.call_args.args[4]
            self.assertNotIn('PRIVATE UNIT VALUE',json.dumps(payload))
            self.assertEqual(payload['values_pipe'],channel.address)

    def test_native_capture_starts_only_through_make_run_and_retains_the_real_record(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);out=root/'host';identity=root/'.sessions/host/identity.json'
            native=dict(root=str(root),out=str(out),role='host',repo=str(root/'repo'),argv=['-unit'],environment={},timeout=10)
            values=dict(files={'host-stage/menu.txt':'__TOKEN__', 'host/runtime/Userdata/Settings.ini':'SettingsMan\n'},secrets={'__TOKEN__':'private'})
            def factory(repo,args,directory,timeout,env):
                self.assertEqual(args,['-unit'])
                self.assertEqual(env['CCCP_TEST_RECORD_ENCODER'],'native-ffmpeg')
                directory.mkdir();runtime=directory/'runtime';(runtime/'Userdata').mkdir(parents=True)
                handle=SimpleNamespace(cwd=runtime,record=dict(exit_code=0,exe_sha256='c'*64),
                    start=Mock(),poll=Mock(return_value=0),finish=Mock(return_value=dict(exit_code=0,exe_sha256='c'*64)),close=Mock())
                return handle
            with patch.object(run_sim_test,'make_run',side_effect=factory) as launch, \
                 patch.object(e2e_video,'find_ffmpeg',return_value='native-ffmpeg'), \
                 patch.object(e2e_video,'encoder_codec',return_value='native-codec'):
                self.assertEqual(remote_video.execute_peer(dict(native=native,identity=str(identity)),values),0)
            launch.assert_called_once()
            self.assertEqual(json.loads((out/'record.json').read_text())['exe_sha256'],'c'*64)

    def test_peer_gate_exchange_rejects_an_escape_and_preserves_native_bytes(self):
        value=base64.b64encode(b'{"pass":false}').decode()
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            with self.assertRaises(ValueError): sessions.publish(root,{'../done.json':value})
            sessions.publish(root,{'host-stage/probe/done.json':value})
            self.assertEqual((root/'host-stage/probe/done.json').read_bytes(),b'{"pass":false}')


if __name__ == '__main__':
    unittest.main()
