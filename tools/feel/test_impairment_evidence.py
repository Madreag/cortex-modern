import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import feel_measure
from feel import report


class ImpairmentEvidence(unittest.TestCase):
    def test_effects_and_resize_need_both_peers_and_same_round(self):
        for case in ('clean', 'zero', 'wrong_round', 'absent_resize', 'different_resize'):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                manifest = dict(ticks=1200, jitter_ms=40, reorder_percent=10, dup_percent=5)
                for peer, seat in (('host', 1), ('client', 2)):
                    (root / peer).mkdir()
                    row = dict(round=8 if case=='wrong_round' and peer=='client' else 7, peer=seat,
                        jitter_ms=40, jitter_packets=0 if case=='zero' else 12, reorder_percent=10, reordered_packets=2,
                        dup_percent=5, duplicated_packets=1)
                    delay = 9 if case=='different_resize' and peer=='client' else 8
                    text = f'[net-lockstep] start round=7 frame=1 local_peer={seat} peers=2\n[net-fake-link] {json.dumps(row)}\n'
                    if case != 'absent_resize': text += f'[net-match] delay change peer=2 frame=600 delay={delay} revision=2\n'
                    (root / peer / 'stdout.log').write_text(text)
                self.assertEqual(report.impairment_evidence(root, manifest)['passed'], case=='clean')

    def test_no_effect_arm_cannot_pass_good_timing(self):
        for settings in (dict(jitter_ms=40), dict(reorder_percent=10, dup_percent=5)):
            with self.subTest(settings=settings), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                (root / 'host').mkdir()
                (root / 'host/stdout.log').write_text('')
                (root / 'manifest.json').write_text(json.dumps(dict(ticks=1200, **settings)))
                (root / 'host_report.json').write_text(json.dumps(dict(lockstep=dict(next_frame=1201,
                    sim_tick_ms=1000/60, missing_frame_stalls=0, steady_missing_frame_stalls=0))))
                rows = [dict(type='committed', tick=tick, wall_ms=tick*1000/60) for tick in (300, 1200)]
                self.assertFalse(report.item9a_gates(root, rows=rows)['pass_check'])

    def test_reorder_and_duplicate_judge_client_timing(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'manifest.json').write_text(json.dumps(dict(ticks=1200, launches_complete=True, reorder_percent=10, dup_percent=5)))
            with patch.object(feel_measure, 'timing_peer', side_effect=lambda root, peer: dict(measurement_complete=True, pass_check=peer!='client')), \
                 patch.object(feel_measure, 'compare_pair', return_value={'pass': True}), \
                 patch.object(feel_measure, 'compare_live_hashes', return_value=[dict(compared_ticks=1200, mismatched_ticks=0)]), \
                 patch.object(feel_measure, 'FULLSTATE_EVERY', 0):
                self.assertFalse(feel_measure.reduce_timing_case(root)['item9a_pass'])


if __name__ == '__main__':
    unittest.main()
