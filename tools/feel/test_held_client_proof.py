import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import feel_measure


class HeldClientProof(unittest.TestCase):
    def test_p10_held_client_kept_history_failure_reaches_the_parent(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'manifest.json').write_text(json.dumps(dict(ticks=1200, silent_tick=600, launches_complete=True)))
            with patch.object(feel_measure, 'timing_peer', return_value=dict(measurement_complete=True, pass_check=True)), \
                 patch.object(feel_measure, 'compare_pair', side_effect=[{'pass': True}, {'pass': False}]), \
                 patch.object(feel_measure, 'compare_live_hashes', return_value=[dict(compared_ticks=30, mismatched_ticks=0)]), \
                 patch.object(feel_measure, 'FULLSTATE_EVERY', 0):
                result = feel_measure.reduce_timing_case(root)
        self.assertFalse(result['off_wire_pass'], result)


if __name__ == '__main__':
    unittest.main()
