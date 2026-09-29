"""Acceptance counterexamples without engine launches."""
import json
from pathlib import Path
import tempfile
import unittest

import cross_peers
import cross_report
import test_determinism_pair as determinism
import soak_two_peer as soak
import test_autosave_restore as autosave
from unittest import mock


class AcceptanceTests(unittest.TestCase):
    def test_f25_every_forced_arm_accepts_the_lever(self):
        import inspect
        for name in ('restore', 'retention', 'anchor', 'resume', 'world_restart'):
            self.assertIn('client_stall', inspect.signature(getattr(autosave, 'arm_' + name)).parameters)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with mock.patch.object(autosave, 'wait_for_checkpoints', side_effect=lambda attempt, ticks: attempt(1, ticks)), \
                 mock.patch.object(autosave, 'run_pair', return_value={}) as pair, \
                 mock.patch.object(autosave, 'forced_hold_evidence', return_value={'requested': True}) as proof, \
                 mock.patch.object(autosave, 'judge_restore', return_value={}), \
                 mock.patch.object(autosave, 'judge_retention', return_value={}), \
                 mock.patch.object(autosave, 'judge_anchor', return_value={}):
                for name in ('restore', 'retention', 'anchor'):
                    result = getattr(autosave, 'arm_' + name)(root, root, 48720, client_stall='40:600')
                    self.assertIn('40:600', pair.call_args.args[5]['client'])
                    self.assertTrue(result['forced_hold']['requested'])
                    proof.assert_called()

    def test_f25_native_hold_and_completed_reclaim_are_required(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for peer in ('host', 'client'):
                (root / peer).mkdir()
            host = '[net-match] hold peer=2 frame=50 AI in control\n[net-match] seat-reclaimed peer=2 frame=170\n'
            client = '[net-lockstep] start round=7 frame=1 local_peer=2\n[net-test] live stall frame=40 ms=600\n[net-match] private catch-up complete frame=170\n'
            (root / 'host/stdout.log').write_text(host)
            (root / 'client/stdout.log').write_text(client)
            self.assertTrue(autosave.forced_hold_evidence(root, '40:600')['requested'])
            for bad in ('', client.split('[net-match] private')[0], client.replace('frame=40', 'frame=39')):
                (root / 'client/stdout.log').write_text(bad)
                with self.assertRaises(AssertionError):
                    autosave.forced_hold_evidence(root, '40:600')
            (root / 'client/stdout.log').write_text(client)
            (root / 'host/stdout.log').write_text('')
            with self.assertRaises(AssertionError):
                autosave.forced_hold_evidence(root, '40:600')

    def cross_case(self, scenario='match'):
        manifest = dict(scenario=scenario, ticks=1201 if scenario == 'match' else 72000,
                        fullstate_every=600, faults=[] if scenario == 'match' else [dict(id='spike')], capture_rows_pending=[])
        checks = dict.fromkeys(cross_report.CORE_CHECKS, True)
        checks.update(shared_fullstate=True, bounded_recovery=True, faults_applied=True,
                      native_fault_effects=True, all_incarnation_exits=True, no_engine_findings=True)
        return manifest, checks

    def test_f10_capture_induced_hold_is_not_success(self):
        manifest, checks = self.cross_case()
        checks.update(zero_unscheduled_holds=False, full_history=False, only_capture_induced_holds=True)
        verdict = cross_report.judge_attempt(manifest, checks, {}, [], [])
        self.assertFalse(verdict.get('v1_passed', verdict['gate_b_eligible']))

    def test_f10_missing_capture_is_not_success(self):
        manifest, checks = self.cross_case()
        checks['shared_fullstate'] = False
        verdict = cross_report.judge_attempt(manifest, checks, {}, [], [])
        self.assertFalse(verdict.get('v1_passed', verdict['gate_b_eligible']))

    def test_f11_soak_has_twenty_minutes(self):
        options = cross_peers.parse_args(['--lane', 'unit-acceptance', '--mac-guard', 'unit-marker', '--scenario', 'soak'])
        self.assertEqual(options.ticks, 72000)
        manifest, checks = self.cross_case('soak')
        manifest['ticks'] = 36000
        self.assertFalse(cross_report.judge_attempt(manifest, checks, {}, [], []).get('v1_passed', True))

    def test_f12_unrelated_placeholder_does_not_decide_v1(self):
        for scenario in ('soak', 'chaos'):
            manifest, checks = self.cross_case(scenario)
            checks.update(validated_autosave_archives=False, forced_end_during_transfer=False)
            verdict = cross_report.judge_attempt(manifest, checks, {}, [], [])
            self.assertTrue(verdict.get('v1_passed', all(checks.values())))
            checks['bounded_recovery'] = False
            verdict = cross_report.judge_attempt(manifest, checks, {}, [], [])
            self.assertFalse(verdict.get('v1_passed', all(checks.values())))

    def test_f13_empty_dump_is_not_identical(self):
        with tempfile.TemporaryDirectory() as temporary:
            a, b = Path(temporary) / 'a', Path(temporary) / 'b'
            a.touch(); b.touch()
            self.assertFalse(determinism.first_object_divergence(a, b).get('identical', False))

    def test_f13_held_client_must_reach_the_planned_terminal_tick(self):
        texts='[net-match] hold peer=2 frame=50 AI in control\n[preview-argument] preview uid=7 attached=false\n'
        records={peer:dict(exit_code=0,timed_out=False) for peer in ('host','client')}
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            for terminal in (599,600):
                def trace(path,*_):
                    end=600 if path.parent.name=='host' else terminal
                    return (dict.fromkeys(range(1,end+1)),None)
                with mock.patch.object(determinism,'peer_text',return_value=texts), \
                     mock.patch.object(determinism,'load_trace',side_effect=trace), \
                     mock.patch.object(determinism,'strict_compare',return_value=(True,{'compared_ticks':terminal})), \
                     mock.patch.object(determinism,'first_object_divergence',return_value={'identical':True}):
                    result=determinism.score(root,'argument','unit',records)
                self.assertEqual(result['passed'],terminal==600,result)

    def test_f13_missing_object_tick_is_not_identical(self):
        with tempfile.TemporaryDirectory() as temporary:
            a, b = Path(temporary) / 'a', Path(temporary) / 'b'
            a.write_text('1 actor x=1\n2 actor x=2\n')
            b.write_text('1 actor x=1\n')
            self.assertFalse(determinism.first_object_divergence(a, b).get('identical', False))
            b.write_text(a.read_text())
            self.assertTrue(determinism.first_object_divergence(a, b).get('identical', False))

    def test_f13_both_dumps_must_cover_the_trace(self):
        with tempfile.TemporaryDirectory() as temporary:
            a, b = Path(temporary) / 'a', Path(temporary) / 'b'
            a.write_text('1 actor x=1\n'); b.write_text(a.read_text())
            self.assertFalse(determinism.first_object_divergence(a, b, expected_ticks={1, 2}).get('identical', False))

    def test_f14_pace_step_and_missing_terminal_fail(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for peer in ('host', 'client'):
                (root / peer).mkdir()
                (root / peer / 'stdout.log').write_text('')
                records = [dict(round=7, tick=tick, wall_ms=tick * 1000 / 60,
                                sim_gated=str(tick), subsystems={key: str(tick) for key in soak.CORE | {'controller'}})
                           for tick in range(1, 7201)]
                (root / (peer + '-live.jsonl')).write_text(''.join(json.dumps(r) + '\n' for r in records))
            score = soak.acceptance_history(root, 7200)
            self.assertTrue(score['pass'], score)
            path = root / 'client-live.jsonl'
            good = path.read_text()
            rows = [json.loads(line) for line in good.splitlines()]
            for row in rows:
                if row['tick'] > 3600:
                    row['wall_ms'] = 60000 + (row['tick'] - 3600) * 1000 / 30
            path.write_text(''.join(json.dumps(r) + '\n' for r in rows))
            self.assertFalse(soak.acceptance_history(root, 7200)['pass'])
            path.write_text('\n'.join(good.splitlines()[:-1]) + '\n')
            self.assertFalse(soak.acceptance_history(root, 7200)['pass'])


if __name__ == '__main__':
    unittest.main()
