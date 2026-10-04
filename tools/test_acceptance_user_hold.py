"""A declared user hold blocks reachability probes, preparation and refresh before SSH."""
import contextlib
import io
import json
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock,patch

import acceptance_remote as dispatch
from test_inventory_oracle_evidence import INVENTORY,run_split
from test_harness_resume6 import module


class UserHeldBox(unittest.TestCase):
    def held(self):
        boxes,_=run_split.load_manifest(INVENTORY/'boxes.json')
        box=next(box for box in boxes if box.name=='Z13')
        box.optional_note='HELD BY THE USER; no route is authorized'
        return box

    def test_optional_probe_never_contacts_a_user_held_box(self):
        box=self.held()
        with patch.object(run_split.subprocess,'run',return_value=SimpleNamespace(returncode=0,stdout=box.hostname)) as command:
            result=run_split.probe_optional_boxes([box])
        self.assertFalse(result['Z13'],'a user-held box was treated as available')
        command.assert_not_called()

    def test_prepared_or_new_peer_cannot_contact_a_user_held_box(self):
        box=self.held();remote=Mock();remote.reachable.return_value=None
        factory=Mock(return_value=remote);rb=SimpleNamespace(RemoteBox=factory)
        with tempfile.TemporaryDirectory() as folder,patch.object(dispatch,'inventory_modules',return_value=(run_split,rb)),patch.object(run_split,'environment_problem',return_value=None),patch.object(run_split,'ship_inputs',return_value={'exe_sha256_there':'b'}),patch.object(run_split,'sync_git',return_value={'head_there':'a'}):
            root=Path(folder)
            with self.assertRaisesRegex(RuntimeError,'HELD BY THE USER'):
                dispatch.prepare_box(root,box,root/'collection',INVENTORY,'a','b')
        factory.assert_not_called()

    def test_dry_refresh_reports_hold_without_contacting_box(self):
        refresh=module(INVENTORY/'refresh_windows.py','refresh_held_unit')
        with tempfile.TemporaryDirectory() as folder:
            manifest=Path(folder)/'boxes.json'
            manifest.write_text(json.dumps(dict(boxes=[dict(name='Z13',optional_note='HELD BY THE USER; no route is authorized')])))
            state=dict(head='a'*40,differing_files=[],line_ending_only=[],untracked_tools=[])
            output=io.StringIO()
            with patch.object(refresh,'MANIFEST',manifest,create=True),patch.object(refresh,'remote_call',return_value=state) as call,contextlib.redirect_stdout(output):
                code=refresh.main(['--box','Z13','--dry-run'])
            self.assertEqual(code,3,'a held-box refresh must remain AWAITING')
            self.assertIn('HELD BY THE USER',output.getvalue());call.assert_not_called()

if __name__=='__main__':unittest.main()
