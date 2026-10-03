"""Boundary cases for the ruled harness fixes; no engines or remote work."""
import contextlib
import copy
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import cross_report
import edith_cross
import e2e_video as video
import soak_two_peer
from compare_sim_traces import CORE
from feel.host_loss import host_loss_evidence, scheduled_hold_classification
from feel.test_attempt_requirements import attempt
from test_soak_oracle_evidence import analyzed_pair


def hl4_fixture():
    names = ['erol', 'edith', 'mac', 'linux']
    context = dict(session='s', match='m', history_branch=0, source_round=1, round=1)
    faults = [dict(id='hold', peer='mac', action='silence', tick=1),
              dict(id='ban', peer='erol', target_peer='edith', action='moderation-ban', tick=3),
              dict(id='loss', peer='erol', action='host-kill', tick=5)]
    manifest = dict(host='erol', faults=faults, ticks=8, instances=[dict(name=n) for n in names])
    peers = {n: dict(feel_gated=True, feel_pass=True, record=dict(exit_code=0, timed_out=False),
                    native_completion=dict(completion='completed'), configs=[dict(peer=i+1)]) for i, n in enumerate(names)}
    peers['erol']['record'] = dict(pid=44, exit_code=137, injected_termination='scheduled crash loss')
    receipt = dict(id='loss', peer='erol', incarnation=0, execution='process-0/0', action='host-kill',
                   engine_pid=44, process_terminated=True, exit_code=137, before_wall_ms=10, after_wall_ms=11,
                   actual=dict(context, applied_frame=5, budget_tick=5, host_peer=1, authority_generation=1))
    state = dict(held_seats=[dict(seat=3, cause='silent')], bans=['b'*64],
                 tickets=[dict(seat=3, incarnation=0, identity_sha256='a'*64)])
    def row(owner, tick, **fields):
        return dict(context, instance=owner, incarnation=0, execution='process-0/0', tick=tick, **fields)
    events = {n: [] for n in names}
    events['mac'] = [row('mac', 1, type='fault', id='hold', action='outage', applied=True, send_recv_armed=True, budget_tick=1)]
    events['erol'] = [row('erol', 2, type='scheduled_hold', id='hold', peer=3, cause='silent', ai_in_control=True, silence_ms=51, bound_ms=50),
                      row('erol', 3, type='moderation_action', id='ban', action='Ban', applied=True, target_peer='edith',
                          target_seat=2, identity_sha256='b'*64, budget_tick=3),
                      row('erol', 5, type='moderation_snapshot', stage='before', state=state, host_peer=1, authority_generation=1)]
    events['edith'] = [row('edith', 3, type='moderation_terminal', id='ban', result='ParticipantBanned', terminal=True,
                          identity_sha256='b'*64, last_live_tick=2)]
    for name in ('mac', 'linux'):
        events[name].append(row(name, 6, type='moderation_snapshot', stage='after', state=state, host_peer=4, authority_generation=2))
    live = {name: [row(name, tick, phase='live', host_peer=1 if tick <= 5 else 4,
                       authority_generation=1 if tick <= 5 else 2, sim_gated='a'*64,
                       subsystems={key: 'a'*64 for key in CORE | {'controller'}})
                   for tick in range(1, 6 if name == 'erol' else 3 if name == 'edith' else 9)] for name in names}
    return manifest, peers, events, live, [receipt]


