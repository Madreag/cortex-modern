"""The native builder must never use the coordinator or an occupied box."""
from pathlib import Path
import unittest
from unittest.mock import patch

import acceptance_posix_build as build


class NativeBuildAdmission(unittest.TestCase):
    def arguments(self):
        return ['--lane','test','--cache','/read-only/repo','--commit','a'*40,
                '--out','/home/erol/cortex-workers/test/build']

    def test_windows_is_refused_before_files_or_compilers(self):
        with patch.object(build.platform,'system',return_value='Windows'), \
             patch.object(build,'write') as write, patch.object(build,'export_commit') as export, \
             patch.object(build.subprocess,'Popen') as process:
            with self.assertRaisesRegex(ValueError,'never builds on Windows'):
                build.main(self.arguments())
        write.assert_not_called(); export.assert_not_called(); process.assert_not_called()

    def test_occupied_native_box_does_not_stage_source_or_claim_its_marker(self):
        with patch.object(build.platform,'system',return_value='Linux'), \
             patch.object(build,'retained_bytes',return_value=0), \
             patch.object(Path,'exists',return_value=True), patch.object(build,'write') as write, \
             patch.object(build,'export_commit') as export:
            with self.assertRaisesRegex(RuntimeError,'occupied'):
                build.main(self.arguments())
        write.assert_not_called(); export.assert_not_called()

    def test_storage_admission_precedes_source_export(self):
        with patch.object(build.platform,'system',return_value='Linux'), \
             patch.object(build,'retained_bytes',return_value=3_900_000_000), \
             patch.object(build,'write') as write, patch.object(build,'export_commit') as export:
            with self.assertRaisesRegex(RuntimeError,'headroom'):
                build.main(self.arguments())
        write.assert_not_called(); export.assert_not_called()


if __name__ == '__main__':
    unittest.main()
