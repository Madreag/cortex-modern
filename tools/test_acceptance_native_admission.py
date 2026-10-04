"""Native foreign-workload checks must precede every owned engine launch."""
from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import unittest
from unittest.mock import Mock, patch

import acceptance_native_runtime as native


class NativeAdmission(unittest.TestCase):
    def test_foreign_workload_refuses_before_capability_engine(self):
        payload = dict(box=dict(name='Mac', kind='posix-ssh', hostname='Erol-Mac'),
                       specs=[dict(peer='mac', acceptance_row='mod-match', preserve_evidence=True)])
        with patch.object(Path,'read_text',return_value=json.dumps(payload)), \
                patch.object(native.platform,'node',return_value='Erol-Mac'), \
                patch.object(native,'assert_box_guard'), patch.object(native,'write_json'), \
                patch.object(native,'box_load',return_value=[dict(ProcessId=99,Name='CortexCommand')]), \
                patch.object(native,'read_capabilities',side_effect=RuntimeError('capability reached')) as caps, \
                redirect_stdout(io.StringIO()) as printed:
            self.assertEqual(native.run_payload('/virtual/payload.json'),1)
        caps.assert_not_called()
        self.assertIn('under this row reservation: CortexCommand pid 99', printed.getvalue())

    def test_foreign_workload_during_preparation_refuses_before_game_start(self):
        spec = dict(peer='mac', role='host', own='/virtual/mac/incarnation-0', acceptance_row='mod-match',
                    preserve_evidence=True, flags=['-net-match-peers','4'], timeout=60)
        payload = dict(box=dict(name='Mac',kind='posix-ssh',hostname='Erol-Mac'), specs=[spec], pin='')
        run = Mock(record={'started':False})
        with patch.object(Path,'read_text',return_value=json.dumps(payload)), \
                patch.object(native.platform,'node',return_value='Erol-Mac'), \
                patch.object(native,'assert_box_guard'), patch.object(native,'write_json'), \
                patch.object(native,'box_load',side_effect=[[],[],[dict(ProcessId=99,Name='CortexCommand')]]), \
                patch.object(native,'read_capabilities',return_value={'peer_limit':4}), \
                patch.object(native,'wait_for_payload_release'), \
                patch.object(native,'prepare_instance',return_value=run), \
                patch.object(native.acceptance_cross,'restore_activity'), \
                patch.object(native.acceptance_cross,'retain_native_screens'), redirect_stdout(io.StringIO()) as printed:
            self.assertEqual(native.run_payload('/virtual/payload.json'),1)
        run.start.assert_not_called()
        self.assertIn('under this row reservation: CortexCommand pid 99', printed.getvalue())
        run.close.assert_called()


if __name__ == '__main__': unittest.main()
