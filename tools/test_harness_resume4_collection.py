"""Collection proofs for laptop planning and census attribution; no engines."""
import contextlib
import io
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch

import soak_two_peer
from test_soak_two_peer import write_peer
from test_acceptance_resume import fixture, reader
from test_acceptance_resume3 import build_plan
from test_inventory_oracle_evidence import run_split


class LaptopCollection(unittest.TestCase):
    def test_optional_variant_survives_into_the_acceptance_manifest(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); plan, requirements = fixture(root)
            schedule = json.loads((root/'split-plan.json').read_text())
            schedule.update(notes=['Z13 absent: rows planned on EROL-PC'], optional_boxes={'Z13': False})
            (root/'split-plan.json').write_text(json.dumps(schedule))
            result = reader.build_manifest(plan, requirements)
            self.assertEqual(result.get('planning_variant'), dict(notes=schedule['notes'], optional_boxes={'Z13': False}))

    def test_frozen_plan_declares_the_box_constraints_and_scene_counts(self):
        with patch.object(run_split, 'probe_optional_boxes', return_value={}):
            result = build_plan.build(Path(__file__).resolve().parents[1])
        declared = result['plan']['split']
        self.assertIn('box_constraints', declared)
        self.assertIn('engine_counts', declared)
        self.assertEqual(declared['engine_counts']['S2a.e2e.mp-host-stall-3p-200'] if 'S2a.e2e.mp-host-stall-3p-200' in declared['engine_counts']
                         else declared['engine_counts']['S2b.e2e.mp-host-stall-3p-200'], 3)

    def test_soak_result_names_census_cost_without_excusing_its_pace_gap(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for peer, peer_id in (('host', 1), ('client', 2)):
                write_peer(root, peer, peer_id, 1000/60, 8, 3901)
                with (root/peer/'stdout.log').open('a') as out:
                    out.write('[mem-census] tick=3600 census_us=cow:200000,atoms:375000\n')
                path = root/f'{peer}-live.jsonl'
                rows = [json.loads(line) for line in path.read_text().splitlines()]
                for row in rows:
                    if row['tick'] >= 3600: row['wall_ms'] += 575
                path.write_text(''.join(json.dumps(row)+'\n' for row in rows))
            options = SimpleNamespace(fullstate_every=0, rematch=False, rematches=0, end_round_tick=0,
                autosave_seconds=60, holds=0, minutes=0, host_stall=[], stall_each_round=False, terrain_events=False)
            with contextlib.redirect_stdout(io.StringIO()):
                soak_two_peer.analyze_soak(root, options, 3900, dict(injections=[]), [],
                                          {peer: dict(exit_code=0, timed_out=False) for peer in ('host', 'client')}, 66)
            result = json.loads((root/'result.json').read_text())
            self.assertFalse(result['checks']['complete_history_and_pace'])
            self.assertIn('harness_cost', result)
            self.assertIn('census instant=575', result['harness_cost']['reason'])
            self.assertFalse(result['checks']['instrument_valid'])
            self.assertEqual(result['harness_cost']['subtraction_ms'], 0)


if __name__ == '__main__':
    unittest.main()