class ResumeEdges(unittest.TestCase):
    def test_memory_exemption_does_not_cover_long_or_host_loss_rows(self):
        manifest, _, peers, _, _ = attempt()
        for peer in peers.values(): peer['memory_census'] = {}
        for changes in (dict(scenario='soak'), dict(scenario='chaos'), dict(scenario='world'),
                        dict(scenario='match', acceptance_row=17, ticks=1201, faults=[dict(action='host-kill')]),
                        dict(scenario='match', acceptance_row=17, ticks=1201, faults=[], world=True)):
            with self.subTest(changes=changes):
                self.assertNotEqual(cross_report.memory_verdict(peers, dict(manifest, **changes))['status'], 'NOT APPLICABLE')

    def test_soak_writes_failed_result_after_bad_abandon(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for name in ('host', 'client'):
                (root/name).mkdir(); (root/name/'stdout.log').write_text('')
            (root/'client-live.jsonl').write_text(json.dumps(dict(abandon_from=12, round=7))+'\n')
            options = SimpleNamespace(fullstate_every=0, rematch=False, end_round_tick=0, autosave_seconds=60,
                                      holds=0, minutes=1, host_stall=[], stall_each_round=0, terrain_events='')
            with contextlib.redirect_stdout(io.StringIO()):
                code = soak_two_peer.analyze_soak(root, options, 1200, dict(injections=[]), [], {}, 1)
            result = json.loads((root/'result.json').read_text())
            self.assertEqual(code, 1)
            self.assertFalse(result['checks']['hashes_equal'])
            self.assertIn('abandon receipt', json.dumps(result['acceptance_history']))

    def test_edith_bad_abandon_keeps_both_peer_verdicts(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); harness, meta = analyzed_pair(root)
            (root/'client-live.jsonl').write_text(json.dumps(dict(abandon_from=12, round=7))+'\n')
            with contextlib.redirect_stdout(io.StringIO()):
                result = edith_cross.analyze_match(harness, root, meta)
            self.assertFalse(result['passed'])
            self.assertEqual(set(result['peers']), {'host', 'client'})
            self.assertIn('abandon receipt', json.dumps(result['receipt_failures']))
            self.assertFalse(edith_cross.soak_verdict(root, 1200)['passed'])

    def test_own_seat_capacity_requires_host_identity_and_same_frame(self):
        for local, published in ((2, 118), (1, 117)):
            text = (f'[net-lockstep] start round=7 frame=1 local_peer={local} peers=4\n'
                    f'[net-lockstep] slow machine peer 1 at frame {published}: 40 ticks/s against 60; the AI takes its seat\n'
                    '[net-lockstep] propose hold peer=1 next_frame=118 cause=own_seat\n'
                    '[net-match] hold peer=1 frame=118 AI in control\n')
            receipts, _ = cross_report.host_hold_evidence(text)
            self.assertFalse(cross_report.design_hold(dict(peer=1, tick=118, round=7), receipts))

    def test_hl4_native_preparations_are_scoped_and_two_survivors_continue(self):
        args = hl4_fixture()
        result = host_loss_evidence(*args)
        self.assertTrue(result['passed'], result)
        self.assertEqual(result['survivors'], ['mac', 'linux'])
        self.assertEqual([len(row['peers']) for row in result['ranges']], [4, 3, 2])
        for peer, tick in ((3, 2), (2, 3)):
            self.assertEqual(scheduled_hold_classification(dict(peer=peer, tick=tick, round=1), result)['classification'], 'SCHEDULED')
        self.assertFalse(scheduled_hold_classification(dict(peer=4, tick=4, round=1), result))
        for kind in ('fault', 'scheduled_hold', 'moderation_action', 'moderation_terminal'):
            changed = copy.deepcopy(args)
            for rows in changed[2].values(): rows[:] = [row for row in rows if row['type'] != kind]
            with self.subTest(kind=kind): self.assertFalse(host_loss_evidence(*changed)['passed'])
        changed = copy.deepcopy(args)
        changed[2]['erol'][0]['silence_ms'] = 50
        self.assertFalse(host_loss_evidence(*changed)['passed'])
        changed = copy.deepcopy(args)
        changed[3]['edith'].append(dict(changed[3]['edith'][-1], tick=3))
        self.assertFalse(host_loss_evidence(*changed)['passed'])

    def test_relative_timing_reduces_two_independent_rounds(self):
        natives = {name: dict(rounds=[dict(round_id=rid, local_capacity_tps=50, sim_tick_ms=1000/60,
            steady_missing_frame_stalls=0, pace=dict(sim_ms_per_tick=20)) for rid in (7, 8)]) for name in ('a', 'b')}
        peers = {name: dict(configs=[dict(round=rid, sim_tick_ms=1000/60) for rid in (7, 8)], native_final_tick=303) for name in natives}
        events = {name: [dict(type='match_boundary', round=7, final_tick=303)] for name in natives}
        live = {name: [dict(round=rid, phase='live', tick=tick, wall_ms=rid*10000+tick*20)
                       for rid in (7, 8) for tick in range(300, 304)] for name in natives}
        result = cross_report.segmented_round_timing({}, peers, events, live, {name: [] for name in natives},
                                                    cross_report.round_capacity_evidence(natives))
        self.assertTrue(all(row['passed'] for rounds in result.values() for row in rounds.values()), result)
        natives['b']['rounds'].pop()
        result = cross_report.segmented_round_timing({}, peers, events, live, {name: [] for name in natives},
                                                    cross_report.round_capacity_evidence(natives))
        for rounds in result.values():
            self.assertTrue(rounds['7']['passed'])
            self.assertEqual(rounds['8']['status'], 'INCOMPLETE')

    def test_copy_can_be_explicitly_rebased_without_outside_writes(self):
        with tempfile.TemporaryDirectory() as folder:
            original, out = Path(folder)/'original', Path(folder)/'copy'
            peer = out/'first/host'; peer.mkdir(parents=True)
            (peer/'launch.json').write_text(json.dumps(dict(exit_code=0, timed_out=False)))
            scenario = dict(name='unit', peers=[dict(name='host')], runs=[dict(name='first')])
            capture = dict(scenario='unit', scenario_definition=scenario, source={}, exe={}, fps=3, started=video.stamp(),
                runs=[dict(name='first', root=str(original/'first'), size='640x360', peers=[dict(peer='host',
                    root=str(original/'first/host'), video_dir=str(original/'first/host/video'), probe_dir=str(original/'first/host-stage/probe'))])])
            (out/'capture.json').write_text(json.dumps(capture))
            writes = []
            def review(_scenario, run, destination):
                writes.append(destination)
                self.assertTrue(Path(run['peers'][0]['root']).is_relative_to(out))
            with (patch.object(video, 'review', side_effect=review), patch.object(video, 'scenario_manifest'),
                  patch.object(video, 'aggregate_review'), contextlib.redirect_stdout(io.StringIO())):
                video.finalize_only(SimpleNamespace(finalize_only=out, allow_recorded_root=True, metadata_only=True,
                                                   scratch_root=out, sheet_every=3))
            self.assertEqual(writes, [out/'first'])
            self.assertFalse(original.exists())

    def test_pending_and_failed_shares_remain_distinct(self):
        from test_inventory_oracle_evidence import run_split
        result = run_split.terminal_coverage(['a', 'b'], dict(a=3, b=1))
        self.assertEqual(result['failed'], {'b': 1})
        self.assertEqual(result['pending'], {'a': 3})
        self.assertEqual(result['status'], 'FAIL')

    def test_available_coverage_ignores_attempts_and_other_peers(self):
        events = dict(a=[dict(type='coverage', phase='live', gameplay_tick=True, event=event, amount=1, result='success')
                         for event in ('round_fired', 'reload_completed', 'thrown_release')], b=[])
        def weapons():
            return next(row for row in cross_report.coverage(events, dict(a={}, b={}), dict(scenario='soak')) if row['id'] == 'weapons')
        self.assertEqual(weapons()['status'], 'FAIL')
        events['b'] = copy.deepcopy(events['a'])
        self.assertEqual(weapons()['status'], 'PASS')
        events['b'][0]['result'] = 'attempt'
        self.assertEqual(weapons()['status'], 'FAIL')


if __name__ == '__main__':
    unittest.main()
