"""Resume counterexamples; synthetic evidence only, no engine launches."""
import contextlib
import copy
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import cross_peers
import cross_report
import e2e_video as video
import feel_measure
from feel import report
from feel.test_attempt_requirements import attempt
from feel.test_harness_cost import complete_cost_log


class HarnessResume(unittest.TestCase):
    def test_x01_shipped_tip_build_accepts_stale_tree_head(self):
        manifest, checks, peers, matrix, recoveries = attempt()
        manifest['preflights']['EDITH']['head'] = 'c' * 40
        result = cross_report.judge_attempt(manifest, checks, peers, matrix, recoveries)
        self.assertTrue(result['v1_passed'], result['oracles']['acceptance_identity'])
        identity = cross_report.acceptance_identity(manifest, peers)
        self.assertIn('stale tree head', json.dumps(identity.get('diagnostics', [])))

    def test_x02_short_match_has_no_memory_bar(self):
        manifest, checks, peers, matrix, recoveries = attempt()
        manifest.update(scenario='match', acceptance_row=17, ticks=1201, faults=[])
        checks['memory_bounds'] = False
        for peer in peers.values():
            peer.update(memory_by_incarnation={'0': dict(passed=False, sizes={}, missing_samples=1)}, memory_census={})
        result = cross_report.judge_attempt(manifest, checks, peers, matrix, recoveries)
        self.assertTrue(result['v1_passed'], result['oracles']['memory'])
        self.assertEqual(result['oracles']['memory']['status'], 'NOT APPLICABLE')
        self.assertEqual(result['oracles']['memory']['reason'], '1201-tick match: shorter than the 120 s warm-up')

    def test_x03_standard_arm_does_not_excuse_whole_round_slowdown(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root/'host').mkdir()
            (root/'host/stdout.log').write_text(complete_cost_log())
            (root/'manifest.json').write_text(json.dumps(dict(ticks=1200, per_peer_lag_ms=dict(host=100, client=100))))
            for name in ('host', 'client'):
                (root/f'{name}_report.json').write_text(json.dumps(dict(pace=dict(sim_ms_per_tick=20, wall_tps=50),
                    lockstep=dict(round_id=7, local_capacity_tps=50, sim_tick_ms=1000/60, next_frame=1201,
                                  missing_frame_stalls=0, steady_missing_frame_stalls=0))))
            rows = [dict(type='committed', tick=t, wall_ms=t*20) for t in (300, 1200)]
            result = report.item9a_gates(root, rows=rows)
        self.assertEqual(result['pins']['item9a_wall_tps']['status'], 'FAIL')
        self.assertEqual(result['pins']['item9a_confirmed_horizon_lag']['status'], 'FAIL')

    def test_x04_invalid_abandon_fails_one_arm_without_aborting_reduction(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root/'manifest.json').write_text(json.dumps(dict(ticks=1200, launches_complete=True)))
            (root/'client-live.jsonl').write_text(json.dumps(dict(abandon_from=12, round=7))+'\n')
            with patch.object(feel_measure, 'timing_peer', return_value=dict(measurement_complete=True, pass_check=True)), \
                 patch.object(feel_measure, 'input_gates', return_value={}), \
                 patch.object(feel_measure, 'compare_pair', return_value=dict(pass_=True, **{'pass': True})), \
                 patch.object(feel_measure, 'FULLSTATE_EVERY', 0):
                try:
                    result = feel_measure.reduce_timing_case(root)
                except ValueError as error:
                    self.fail(f'arm analysis aborted on receipt: {error}')
            self.assertFalse(result['off_wire_pass'])
            self.assertIn('abandon receipt', json.dumps(result))
            self.assertTrue((root/'hash-proof.json').is_file())

    def test_x05_feeder_and_own_seat_capacity_are_named_design_holds(self):
        text = ('[net-lockstep] start round=7 frame=1 local_peer=1 peers=4\n'
                '[net-lockstep] slow machine peer 1 at frame 118: 40 ticks/s against 60; the AI takes its seat\n'
                '[net-lockstep] propose hold peer=1 next_frame=118 cause=own_seat\n'
                '[net-match] hold peer=1 frame=118 AI in control\n')
        receipts, _ = cross_report.host_hold_evidence(text)
        result = cross_report.design_hold(dict(peer=1, tick=118, round=7), receipts)
        self.assertEqual(result.get('classification'), 'capacity (design)', result)

    def test_x06_host_loss_accepts_only_the_scoped_hold_and_ban_schedule(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            faults = [dict(id='hold', peer='mac', tick=300, action='silence', duration_ms=10000, duration_ticks=600),
                      dict(id='ban', peer='erol', target_peer='edith', tick=500, action='moderation-ban'),
                      dict(id='loss', peer='erol', tick=900, action='host-kill')]
            schedule = root/'schedule.json'; schedule.write_text(json.dumps(faults))
            args = ['--lane', 'unit', '--mac-guard', str(root/'guard'), '--scenario', 'match', '--schedule', str(schedule)]
            try:
                plan = cross_peers.make_plan(cross_peers.parse_args(args))
            except ValueError as error:
                self.fail(f'HL4 preparation was refused: {error}')
            self.assertEqual(len(plan['faults']), 3)
            schedule.write_text(json.dumps(faults + [dict(id='extra', peer='mac', tick=700, action='live-stall', duration_ms=50)]))
            with self.assertRaises(ValueError):
                cross_peers.make_plan(cross_peers.parse_args(args))

    def test_x07_killed_peer_uses_last_periodic_watch_summary(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); (root/'video').mkdir()
            (root/'video/injected-drop.json').write_text(json.dumps(dict(requested_tick=300,
                last_recorded_frame=dict(frame=300, sim_tick=300))))
            peer = dict(peer='client', root=str(root), video_dir=str(root/'video'), expected_termination=True,
                        record=dict(exit_code=137, injected_termination='scenario drop after recorded tick 300'))
            armed = '[text-watch] armed '+json.dumps(dict(armed='h15-layout', rule='layout', state='always'))+'\n'
            with patch.object(video, 'expected_screen_watches', return_value={'layout'}), patch.object(video, 'freeze_scan', return_value=[]):
                (root/'stdout.log').write_text(armed)
                missing = video.capture_evidence_items({}, dict(name='unit'), peer)
                self.assertEqual(next(r for r in missing if r['id']=='screen-layout-client')['probe'], 'incomplete')
                summaries = '\n'.join('[text-watch] summary '+json.dumps(dict(watch='h15-layout', frames=t, active_frames=t,
                    violations=0, tick=t, through_tick=t, flush='periodic')) for t in (100, 300))
                (root/'stdout.log').write_text(armed+summaries)
                complete = video.capture_evidence_items({}, dict(name='unit'), peer)
            row = next(r for r in complete if r['id']=='screen-layout-client')
            self.assertEqual(row['probe'], 'pass', row)
            self.assertIn('terminated by the scene at tick 300', json.dumps(row))

    def test_x08_capacity_is_complete_per_round(self):
        native = dict(pace=dict(sim_ms_per_tick=20, wall_tps=50), rounds=[
            dict(round_id=r, local_capacity_tps=50, sim_tick_ms=1000/60) for r in (7, 8)])
        natives = {name: copy.deepcopy(native) for name in ('host', 'client')}
        result = cross_report.round_capacity_evidence(natives)
        self.assertTrue(result['complete'], result)
        self.assertTrue(all(row['complete'] for row in result['rounds'].values()))
        natives['client']['rounds'].pop()
        result = cross_report.round_capacity_evidence(natives)
        self.assertTrue(result['rounds']['7']['complete'])
        self.assertFalse(result['rounds']['8']['complete'])

    def test_x09_copied_capture_never_writes_outside_given_root(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); original = root/'original'; copied = root/'copy'
            original_peer = original/'first/host'; copied_peer = copied/'first/host'
            original_peer.mkdir(parents=True); copied_peer.mkdir(parents=True)
            (copied_peer/'launch.json').write_text(json.dumps(dict(exit_code=0, timed_out=False)))
            scenario = dict(name='unit', peers=[dict(name='host')], runs=[dict(name='first')])
            peer = dict(peer='host', root=str(original_peer), video_dir=str(original_peer/'video'),
                        probe_dir=str(original/'first/host-stage/probe'))
            captured = dict(scenario='unit', scenario_definition=scenario, source={}, exe={}, fps=3, started=video.stamp(),
                            runs=[dict(name='first', root=str(original/'first'), size='640x360', peers=[peer])])
            (copied/'capture.json').write_text(json.dumps(captured))
            writes = []
            def review(_scenario, _run, destination):
                writes.append(Path(destination).resolve())
                (Path(destination)/'review.json').write_text('{}')
            with patch.object(video, 'review', side_effect=review), patch.object(video, 'scenario_manifest'), \
                 patch.object(video, 'aggregate_review'), contextlib.redirect_stdout(io.StringIO()):
                try:
                    video.finalize_only(SimpleNamespace(finalize_only=copied, metadata_only=True, scratch_root=copied, sheet_every=3))
                except ValueError as error:
                    self.assertIn('recorded root', str(error))
            self.assertEqual([str(p) for p in writes if not p.is_relative_to(copied.resolve())], [], 'copied capture wrote into original')
            self.assertFalse((original/'first/review.json').exists())

    def test_x10_pending_review_is_not_failed_share(self):
        from test_inventory_oracle_evidence import run_split
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            share = SimpleNamespace(label='S1', box=SimpleNamespace(name='local'), ids=['S1.unit'])
            results = {}
            run_split.finish_share(share, SimpleNamespace(out=root), results, 3)
            receipt = json.loads((root/'S1/terminal.json').read_text())
            self.assertEqual(receipt['state'], 'pending', receipt)
            coverage = run_split.terminal_coverage(['S1'], results)
            self.assertFalse(coverage['passed'])
            self.assertEqual(coverage['failed'], {})
            self.assertEqual(coverage['pending'], {'S1': 3})

    def test_x11_zero_native_coverage_is_required_red(self):
        rows = cross_report.coverage({'peer': [dict(type='progress', budget_tick=72000)]}, {'peer': {}}, dict(scenario='soak'))
        weapons = next(row for row in rows if row['id']=='weapons')
        self.assertTrue(weapons['required'], weapons)
        self.assertEqual(weapons['status'], 'FAIL', weapons)
        for row in rows:
            if row['id'] != 'terrain':
                self.assertTrue(row['required'] or row.get('engineer_receipts'), row)


if __name__ == '__main__':
    unittest.main()
