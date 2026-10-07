"""Deployment writes keep their declared boundaries and refuse occupied targets."""
import base64
import hashlib
import io
from pathlib import Path
import tarfile
import tempfile
import unittest

from test_relay_build_inputs import module
from test_inventory_oracle_evidence import INVENTORY


class DeployedRefresh(unittest.TestCase):
    def test_native_install_hashes_bytes_and_refuses_an_occupied_tree(self):
        helper=module(INVENTORY/'deployed_refresh.py','deployed_refresh_unit')
        namespace={};exec(helper.REMOTE,namespace)
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);payload=io.BytesIO();data=b'current helper\n'
            with tarfile.open(fileobj=payload,mode='w') as archive:
                info=tarfile.TarInfo('nested/helper.py');info.size=len(data);archive.addfile(info,io.BytesIO(data))
            packed=payload.getvalue()
            request=dict(action='install',repo=str(root),target=str(root/'native'),archive=base64.b64encode(packed).decode(),
                         sha256=hashlib.sha256(packed).hexdigest(),files={'nested/helper.py':hashlib.sha256(data).hexdigest()})
            namespace['guard']=lambda repo:dict(engines=[123],markers=[])
            with self.assertRaisesRegex(ValueError,'occupied'):namespace['run'](request)
            self.assertFalse((root/'native').exists())
            namespace['guard']=lambda repo:dict(engines=[],markers=[])
            self.assertTrue(namespace['run'](request)['passed'])
            self.assertEqual((root/'native/nested/helper.py').read_bytes(),data)
            request['sha256']='0'*64
            with self.assertRaisesRegex(ValueError,'payload hash'):namespace['run'](request)
            self.assertEqual((root/'native/nested/helper.py').read_bytes(),data)

    def test_declared_member_cannot_escape_the_native_artifact(self):
        helper=module(INVENTORY/'deployed_refresh.py','deployed_refresh_escape_unit')
        namespace={};exec(helper.REMOTE,namespace)
        namespace['guard']=lambda repo:dict(engines=[],markers=[])
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);payload=io.BytesIO();data=b'escape'
            with tarfile.open(fileobj=payload,mode='w') as archive:
                info=tarfile.TarInfo('../outside.py');info.size=len(data);archive.addfile(info,io.BytesIO(data))
            packed=payload.getvalue()
            request=dict(action='install',repo=str(root),target=str(root/'native'),archive=base64.b64encode(packed).decode(),
                         sha256=hashlib.sha256(packed).hexdigest(),files={'../outside.py':hashlib.sha256(data).hexdigest()})
            with self.assertRaisesRegex(ValueError,'leaves the registered artifact'):namespace['run'](request)
            self.assertFalse((root/'outside.py').exists())


if __name__=='__main__':unittest.main()
