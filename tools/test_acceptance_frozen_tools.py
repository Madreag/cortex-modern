"""Frozen tool identity and native adapter admission regressions; no engine runs."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import acceptance_frozen_tools as frozen
import acceptance_native_runtime as native


TIP = 'a'*40


class FrozenIdentity(unittest.TestCase):
    def setUp(self):
        saved = os.environ.pop(frozen.AUTHORIZED_ENV, None)
        if saved is not None:
            self.addCleanup(os.environ.__setitem__, frozen.AUTHORIZED_ENV, saved)

    def test_one_changed_frozen_byte_is_refused(self):
        data = b'unchanged exported tool\n'
        value = dict(frozen_commit=TIP, coordinator_commit=TIP,
                     files=[dict(path='tools/cross_peers.py', sha256=hashlib.sha256(data).hexdigest())])
        with patch.object(Path, 'is_file', return_value=True), \
                patch.object(Path, 'read_text', return_value=json.dumps(value)), \
                patch.object(Path, 'read_bytes', return_value=data):
            self.assertEqual(frozen.receipt('/virtual')['frozen_commit'], TIP)
        with patch.object(Path, 'is_file', return_value=True), \
                patch.object(Path, 'read_text', return_value=json.dumps(value)), \
                patch.object(Path, 'read_bytes', return_value=data+b' '):
            with self.assertRaisesRegex(ValueError, 'bytes differ'):
                frozen.receipt('/virtual')

    def test_an_export_other_than_the_authorized_commit_is_refused(self):
        older = json.dumps(dict(frozen_commit='f'*40, coordinator_commit=TIP, files=[]))
        with patch.object(Path, 'is_file', return_value=True), patch.object(Path, 'read_text', return_value=older):
            with self.assertRaisesRegex(ValueError, 'exports f{40} but the authorized export is a{40}'):
                frozen.receipt('/virtual')
            with self.assertRaisesRegex(ValueError, 'authorized export is b{40}'):
                frozen.receipt('/virtual', authorized='b'*40)
            self.assertEqual(frozen.receipt('/virtual', authorized='f'*40)['frozen_commit'], 'f'*40)
            with patch.dict(os.environ, {frozen.AUTHORIZED_ENV: 'f'*40}):
                self.assertEqual(frozen.receipt('/virtual')['frozen_commit'], 'f'*40)
        unnamed = json.dumps(dict(frozen_commit='f'*40, files=[]))
        with patch.object(Path, 'is_file', return_value=True), patch.object(Path, 'read_text', return_value=unnamed):
            with self.assertRaisesRegex(ValueError, 'authorized export is None'):
                frozen.receipt('/virtual')

    def test_an_export_of_the_coordinator_tip_composes_by_default(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); repo = root/'repo'; export = root/'export'
            def git(*args):
                return subprocess.check_output(['git', '-C', str(repo), *args], stderr=subprocess.DEVNULL, text=True).strip()
            (repo/'tools').mkdir(parents=True); (repo/'Source/Managers').mkdir(parents=True)
            (repo/'tools/cross_peers.py').write_bytes(b'coordinator\n')
            (repo/'Source/Managers/SettingsMan.h').write_bytes(b'constexpr const char *c_DefaultSessionDirectoryUrl = "d";\n')
            git('init', '-q'); git('config', 'user.name', 'Test'); git('config', 'user.email', 'test@example.invalid')
            git('add', '.'); git('commit', '-q', '-m', 'Tip'); tip = git('rev-parse', 'HEAD')
            (export/'tools').mkdir(parents=True); (export/'tools/cross_peers.py').write_bytes(b'coordinator\n')
            (export/'export.json').write_text(json.dumps(dict(source_commit=tip, files=[dict(path='tools/cross_peers.py',
                sha256=hashlib.sha256(b'coordinator\n').hexdigest())])))
            value = frozen.compose(repo, export, root/'bundle')
            self.assertEqual((value['frozen_commit'], value['coordinator_commit']), (tip, tip))
            self.assertEqual(frozen.receipt(root/'bundle')['coordinator_commit'], tip)
            self.assertEqual(sorted(entry['origin'] for entry in value['files']), ['driver-config', 'frozen'])
            with self.assertRaisesRegex(ValueError, 'the export is of '+tip+' but the authorized export is c{40}'):
                frozen.compose(repo, export, root/'other', 'c'*40)

    def test_retired_host_is_refused_before_even_a_capability_launch(self):
        payload = dict(box=dict(name='Z13',kind='windows-task',hostname='EROL-TABLET'), specs=[])
        with patch.object(Path, 'read_text', return_value=json.dumps(payload)), \
                patch.object(native.platform, 'node', return_value='EROL-PC'), \
                patch.object(native, 'read_capabilities') as caps:
            with self.assertRaisesRegex(ValueError, 'retired'):
                native.run_payload('/virtual/payload.json')
        caps.assert_not_called()

    def test_existing_private_overlay_settings_regression_holds_in_owned_adapter(self):
        import test_acceptance_private_runtime as previous
        with patch.object(previous, 'cross', native):
            previous.PrivateRuntimeMetadata().test_fresh_module_overlay_has_settings_before_activity_staging()

    def test_foreign_shared_posix_reservation_is_never_removed(self):
        claim = dict(marker='/virtual/ACCEPTANCE-STREAM-RUNNING', token='this-run', previous=None)
        with patch.object(Path, 'read_text', return_value='another-run'), \
                patch.object(Path, 'unlink') as unlink, patch.object(Path, 'rmdir') as rmdir:
            self.assertFalse(native.release_shared_reservation(claim))
        unlink.assert_not_called()
        rmdir.assert_not_called()

    def test_occupied_shared_stream_refuses_before_cross_claim(self):
        box = dict(kind='posix-ssh', scratch='/virtual/lane',
                   acceptance_marker='/virtual/ACCEPTANCE-STREAM-RUNNING',
                   exclusive_marker='/virtual/lane/FEEL-MATRIX-RUNNING')
        with patch.object(Path, 'exists', return_value=True), patch.object(Path, 'mkdir') as mkdir, \
                patch.object(native.time, 'monotonic', side_effect=[0, 0, 2]), \
                patch.object(native.time, 'sleep'):
            with self.assertRaisesRegex(TimeoutError, 'occupied'):
                native.acquire_shared_reservation(box, '/virtual/lane/run', timeout=1)
        mkdir.assert_not_called()


if __name__ == '__main__':
    unittest.main()
