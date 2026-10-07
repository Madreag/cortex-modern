"""Admission detectors: named boxes, real engine shares and bounded transients."""
import contextlib
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import spread_peers as spread


class TransientTests(unittest.TestCase):
    def wait_then_launch(self, reasons, *, quiet=False):
        clock, attempts, launches = [0.0], [], []
        box = dict(name='NAMED', kind='posix')
        needs = SimpleNamespace(peer_id='seat', alone=quiet)
        def rpc(target, action, body, **kwargs):
            self.assertIs(target, box)
            self.assertEqual(action, 'claim')
            attempts.append(clock[0])
            if clock[0] < 40:
                reason = reasons[min(len(attempts)-1, len(reasons)-1)]
                raise RuntimeError(reason)
            launches.append(clock[0])
            return dict(token='owned')
        backend = SimpleNamespace(rpc=rpc, probe=lambda *a, **k:dict(exclusive_holders=[dict(live=True)]))
        with patch.object(spread.time, 'monotonic', side_effect=lambda:clock[0]), \
             patch.object(spread.time, 'sleep', side_effect=lambda seconds:clock.__setitem__(0, clock[0]+seconds)):
            self.assertEqual(spread.claim_named_peer(backend, box, needs, {}, wait=90), dict(token='owned'))
        self.assertGreater(len(attempts), 1)
        self.assertEqual(len(launches), 1)
        self.assertGreaterEqual(launches[0], 40)
        self.assertLessEqual(launches[0], 90)

    def test_fifo_and_start_spacing_wait_then_start(self):
        self.wait_then_launch(['capacity refused: earlier work request is waiting: earlier',
                               'capacity refused: minimum start spacing has not elapsed'])

    def test_live_alone_claim_waits_then_starts(self):
        self.wait_then_launch(['capacity refused: another run holds the box alone'])

    def test_capacity_mutex_waits_then_starts(self):
        self.wait_then_launch(['claim failed: RuntimeError: capacity update is busy; skip this box'])

    def test_cpu_waits_then_starts(self):
        self.wait_then_launch(['capacity refused: CPU 100.0% exceeds 90% over the last 10 seconds'])

    def test_ssh_waits_then_starts(self):
        self.wait_then_launch(['NAMED: ssh does not answer'])

    def test_preparation_reservations_do_not_use_engine_shares(self):
        raw = dict(present=True, actual_engines=0, engines=4, slots_in_use=4,
                   builds_in_use=0, free_gb=30, cpu_busy_percent=0,
                   jobs=[dict(engines=1, started_engines=0, alone=True, compiler=False) for _ in range(4)])
        state = spread.real_engine_capacity(raw)
        self.assertEqual((state['engines'], state['slots_in_use']), (0, 0))
        self.assertTrue(all(not job['alone'] for job in state['jobs']))
        self.wait_then_launch(['capacity refused: all 4 test shares are in use'])

    def test_build_waits_then_starts(self):
        self.wait_then_launch(['capacity refused: a build is running on the box'])

    def test_quiet_waits_and_starts_only_when_alone(self):
        self.wait_then_launch(['capacity refused: alone run needs an idle box'], quiet=True)

    def test_hard_refusals_never_wait_or_launch(self):
        for reason in ('off limits: owner withheld this box',
                       'free memory 11.0 GB is below floor 12.0 GB',
                       'live engine capacity is in use',
                       'operating system does not fit windows'):
            with self.subTest(reason=reason):
                backend = SimpleNamespace(rpc=unittest.mock.Mock(side_effect=RuntimeError('capacity refused: '+reason)))
                with patch.object(spread.time, 'sleep') as sleep, self.assertRaisesRegex(RuntimeError, reason):
                    spread.claim_named_peer(backend, dict(name='NAMED', kind='posix'),
                                            SimpleNamespace(peer_id='seat'), {}, wait=90)
                sleep.assert_not_called()
                self.assertEqual(backend.rpc.call_count, 1)

    def test_every_transient_stops_at_the_same_budget_without_launch(self):
        for reason in ('capacity refused: another run holds the box alone',
                       'NAMED: ssh does not answer', 'capacity refused: a build is running on the box'):
            with self.subTest(reason=reason):
                clock = [0.0]
                backend = SimpleNamespace(rpc=unittest.mock.Mock(side_effect=RuntimeError(reason)))
                with patch.object(spread.time, 'monotonic', side_effect=lambda:clock[0]), \
                     patch.object(spread.time, 'sleep', side_effect=lambda seconds:clock.__setitem__(0, clock[0]+seconds)), \
                     self.assertRaisesRegex(RuntimeError, 'expired after 10s'):
                    spread.claim_named_peer(backend, dict(name='NAMED', kind='posix'),
                                            SimpleNamespace(peer_id='seat'), {}, wait=10)
                self.assertEqual(clock[0], 10)

    def test_ssh_probe_uses_the_same_budget_before_claiming(self):
        clock, probes = [0.0], []
        box, needs = dict(name='NAMED', kind='posix'), SimpleNamespace(peer_id='seat')
        def rpc(target, action, body, **kwargs):
            probes.append(clock[0])
            if clock[0] < 20:
                raise RuntimeError('SSH does not answer or the saved connection is unavailable')
            return dict(present=True)
        backend = SimpleNamespace(rpc=rpc)
        backend.probe = lambda:backend.rpc(box, 'probe', {})
        with patch.object(spread.time, 'monotonic', side_effect=lambda:clock[0]), \
             patch.object(spread.time, 'sleep', side_effect=lambda seconds:clock.__setitem__(0, clock[0]+seconds)):
            waiter = spread.bind_admission_transport(backend, box, needs, 60)
            self.assertEqual(backend.probe(), dict(present=True))
            self.assertEqual(waiter.remaining(), 40)
        self.assertEqual(probes[-1], 20)

    def test_quiet_native_guard_does_not_launch_beside_an_engine(self):
        clock, starts, occupied = [0.0], [], [False]
        @contextlib.contextmanager
        def admission(*args):
            occupied[0] = True
            try:
                yield dict(engines=1 if clock[0] < 20 else 0)
            finally:
                occupied[0] = False
        load = SimpleNamespace(admission=admission, LoadRefusal=RuntimeError)
        def sleep(seconds):
            self.assertFalse(occupied[0], 'the native engine mutex must be released during a wait')
            self.assertEqual(starts, [])
            clock[0] += seconds
        with patch.object(spread.time, 'monotonic', side_effect=lambda:clock[0]), \
             patch.object(spread.time, 'sleep', side_effect=sleep), \
             spread.named_engine_cpu_wait(load, dict(name='NAMED'), SimpleNamespace(peer_id='host', alone=True), lambda:None, wait=60):
            with load.admission([], {}, {}, lambda:None) as state:
                self.assertEqual(state['engines'], 0)
                starts.append(clock[0])
        self.assertEqual(starts, [20])

    def test_real_engine_count_survives_normalization_and_input_is_unchanged(self):
        raw = dict(actual_engines=2, engines=6, slots_in_use=6,
                   jobs=[dict(engines=1, started_engines=0, alone=True)], builds_in_use=2, actual_builds=0)
        state = spread.real_engine_capacity(raw)
        self.assertEqual((state['engines'], state['slots_in_use'], state['builds_in_use']), (2, 2, 0))
        self.assertEqual(raw['engines'], 6)
        self.assertTrue(raw['jobs'][0]['alone'])
        self.assertEqual(spread.real_engine_capacity(state), state)

    def test_physical_limits_win_over_cpu_and_preparation_claims(self):
        box = dict(engines_max=4, free_floor_gb=12, engine_peak_gb=3.4)
        needs = SimpleNamespace(engines=1, memory=0)
        raw = dict(present=True, actual_engines=4, engines=8, slots_in_use=8, free_gb=30,
                   cpu_busy_percent=100, jobs=[dict(engines=1, started_engines=0, alone=True)])
        reason = lambda *args:'CPU 100.0% exceeds 90% over the last 10 seconds'
        self.assertEqual(spread.named_live_reason(reason, box, needs, raw), 'live engine capacity is in use')
        raw.update(actual_engines=0, free_gb=11)
        self.assertIn('needs 12.0 GB', spread.named_live_reason(reason, box, needs, raw))

    def test_quiet_launch_is_not_blocked_by_other_cases_preparation_claims(self):
        raw = dict(present=True, actual_engines=0, engines=4, slots_in_use=4, jobs=[
            dict(engines=1, started_engines=0, alone=True) for _ in range(4)])
        def original(box, needs, state):
            if state['jobs'] or state['engines'] or state['slots_in_use']:
                return 'alone run needs an idle box'
        self.assertIsNone(spread.named_live_reason(original, {}, SimpleNamespace(alone=True),
                                                   spread.real_engine_capacity(raw)))

    def test_active_transport_propagates_remaining_budget_through_the_existing_holder(self):
        clock, written = [0.0], []
        claim = dict(token='owned', root='/own')
        box, needs = dict(name='NAMED', kind='local'), SimpleNamespace(peer_id='host')
        backend = SimpleNamespace(active_claim=claim, rpc=lambda target, action, body, **k:written.append(body))
        def launch(target, own, request):
            backend.rpc(target, 'write', dict(path='/own/request.json', value=dict(claim=own,
                argv=['python', '/kit/box_hold.py', '--wait', '0', '--label', 'case', '--', 'python', 'native'])))
        backend.launch = launch
        with patch.object(spread.time, 'monotonic', side_effect=lambda:clock[0]):
            retry = spread.bind_admission_transport(backend, box, needs, 60)
            clock[0] = 20
            spread.launch_native(backend, box, claim, {}, retry.remaining())
        self.assertEqual(claim['runner_wait_remaining'], 40)
        self.assertEqual(written[0]['value']['argv'][3], '40')


if __name__ == '__main__':
    unittest.main()
