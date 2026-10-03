import contextlib
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

import edith_cross
import soak_two_peer


def control(tick=1101, **changes):
    return dict(dict(type='recovery', recovery_phase='first_controllable_input', round=7, peer=2, incarnation=0,
                     terminal=True, controllable=True, held=False, catchup=False, tick=tick, wire_tick=tick, actor=9, player=0,
                     input=dict(actor=9, seat=0, input_round=7, target_tick=tick, produced_tick=tick-1,
                                produced_wall_ms=1000, queue_confirmed=True, input_serial=1)), **changes)


def recovery_pair(root, *, wait='', event=None):
    for peer, seat in [('host', 1), ('client', 2)]:
        (root / peer).mkdir()
        (root / peer / 'stdout.log').write_text(f'[net-lockstep] start round=7 frame=1 local_peer={seat} peers=2\n' +
            ('[net-match] hold peer=2 frame=1005 AI in control\n' + wait if peer == 'host' else
             '[net-test] live stall frame=1000 ms=1500\n[net-match] seat-reclaimed peer=2 frame=1100 live_actors=2\n'))
    (root / 'client/events.jsonl').write_text(json.dumps(control() if event is None else event) + '\n')


def analyzed_pair(root, *, timing=True):
    result = dict(name='unit', off_wire_pass=True, peers={'host': dict(pass_check=timing)},
                  proof=dict(live_passes={'host/client': [dict(compared_ticks=1201, mismatched_ticks=0,
                                                              mismatched_applied_input_ticks=0)]}))
    harness = SimpleNamespace(records=SimpleNamespace(compress_case_records=Mock()),
        feel=SimpleNamespace(reduce_timing_case=Mock(return_value=result),
                             timing_peer=Mock(return_value=dict(pass_check=timing))))
    for name in ('host', 'client'):
        (root / name).mkdir()
        (root / f'{name}-record.json').write_text(json.dumps(dict(exit_code=0, evidence_complete=True, exe_sha256='a' * 64)))
        (root / f'{name}-build.json').write_text(json.dumps(dict(source_sha='b'*40, build=dict(commit='b'*40, executable_sha256='a'*64))))
        (root / name / 'stdout.log').write_text('[net-route] RouteAllowed route=direct allowed=1\n')
    meta = dict(name='unit', path='direct', source_sha='b'*40, feel_records=False, machines=dict(host='EROL-PC', client='EDITH'), local_peer='host')
    return harness, meta


class SoakOracleEvidence(unittest.TestCase):
    def test_timing_uses_complete_native_capacity_of_a_uniformly_heavy_round(self):
        from feel.report import item9a_gates
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'host').mkdir()
            (root / 'host/stdout.log').write_text('')
            (root / 'manifest.json').write_text(json.dumps(dict(ticks=1200)))
            native = dict(pace=dict(sim_ms_per_tick=20), lockstep=dict(round_id=7, local_capacity_tps=50,
                sim_tick_ms=1000/60, next_frame=1201, missing_frame_stalls=0, steady_missing_frame_stalls=0))
            for peer in ('host', 'client'):
                (root / f'{peer}_report.json').write_text(json.dumps(native))
            rows = [dict(type='committed', tick=tick, wall_ms=tick*20) for tick in (300, 1200)]
            self.assertTrue(item9a_gates(root, rows=rows)['pass_check'])
            (root / 'client_report.json').write_text('{}')
            result = item9a_gates(root, rows=rows)
            self.assertEqual(result['pins']['item9a_wall_tps']['status'], 'FAIL')
            self.assertEqual(result['pins']['item9a_confirmed_horizon_lag']['status'], 'FAIL')

    def test_each_planned_injection_needs_its_own_recovery_and_bounded_wait(self):
        for case in ('clean', 'wait_at_61', 'wrong_peer', 'wrong_round', 'wrong_incarnation', 'not_fresh', 'missing_fault'):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                event = control(**({'peer': 1} if case == 'wrong_peer' else {'round': 8} if case == 'wrong_round' else
                                   {'incarnation': 1} if case == 'wrong_incarnation' else {}))
                if case == 'not_fresh': event['input']['produced_tick'] = 1099
                recovery_pair(root, wait='[net-frame-wait] frame=1066 wait_ms=51\n' if case == 'wait_at_61' else '', event=event)
                planned = [1000, 2000] if case == 'missing_fault' else [1000]
                result = soak_two_peer.soak_hold_judgement(root, planned)
                self.assertEqual(result['passed'], case == 'clean', result)

    def test_plan_names_every_host_client_and_rematch_fault(self):
        plan = soak_two_peer.stall_plan([1000], 1500, ['2000:1000'], 500, 3)
        self.assertEqual([(row['peer'], row['tick'], row['round_index']) for row in plan],
                         [('client', 1000, 0), ('host', 2000, 0), ('client', 500, 0), ('client', 500, 1), ('client', 500, 2)])

    def test_soak_failure_is_required_with_good_timing(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            harness, meta = analyzed_pair(root)
            (root / 'soak-verdict.json').write_text(json.dumps(dict(passed=False, stalls=[])))
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertFalse(edith_cross.analyze_match(harness, root, meta)['passed'])

    def test_clean_pair_passes_without_injected_faults(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            harness, meta = analyzed_pair(root)
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertTrue(edith_cross.analyze_match(harness, root, meta)['passed'])

    def test_p17_failed_soak_and_timing_reach_the_pair_verdict(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            harness, meta = analyzed_pair(root, timing=False)
            (root / 'soak-verdict.json').write_text(json.dumps(dict(passed=False, stalls=[])))
            with patch.object(soak_two_peer, 'soak_hold_judgement', return_value=dict(passed=True, planned=[], explained=[], unexplained=[])), \
                 contextlib.redirect_stdout(io.StringIO()):
                result = edith_cross.analyze_match(harness, root, meta)
        self.assertFalse(result['passed'], result)

    def test_timing_failure_is_required_without_a_soak(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            harness, meta = analyzed_pair(root, timing=False)
            with contextlib.redirect_stdout(io.StringIO()):
                result = edith_cross.analyze_match(harness, root, meta)
        self.assertFalse(result['passed'], result)

    def test_p18_unrelated_bootstrap_and_late_survivor_wait_do_not_prove_recovery(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for peer in ('host', 'client'):
                (root / peer).mkdir()
            (root / 'host/stdout.log').write_text(
                '[net-match] hold peer=1 frame=2500 AI in control\n'
                '[lockstep-recv] asked peer 1 to resend frame=2500 of peer 1 reason=missing\n'
                '[net-frame-wait] frame=2561 wait_ms=90\n')
            (root / 'client/stdout.log').write_text('[net-match] bootstrap checkpoint=2620 local_peer=2\n')
            result = soak_two_peer.soak_hold_judgement(root, [])
        self.assertFalse(result['passed'], result)


if __name__ == '__main__':
    unittest.main()
