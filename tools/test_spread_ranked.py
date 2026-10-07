"""Ranked named routes exercise the real case allocator without engines."""
import contextlib
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import spread_peers as spread


class RankedTests(unittest.TestCase):
    def case(self, refusals, *, wait=0, peers=None, pins=None, boxes=None):
        case = object.__new__(spread.Case)
        case.peers = peers or [spread.Peer('host')]
        case.names = [peer.name for peer in case.peers]
        case.pins = spread.pairs(pins or 'host=FIRST|SECOND')
        case.registry, case.id, case.lane, case.peer_ports = Path('facts.json'), 'case', 'lane', {}
        case.members, case.pending, case.match = {}, [], spread.Match(51580, parameters=dict(runner_wait=wait))
        case.stack = contextlib.ExitStack()
        self.addCleanup(case.stack.close)
        calls = []
        boxes = boxes or [dict(name=name, kind='local', os='windows', hostname=name) for name in ('FIRST', 'SECOND', 'UNNAMED')]
        def rpc(box, action, value, **kwargs):
            calls.append(box['name'])
            if reason := refusals.get(box['name']):
                raise RuntimeError('capacity refused: '+reason)
            return dict(token='owned')
        case.backend = lambda:SimpleNamespace(rpc=rpc, probe=lambda box, **k:dict(hostname=box['hostname']), control=lambda b:'control')
        case.pool = SimpleNamespace(load_registry=lambda p:dict(boxes=boxes), static_reason=lambda *a:None,
                                    Needs=lambda **k:SimpleNamespace(**k))
        facts = SimpleNamespace(machine_name=lambda:'controller', process_start=lambda p:1)
        case.transport_module = SimpleNamespace(worker=SimpleNamespace(facts=facts))
        case.refuse = lambda name, box, reason:spread.SpreadRefusal(f'{name} on {box}: {reason}')
        return case, calls

    def test_first_says_no_then_second_runs_and_receipt_names_it(self):
        case, calls = self.case({'FIRST':'free memory 11.0 GB is below floor 12.0 GB'})
        case.allocate()
        self.assertEqual(calls, ['FIRST', 'SECOND'])
        self.assertEqual(case.assigned_boxes, {'host':'SECOND'})
        self.assertEqual([row['box'] for row in case.route_attempts['host']], ['FIRST', 'SECOND'])
        self.assertEqual(case.route_attempts['host'][0]['status'], 'NO')

    def test_both_say_no_and_return_both_boxes_and_reasons(self):
        case, calls = self.case({'FIRST':'live engine capacity is in use', 'SECOND':'off limits: owner'})
        with self.assertRaisesRegex(spread.SpreadRefusal, 'FIRST.*live engine capacity.*SECOND.*off limits'):
            case.allocate()
        self.assertEqual(calls, ['FIRST', 'SECOND'])

    def test_second_is_not_tried_until_first_wait_expires(self):
        case, calls = self.case({'FIRST':'CPU 100.0% exceeds 90% over the last 10 seconds'}, wait=40)
        clock, second_at = [0.0], []
        original = case.backend
        def backend():
            result = original()
            raw = result.rpc
            def rpc(box, *args, **kwargs):
                if box['name'] == 'SECOND':
                    second_at.append(clock[0])
                return raw(box, *args, **kwargs)
            result.rpc = rpc
            return result
        case.backend = backend
        with patch.object(spread.time, 'monotonic', side_effect=lambda:clock[0]), \
             patch.object(spread.time, 'sleep', side_effect=lambda seconds:clock.__setitem__(0, clock[0]+seconds)):
            case.allocate()
        self.assertEqual(second_at, [40])
        self.assertEqual(case.route_attempts['host'][0]['status'], 'WAIT EXPIRED')

    def test_actual_assignment_keeps_reviewed_peer_alone(self):
        peers = [spread.Peer('host', reviewed=True), spread.Peer('seat')]
        case, calls = self.case({}, peers=peers, pins='host=FIRST,seat2=FIRST|SECOND')
        case.allocate()
        self.assertEqual(case.assigned_boxes, {'host':'FIRST', 'seat':'SECOND'})
        self.assertEqual(calls, ['FIRST', 'SECOND'])

    def test_integrity_conflict_does_not_try_another_box(self):
        case, calls = self.case({'FIRST':'PORT CONFLICT 51580; case other'})
        with self.assertRaisesRegex(spread.SpreadRefusal, 'PORT CONFLICT'):
            case.allocate()
        self.assertEqual(calls, ['FIRST'])

    def test_prelaunch_capacity_change_tries_the_second_named_box(self):
        case, calls = self.case({})
        case.allocate()
        case.pool.live_reason = lambda box, needs, state:'live engine capacity is in use' if box['name']=='FIRST' else None
        prepared = []
        case.prepare_peer = lambda name:prepared.append(case.members[name][0]['name'])
        case.check_prepared_hashes = lambda:None
        case.guard = lambda:None
        case.directory = None
        case.ensure_peer_ready('host')
        self.assertEqual(case.assigned_boxes, {'host':'SECOND'})
        self.assertEqual(prepared, ['SECOND'])
        self.assertEqual(calls, ['FIRST', 'SECOND'])

    def test_reviewed_recorder_uses_the_named_windows_session_slot(self):
        peers = [spread.Peer('host', recorder=True, reviewed=True)]
        boxes = [dict(name='FIRST', kind='windows-task', os='windows', hostname='remote')]
        case, calls = self.case({}, peers=peers, pins='host=FIRST', boxes=boxes)
        case.match.parameters['peer_task_slots'] = {'host': 2}
        case.allocate()
        self.assertEqual(calls, ['FIRST'])
        box, claim, request, backend = case.members['host']
        self.assertEqual(box['requested_task_slot'], 2)
        self.assertTrue(claim['needs']['reviewed'])
        self.assertFalse(claim['needs']['share_ok'])

    def test_reviewed_recorder_still_requires_a_native_windows_runner(self):
        peers = [spread.Peer('host', recorder=True, reviewed=True)]
        boxes = [dict(name='FIRST', kind='posix-ssh', os='linux', hostname='remote')]
        case, calls = self.case({}, peers=peers, pins='host=FIRST', boxes=boxes)
        with self.assertRaisesRegex(spread.SpreadRefusal, 'private Windows recorder'):
            case.allocate()
        self.assertEqual(calls, [])

    def test_named_quiet_route_keeps_strict_admission_despite_old_timing_rating(self):
        peers = [spread.Peer('host', quiet=True)]
        boxes = [dict(name='FIRST', kind='windows-task', os='windows', hostname='remote', timing=False)]
        case, calls = self.case({}, peers=peers, pins='host=FIRST', boxes=boxes)
        case.allocate()
        self.assertEqual(calls, ['FIRST'])
        self.assertTrue(case.members['host'][1]['needs']['alone'])
        self.assertFalse(case.members['host'][1]['needs']['share_ok'])


if __name__ == '__main__':
    unittest.main()
