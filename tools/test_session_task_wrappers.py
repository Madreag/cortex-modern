"""Task-wrapper detectors use private folders and mock tasks; no engine starts."""
import contextlib
import base64
import json
import os
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from edith import remote_box as remote
import spread_peers as spread


class SessionWrapperTests(unittest.TestCase):
    def slot(self, root):
        return dict(task='fake-task', slot=1, preserve_runner=True,
                    wrapper=str(root/'run.ps1'), payload=str(root/'command.ps1'),
                    owner_marker=str(root/'payload-owner.json'))

    def test_foreign_wrapper_refuses_by_box_task_and_path(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            slot = self.slot(root)
            # A comment naming the payload is not the native runner.
            body = b'# command.ps1\r\nWrite-Output foreign\r\n'
            Path(slot['wrapper']).write_bytes(body)
            reason = remote.wrapper_refusal(dict(name='NAMED', free_floor_gb=6), slot)
            self.assertEqual(reason, f'FOREIGN TASK WRAPPER: NAMED fake-task {slot["wrapper"]}')
            self.assertEqual(Path(slot['wrapper']).read_bytes(), body)
            self.assertFalse(spread.transient_admission(reason))

    def test_native_submission_refuses_before_changing_the_request_or_payload(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            slot = self.slot(root)
            Path(slot['wrapper']).write_bytes(b'foreign payload')
            Path(slot['payload']).write_bytes(b'unchanged command')
            request = root/'request.json'
            request.write_text('{"claim":{"token":"owned"}}')
            before = {p.name:p.read_bytes() for p in root.iterdir()}
            box = dict(name='NAMED', requested_task_slot=1, free_floor_gb=6)
            claim = dict(token='owned', needs=dict(peer_id='host'), runner_wait=60)
            worker = dict(facts=SimpleNamespace(read_reservation=lambda *a, **k:None),
                          root_for=lambda box:root, mutex=lambda *a, **k:contextlib.nullcontext(),
                          task_slots=lambda box:spread.named_task_slots([dict(slot, state='Unavailable')], box))
            original = unittest.mock.Mock()
            pool = SimpleNamespace(Needs=lambda **k:SimpleNamespace(**k), live_reason=lambda *a:None)
            with patch.dict('sys.modules', pool=pool), self.assertRaisesRegex(spread.SpreadRefusal, 'FOREIGN TASK WRAPPER: NAMED fake-task'):
                spread.submit_named_task(original, dict(box=box, claim=claim, request=str(request)), worker)
            original.assert_not_called()
            self.assertEqual(before, {p.name:p.read_bytes() for p in root.iterdir()})

    def test_catalog_preserve_flag_follows_the_named_task(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            registry = root/'boxes.json'
            slots = [dict(self.slot(root), task='chosen'), dict(self.slot(root), task='other', preserve_runner=False)]
            registry.write_text(json.dumps(dict(boxes=[dict(name='NAMED', ssh='saved-alias', task_slots=slots)])))
            box = remote.RemoteBox('saved-alias', task='chosen', pool_registry=registry)
            self.assertTrue(box.catalog_slot()['preserve_runner'])
            self.assertEqual(box.catalog_slot()['task'], 'chosen')

    def test_preserved_direct_start_refuses_foreign_wrapper_before_shipping(self):
        box = remote.RemoteBox('saved-alias', task='fake-task', preserve_runner=False)
        slot = dict(task=box.task, wrapper=box.session_script, payload='command.ps1',
                    owner_marker='payload-owner.json', preserve_runner=True, box_name='NAMED')
        with patch.object(box, 'catalog_slot', return_value=slot), \
             patch.object(box, 'wait_task_idle'), patch.object(box, 'scp_to') as ship, \
             patch.object(box, 'ssh', return_value=json.dumps(dict(wrapper=base64.b64encode(b'foreign').decode(), command=True))):
            with self.assertRaisesRegex(RuntimeError, 'FOREIGN TASK WRAPPER: NAMED fake-task'):
                box.start_task(Path('not-shipped.ps1'))
        ship.assert_not_called()

    def test_nonpreserved_direct_start_keeps_the_original_install_call(self):
        box = remote.RemoteBox('saved-alias', task='fake-task')
        with patch.object(box, 'catalog_slot', return_value=dict(preserve_runner=False)), \
             patch.object(box, 'wait_task_idle') as idle, patch.object(box, 'scp_to') as ship, \
             patch.object(box, 'ssh') as launch:
            box.start_task(Path('payload.ps1'), budget_s=123)
        idle.assert_called_once_with(123)
        ship.assert_called_once_with(Path('payload.ps1'), box.session_script)
        launch.assert_called_once_with('Start-ScheduledTask -TaskName fake-task')

    def test_restore_never_changes_a_foreign_payload_marker(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            slot = self.slot(root)
            for name, data in (('run.ps1', b'wrapper'), ('command.ps1', b'foreign command'),
                               ('payload-owner.json', b'{"token":"another-owner"}')):
                (root/name).write_bytes(data)
            before = {p.name:p.read_bytes() for p in root.iterdir()}
            if os.name != 'nt':
                self.skipTest('native PowerShell transaction is Windows-specific')
            result = self.ps(remote.task_restore_script(slot, 'our-token', abort=True), expected=1)
            self.assertIn('DIRECT TASK OWNER CHANGED', result.stderr)
            self.assertEqual(before, {p.name:p.read_bytes() for p in root.iterdir()})

    @unittest.skipUnless(os.name == 'nt', 'native PowerShell transaction is Windows-specific')
    def test_payload_end_restores_wrapper_and_previous_command_byte_for_byte(self):
        self.transaction(abort=False)

    @unittest.skipUnless(os.name == 'nt', 'native PowerShell transaction is Windows-specific')
    def test_payload_abort_restores_wrapper_and_previous_command_byte_for_byte(self):
        self.transaction(abort=True)

    def transaction(self, *, abort):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            slot = self.slot(root)
            wrapper = b'\xef\xbb\xbf'+remote.session_wrapper(6).replace('\n', '\r\n').encode('utf-8')
            previous = b'\xef\xbb\xbf# previous command\r\n'
            Path(slot['wrapper']).write_bytes(wrapper)
            Path(slot['payload']).write_bytes(previous)
            token = 'only-this-fixture'
            payload = root/('.direct-script-'+token+'.ps1')
            payload.write_text("Write-Output 'direct payload finished'\nexit 7\n")
            slot['wrapper_sha256'] = remote.sha256_file(Path(slot['wrapper']))
            state = remote.ps_quote(str(root/'fake-running'))
            prefix = ("function Get-ScheduledTask {param($TaskName) [pscustomobject]@{State=if(Test-Path -LiteralPath "+state+"){ 'Running' }else{ 'Ready' }}}\n"
                      "function Start-ScheduledTask {param($TaskName) "+
                      ("Set-Content -LiteralPath "+state+" -Value 'owned fixture'" if abort else "& pwsh -NoProfile -NonInteractive -File "+remote.ps_quote(slot['payload']))+"}\n"
                      "function Stop-ScheduledTask {param($TaskName) Remove-Item -LiteralPath "+state+"}\n")
            self.ps(prefix+remote.task_install_script(slot, token, str(payload), 10))
            self.assertEqual(Path(slot['wrapper']).read_bytes(), wrapper)
            if abort:
                self.assertTrue(Path(slot['owner_marker']).is_file())
                self.ps(prefix+remote.task_restore_script(slot, token, abort=True))
            self.assertEqual(Path(slot['wrapper']).read_bytes(), wrapper)
            self.assertEqual(Path(slot['payload']).read_bytes(), previous)
            self.assertFalse(Path(slot['owner_marker']).exists())
            self.assertEqual(sorted(p.name for p in root.iterdir()), ['command.ps1', 'run.ps1'])

    def ps(self, script, expected=0):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)/'fake-task.ps1'
            path.write_text(script, encoding='utf-8')
            result = subprocess.run(['pwsh', '-NoProfile', '-NonInteractive', '-File', str(path)],
                                    text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, expected, result.stdout+result.stderr)
        return result


if __name__ == '__main__':
    unittest.main()
