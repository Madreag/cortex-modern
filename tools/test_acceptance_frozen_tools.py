"""Frozen tool identity and native adapter admission regressions; no engine runs."""
import hashlib
import json
from pathlib import Path
import unittest
from unittest.mock import patch

import acceptance_frozen_tools as frozen
import acceptance_native_runtime as native


class FrozenIdentity(unittest.TestCase):
    def test_one_changed_frozen_byte_is_refused(self):
        data = b'unchanged exported tool\n'
        value = dict(frozen_commit=frozen.AUTHORIZED_COMMIT,
                     files=[dict(path='tools/cross_peers.py', sha256=hashlib.sha256(data).hexdigest())])
        with patch.object(Path, 'is_file', return_value=True), \
                patch.object(Path, 'read_text', return_value=json.dumps(value)), \
                patch.object(Path, 'read_bytes', return_value=data):
            self.assertEqual(frozen.receipt('/virtual')['frozen_commit'], frozen.AUTHORIZED_COMMIT)
        with patch.object(Path, 'is_file', return_value=True), \
                patch.object(Path, 'read_text', return_value=json.dumps(value)), \
                patch.object(Path, 'read_bytes', return_value=data+b' '):
            with self.assertRaisesRegex(ValueError, 'bytes differ'):
                frozen.receipt('/virtual')

    def test_an_unnamed_newer_tip_is_not_an_authorized_export(self):
        with patch.object(Path, 'is_file', return_value=True), \
                patch.object(Path, 'read_text', return_value=json.dumps(dict(frozen_commit='f'*40, files=[]))):
            with self.assertRaisesRegex(ValueError, 'NOTE 11'):
                frozen.receipt('/virtual')

    def test_erol_pc_is_refused_before_even_a_capability_launch(self):
        payload = dict(box=dict(kind='windows-task'), specs=[])
        with patch.object(Path, 'read_text', return_value=json.dumps(payload)), \
                patch.object(native.platform, 'node', return_value='EROL-PC'), \
                patch.object(native, 'read_capabilities') as caps:
            with self.assertRaisesRegex(ValueError, 'driver only'):
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
