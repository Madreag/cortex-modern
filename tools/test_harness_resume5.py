"""Acceptance box roles and section variants; no engines or remote calls."""
import copy
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from test_acceptance_resume3 import build_plan
from test_inventory_oracle_evidence import run_split, run_stream

REPO = Path(__file__).resolve().parents[1]


def built(proof=None, marker=None):
    boxes, document = run_split.load_manifest(build_plan.BOXES_JSON)
    boxes, document = copy.deepcopy(boxes), copy.deepcopy(document)
    laptop = next(box for box in boxes if box.name == 'Z13')
    laptop.timing_proof = proof or {}
    if proof is not None:
        laptop.build_receipt = dict(date='2026-10-03',commit='a'*40,executable_sha256='b'*64)
    for entry in document['boxes']:
        if entry['name'] == 'Z13':
            if proof is not None:
                entry['timing_proof'] = copy.deepcopy(proof)
            else:
                entry.pop('timing_proof', None)
    with patch.object(run_split, 'load_manifest', return_value=(boxes, document)), \
         patch.object(run_split, 'probe_optional_boxes', return_value={'Z13': True}), \
         patch.object(run_split, 'WINDOW_MARKER', marker or REPO/'absent-unit-marker', create=True):
        return build_plan.build(REPO)


class AcceptanceBoxRoles(unittest.TestCase):
    def test_d01_local_game_peer_without_window_is_refused_at_plan_time(self):
        common = dict(repo=str(REPO), exe='never-run.exe', tools_root=str(REPO/'tools'), scratch_root='unused', streams=['S2'])
        local = run_split.Box(name='EROL-PC', kind='local', optional=True, **common)
        command = run_stream.Command('S2.unit', 'tools/e2e_video.py', [], engine_count=2)
        with tempfile.TemporaryDirectory() as folder:
            marker = Path(folder)/'marker'
            marker.write_text(json.dumps(dict(token='USER-AT-DESK')))
            with patch.object(run_split, 'WINDOW_MARKER', marker, create=True), \
                 patch.object(run_split, 'probe_optional_boxes', return_value={}):
                with self.assertRaisesRegex(ValueError, 'EROL-PC.*window'):
                    run_split.plan([local], {}, {'S2': [command]}, ['S2'], {'S2': 'EROL-PC'}, None)

    def test_d01_four_box_roster_and_rotations_use_z13(self):
        plan = built()['plan']
        for row in plan['rows']:
            if row.get('roster') == 'four-way' and not row.get('window_required'):
                self.assertEqual(set(row['boxes']), {'Z13', 'EDITH', 'Mac', 'Linux'}, row['id'])
        self.assertEqual(set(plan['matrix']['hosts']), {'z13', 'edith', 'mac', 'linux'})
        self.assertFalse(any(row.get('host') == 'erol' and not row.get('window_required') for row in plan['rows']))

    def test_d02_marker_token_selects_the_section_variant(self):
        with tempfile.TemporaryDirectory() as folder:
            marker = Path(folder)/'marker'
            marker.write_text(json.dumps(dict(token='USER-AT-DESK')))
            held = built(marker=marker)['plan']
            self.assertEqual(held.get('window', {}).get('variant'), 'WITHOUT')
            marker.write_text(json.dumps(dict(token='named-window',pid=os.getpid(),stream_root=str(Path(folder)/'lane'))))
            free = built(marker=marker)['plan']
            self.assertEqual(free.get('window', {}).get('variant'), 'WITH')
            self.assertEqual(held['window']['reason'], 'deferred to the EROL-PC window')

    def test_d03_relay_driver_is_local_and_both_game_peers_are_remote(self):
        plan = built()['plan']
        for rid in ('S1.turn-hold', 'S1.turn-renew', 'S1.relay-compare', 'gap.mp-relay-cloudflare'):
            row = next(row for row in plan['rows'] if row['id'] == rid and not row['share'].startswith(('X.mac','X.linux')))
            self.assertEqual(row.get('driver_box'), 'EROL-PC', rid)
            self.assertEqual(row.get('engine_boxes'), {'ALLY': 1, 'EDITH': 1}, rid)
            self.assertEqual({ref['box'] for ref in row['identities']}, {'ALLY', 'EDITH'}, rid)
            self.assertFalse(row.get('window_required'), rid)
            self.assertEqual(row.get('credential_files_box'), 'EROL-PC', rid)

    def test_d04_three_peer_capture_is_edith_blocked_not_a_laptop(self):
        plan = built()['plan']
        row = next(row for row in plan['rows'] if row.get('scenario') == 'mp-host-stall-3p-200')
        self.assertEqual(row['box'], 'EDITH')
        self.assertEqual(row.get('engine_boxes'), {'EDITH': 3})
        self.assertEqual(row.get('blocked_reason'), 'blocked by the EDITH readback fix (engine row A57.2)')
        self.assertFalse(row.get('window_required'))

    def test_d05_timing_proof_moves_only_the_two_engine_arms(self):
        for proof, target in ((None, 'EROL-PC'), ({'date':'2026-10-03', 'series':'measured.json', 'pass':False}, 'EROL-PC'),
                              ({'date':'2026-10-03', 'series':'measured.json', 'pass':'true'}, 'EROL-PC'),
                              ({'date':'2026-10-03', 'series':'measured.json', 'pass':True}, 'Z13')):
            with self.subTest(proof=proof):
                plan = built(proof)['plan']
                row = next(row for row in plan['rows'] if row['id'] == 'S3.feel-matrix')
                self.assertEqual(row['box'], target)
                self.assertEqual(row.get('window_required'), target == 'EROL-PC')
                for timing in (r for r in plan['rows'] if r.get('timing') and max(r.get('engine_boxes', {}).values(), default=0) >= 3):
                    self.assertEqual(timing['box'], 'EROL-PC')
                    self.assertTrue(timing['window_required'])

    def test_d06_sections_cover_every_row_once_in_the_ruled_order(self):
        plan = built()['plan']
        sections = plan.get('sections', [])
        self.assertEqual([row['id'] for row in sections], [0, '0b', *range(1, 7)])
        self.assertEqual(sections[0]['rows'], [])
        scheduled = [tuple(ref) for section in sections for ref in section['rows']]
        self.assertEqual(sorted(scheduled), sorted((row['share'], row['id']) for row in plan['rows']))
        self.assertEqual(len(scheduled), len(set(scheduled)))
        feel = next(row for row in plan['rows'] if row['id']=='S3.feel-matrix')
        self.assertEqual(feel['section'], 3)
        for row in plan['rows']:
            if row['share']=='X.cross': self.assertEqual(row['section'], 5)
            if row['share']=='X.rows': self.assertEqual(row['section'], 6)


if __name__ == '__main__':
    unittest.main()
