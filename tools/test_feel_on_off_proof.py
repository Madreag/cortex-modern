"""The feel matrix's recorder-on/off comparison is judged only when both matches committed one timeline. A pair of
matches that agreed different first frames or delay changes is not judged, and the row says why, so the inventory's
defect list reads it as a diagnostic instead of a desync; a pair on one timeline that differs is still a defect."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import feel_measure

INVENTORY = Path(os.environ.get('CC_INVENTORY_DIR') or 'D:/Projects/reviews/takeover-20260909/grok-workers/lead-tools/inventory')


def write_match(run, first_frame, changes, delays=(23, 23)):
    (run / 'host').mkdir(parents=True)
    report = {'service': {'runner': {'lobby': {'match_config': {'peer_input_delays': list(delays)}}}}}
    (run / 'host_report.json').write_text(json.dumps(report), encoding='utf-8')
    lines = [f'[net-match] agreed first frame={first_frame}'] + [f'[net-match] delay change peer={peer} frame={frame} delay={delay}' for peer, frame, delay in changes]
    (run / 'host/stdout.log').write_text('\n'.join(lines) + '\n', encoding='utf-8')


def diverged_row():
    reasons = ["tick 28: on-wire divergence in ['actor_timers', 'controller_route']"]
    return {'cross_peer': False, 'sim_gated_pass': False, 'all_tick_hashes_identical': False, 'first_full_row_difference': 28, 'pass': False,
            'existing_comparator': {'compared_ticks': 27, 'first_divergence': 28, 'divergent_subsystems': ['actor_timers', 'controller_route'], 'reasons': reasons}}


def timelines(root, on_first, off_first, on_changes=((2, 392, 20),), off_changes=((2, 392, 20),)):
    write_match(root / 'on', on_first, on_changes)
    write_match(root / 'off', off_first, off_changes)
    return {state: feel_measure.committed_timeline(root / state) for state in ('on', 'off')}


class OnOffScope(unittest.TestCase):
    def test_two_matches_with_different_first_frames_are_not_judged_and_say_why(self):
        with tempfile.TemporaryDirectory() as temp:
            row = feel_measure.scope_on_off(diverged_row(), timelines(Path(temp), 6, 5))
        self.assertIs(row['required'], False)
        self.assertIn('agreed_first_frame', row.get('reason', ''), 'a not-judged recorder on/off row carries no reason')
        self.assertIs(row['pass'], False, 'the comparison itself is reported as it was measured')

    def test_different_delay_changes_are_named(self):
        with tempfile.TemporaryDirectory() as temp:
            row = feel_measure.scope_on_off(diverged_row(), timelines(Path(temp), 5, 5, off_changes=((2, 392, 20), (2, 1119, 21))))
        self.assertIs(row['required'], False)
        self.assertIn('delay_changes', row.get('reason', ''))
        self.assertNotIn('agreed_first_frame', row['reason'])

    def test_one_committed_timeline_is_judged_with_no_reason(self):
        with tempfile.TemporaryDirectory() as temp:
            row = feel_measure.scope_on_off(diverged_row(), timelines(Path(temp), 5, 5))
        self.assertIs(row['required'], True)
        self.assertNotIn('reason', row)


@unittest.skipUnless((INVENTORY / 'extract_defects.py').is_file(), 'the inventory extractor is not on this box')
class InventoryReadsTheScope(unittest.TestCase):
    def defects(self, row):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            report = {'name': '100ms-60hz', 'off_wire_pass': not row['required'],
                      'proof': {'peers_on': {'cross_peer': True, 'pass': True}, 'host_on_off': row}}
            (root / '100ms-60hz-on').mkdir()
            (root / '100ms-60hz-on/feel-report.json').write_text(json.dumps(report), encoding='utf-8')
            done = subprocess.run([sys.executable, str(INVENTORY / 'extract_defects.py'), str(root), '--out', str(root / 'DEFECTS.json')],
                                  capture_output=True, text=True, timeout=120)
            self.assertEqual(done.returncode in (0, 1), True, done.stderr[-2000:])
            return [entry['quoted_line'] for entry in json.loads((root / 'DEFECTS.json').read_text(encoding='utf-8'))['defects']]

    def test_a_not_judged_pair_is_no_divergence_defect(self):
        with tempfile.TemporaryDirectory() as temp:
            row = feel_measure.scope_on_off(diverged_row(), timelines(Path(temp), 6, 5))
        found = [line for line in self.defects(row) if 'divergen' in line]
        self.assertEqual(found, [], f'the not-judged recorder on/off row was filed as a defect: {found}')

    def test_a_judged_pair_that_differs_is_still_a_defect(self):
        with tempfile.TemporaryDirectory() as temp:
            row = feel_measure.scope_on_off(diverged_row(), timelines(Path(temp), 5, 5))
        self.assertTrue(any('on-wire divergence' in line for line in self.defects(row)), 'a divergence on one committed timeline went unreported')


if __name__ == '__main__':
    unittest.main()
