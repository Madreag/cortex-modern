"""Committed-source transfer and the native Ally boundary."""
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import acceptance_ally_build as build


class AllyBuildBoundary(unittest.TestCase):
    def test_pc_payload_is_refused_before_any_native_build_action(self):
        with patch.object(build.platform,'node',return_value='EROL-PC'), \
             patch.object(build,'ps') as native, patch.object(build,'write') as write:
            with self.assertRaisesRegex(ValueError,'only on EROL-ALLY7'):
                build.payload(SimpleNamespace())
        native.assert_not_called(); write.assert_not_called()

    def test_committed_manifest_uses_objects_instead_of_dirty_working_bytes(self):
        listing = b'100644 blob '+b'a'*40+b'      3\tSource/test.cpp\0'
        with patch.object(build.subprocess,'check_output',return_value=listing) as git:
            value=build.source_entries(Path('/read-only/repo'),'b'*40)
        self.assertEqual(value,[('Source/test.cpp','100644','a'*40,3)])
        self.assertEqual(git.call_args.args[0],['git','-C',str(Path('/read-only/repo')),'ls-tree','-rlz','b'*40])

    def test_non_blob_source_objects_are_not_silently_skipped(self):
        with patch.object(build.subprocess,'check_output',return_value=b'160000 commit '+b'a'*40+b'       -\tdependency\0'):
            with self.assertRaisesRegex(ValueError,'unsupported object'):
                build.source_entries(Path('/read-only/repo'),'b'*40)


if __name__=='__main__':
    unittest.main()
