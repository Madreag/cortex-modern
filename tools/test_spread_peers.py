"""Detect boundary and ownership defects in the shared peer interface, without engines."""
import base64
import contextlib
import errno
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
import subprocess
import sys
from unittest.mock import patch

import spread_peers as spread
from test_named_spread import NamedRoutingTests
from test_peer_run_guards import PeerRunGuardTests


class ContractTests(unittest.TestCase):
    def exclusive_fake(self, root, clock, *, live):
        marker = root/'exclusive.lock'
        marker.write_text('owner evidence')
        owner = dict(pid=7, process_start=12.0, machine='NAMED', label='timing window', token='other')
        facts = SimpleNamespace(machine_name=lambda:'NAMED', process_start=lambda pid:12.0 if live else None,
                                read_reservation=lambda path, **kwargs:owner if live else None)
        box = dict(name='NAMED', kind='local', markers=dict(exclusive=str(marker)))
        def raw_probe(target, **kwargs):
            self.assertIs(target, box)
            if clock[0] >= 20 and live:
                marker.unlink(missing_ok=True)
            return dict(reason='another run holds the box alone' if marker.exists() else None, jobs=[])
        def probe(target, **kwargs):
            # The native worker enriches this unchanged capacity probe.
            raw_probe(target, **kwargs)
            return spread.native_exclusive_state(raw_probe, target, facts=facts, root=root, **kwargs)
        return box, marker, probe

    def test_live_exclusive_claim_waits_twenty_seconds_before_launch(self):
        for phase in ('claim', 'prepared', 'final'):
            with self.subTest(phase=phase), tempfile.TemporaryDirectory() as temporary:
                clock, starts, locked, renewals = [0.0], [], [False], []
                box, marker, probe = self.exclusive_fake(Path(temporary), clock, live=True)
                claim = dict(token='owned', needs=dict(peer_id='host', alone=False), runner_wait=60)
                needs = SimpleNamespace(**claim['needs'])
                def reason(box, needs, state):
                    return state.get('refusal') or state['reason']
                def rpc(target, action, body, **kwargs):
                    self.assertIs(target, box)
                    self.assertEqual(action, 'claim')
                    if refusal := reason(target, needs, probe(target)):
                        raise RuntimeError('capacity refused: '+refusal)
                    starts.append(clock[0])
                    return claim
                @contextlib.contextmanager
                def mutex(*args, **kwargs):
                    self.assertFalse(locked[0])
                    locked[0] = True
                    try: yield
                    finally: locked[0] = False
                def sleep(seconds):
                    self.assertFalse(locked[0])
                    self.assertEqual(starts, [])
                    clock[0] += seconds
                worker = SimpleNamespace(capacity_state=probe, renew_claim=lambda value:renewals.append(value))
                pool = SimpleNamespace(Needs=lambda **values:SimpleNamespace(**values), live_reason=reason)
                backend = SimpleNamespace(rpc=rpc, probe=probe)
                with patch.object(spread.time, 'monotonic', side_effect=lambda:clock[0]), \
                     patch.object(spread.time, 'sleep', side_effect=sleep):
                    if phase == 'claim':
                        self.assertIs(spread.claim_named_peer(backend, box, needs, {}, wait=60), claim)
                    elif phase == 'prepared':
                        spread.wait_named_launch(worker, pool, box, claim, wait=60)
                        starts.append(clock[0])
                    else:
                        with spread.named_launch_capacity(box, needs, claim, probe=probe, live_reason=reason,
                                mutex=mutex, root=Path(temporary), renew=lambda value:renewals.append(value)):
                            self.assertTrue(locked[0])
                            starts.append(clock[0])
                self.assertEqual(starts, [20.0])
                self.assertFalse(marker.exists())
                self.assertFalse(locked[0])
                if phase != 'claim':
                    self.assertEqual(len(renewals), 10)
        for wait in (0, 10):
            with self.subTest(wait=wait), tempfile.TemporaryDirectory() as temporary:
                clock, starts = [0.0], []
                box, marker, probe = self.exclusive_fake(Path(temporary), clock, live=True)
                def rpc(target, action, body, **kwargs):
                    if probe(target)['reason']:
                        raise RuntimeError('capacity refused: another run holds the box alone')
                    starts.append(clock[0])
                with patch.object(spread.time, 'monotonic', side_effect=lambda:clock[0]), \
                     patch.object(spread.time, 'sleep', side_effect=lambda seconds:clock.__setitem__(0,clock[0]+seconds)), \
                     self.assertRaisesRegex(RuntimeError, 'expired after 10s' if wait else 'capacity refused'):
                    spread.claim_named_peer(SimpleNamespace(rpc=rpc, probe=probe), box,
                                            SimpleNamespace(peer_id='host'), {}, wait=wait)
                self.assertEqual(starts, [])
                self.assertEqual(clock[0], wait)
                self.assertTrue(marker.exists())

    def test_ownerless_exclusive_marker_refuses_by_name_without_wait_or_archive(self):
        with tempfile.TemporaryDirectory() as temporary:
            box, marker, probe = self.exclusive_fake(Path(temporary), [0.0], live=False)
            claim = dict(token='owned', needs=dict(peer_id='host', alone=False), runner_wait=60)
            needs = SimpleNamespace(**claim['needs'])
            reason = lambda box, needs, state:state.get('refusal') or state['reason']
            def rpc(target, action, body, **kwargs):
                raise RuntimeError('capacity refused: '+reason(target, needs, probe(target)))
            worker = SimpleNamespace(capacity_state=probe, renew_claim=unittest.mock.Mock())
            pool = SimpleNamespace(Needs=lambda **values:SimpleNamespace(**values), live_reason=reason)
            with patch.object(spread.time, 'sleep') as sleep:
                for phase in ('claim', 'prepared', 'final'):
                    with self.subTest(phase=phase), self.assertRaisesRegex(RuntimeError, 'ownerless exclusive marker.*exclusive.lock'):
                        if phase == 'claim':
                            spread.claim_named_peer(SimpleNamespace(rpc=rpc, probe=probe), box, needs, {}, wait=60)
                        elif phase == 'prepared':
                            spread.wait_named_launch(worker, pool, box, claim, wait=60)
                        else:
                            with spread.named_launch_capacity(box, needs, claim, probe=probe, live_reason=reason,
                                    mutex=lambda *args, **kwargs:contextlib.nullcontext(), root=Path(temporary), renew=worker.renew_claim):
                                self.fail('an ownerless marker cannot launch')
                sleep.assert_not_called()
                worker.renew_claim.assert_not_called()
            self.assertEqual(marker.read_text(), 'owner evidence')

    def test_worker_exclusive_extension_runs_before_main_and_keeps_positional_probe_calls(self):
        source = ('def launch():\n'
                  '    if True:\n'
                  '        if True:\n'
                  "            with mutex(root_for(box)/'.capacity.lock',wait=15):\n"
                  "                before=capacity_state(box,refresh_display=False,ignore_token=claim['token'])\n"
                  "                if reason:=live_reason(box,Needs(**claim['needs']),before):raise RuntimeError('capacity changed before launch: '+reason)\n"
                  '                pass\n'
                  "if __name__=='__main__':raise SystemExit(main())\n")
        with tempfile.TemporaryDirectory() as temporary:
            seen = []
            facts = SimpleNamespace(read_reservation=lambda *args, **kwargs:None)
            box = dict(name='NAMED', markers={})
            def probe(target, **kwargs):
                seen.append(kwargs)
                return dict(jobs=[])
            def main():
                value = namespace['capacity_state'](box, True, False, 'owned')
                self.assertEqual(value['exclusive_holders'], [])
                return 0
            namespace = dict(__name__='__main__', capacity_state=probe, facts=facts,
                             root_for=lambda box:Path(temporary), main=main)
            with self.assertRaises(SystemExit) as result:
                exec(compile(spread.native_cpu_wait_source(source), '<native-worker>', 'exec'), namespace)
            self.assertEqual(result.exception.code, 0)
            self.assertEqual(seen, [dict(read_only=True, refresh_display=False, ignore_token='owned')])

    def test_posix_quiet_marker_waits_for_its_live_owner_and_preserves_ownerless_evidence(self):
        for live in (True, False):
            with self.subTest(live=live), tempfile.TemporaryDirectory() as temporary:
                clock, writes, renewals = [0.0], [], []
                marker = Path(temporary)/'quiet.lock'
                marker.write_text('previous owner evidence')
                previous = dict(pid=7, process_start=12.0, machine='NAMED', label='previous window', token='other')
                def read(path, **kwargs):
                    self.assertFalse(kwargs['archive'])
                    if live and clock[0] >= 20:
                        marker.unlink(missing_ok=True)
                    return previous if marker.exists() else None
                def write(path, label, **kwargs):
                    self.assertFalse(marker.exists())
                    self.assertEqual(clock[0], 20)
                    self.assertEqual(kwargs['token'], 'owned')
                    marker.write_text('owned window')
                    writes.append(label)
                facts = SimpleNamespace(read_reservation=read, write_reservation=write,
                                        machine_name=lambda:'NAMED', process_start=lambda pid:12.0 if live else None)
                box = dict(name='NAMED', kind='posix')
                claim = dict(token='owned', needs=dict(peer_id='client'), runner_wait=60)
                with patch.object(spread.time, 'monotonic', side_effect=lambda:clock[0]), \
                     patch.object(spread.time, 'sleep', side_effect=lambda seconds:clock.__setitem__(0,clock[0]+seconds)) as sleep:
                    if live:
                        spread.claim_native_exclusive_marker(box, claim, marker, 'own window', facts=facts,
                                                             renew=lambda value:renewals.append(value))
                        self.assertEqual(writes, ['own window'])
                        self.assertEqual(len(renewals), 10)
                    else:
                        with self.assertRaisesRegex(spread.SpreadRefusal, 'ownerless exclusive marker.*quiet.lock'):
                            spread.claim_native_exclusive_marker(box, claim, marker, 'own window', facts=facts,
                                                                 renew=lambda value:renewals.append(value))
                        sleep.assert_not_called()
                        self.assertEqual(marker.read_text(), 'previous owner evidence')
                        self.assertEqual((writes, renewals), ([], []))

    def test_direct_host_route_to_named_lan_peer_does_not_select_its_overlay(self):
        with patch.object(spread.sys, 'platform', 'win32'), \
             patch.object(spread.subprocess, 'run', return_value=SimpleNamespace(returncode=0, stdout='192.0.2.10\n')) as query:
            self.assertEqual(spread.native_address_probe('192.0.2.20'), '192.0.2.10')
        self.assertIn('Find-NetRoute', query.call_args.args[0][-1])
        self.assertIn('192.0.2.20', query.call_args.args[0][-1])
        self.assertEqual(query.call_count, 1)

    def test_named_endpoint_reads_ssh_facts_without_choosing_a_box(self):
        with patch.object(spread.subprocess, 'run', return_value=SimpleNamespace(returncode=0, stdout='hostname named-peer.local\n')) as query, \
             patch.object(spread.socket, 'getaddrinfo', return_value=[(None,None,None,None,('192.0.2.30',0))]):
            self.assertEqual(spread.named_box_endpoint(dict(name='SecondPosixBox', kind='posix', ssh='saved-peer')), '192.0.2.30')
        self.assertEqual(query.call_args.args[0], ['ssh','-G','saved-peer'])

    def test_concurrent_seat_waits_for_requested_host_but_keeps_caller_order(self):
        host = SimpleNamespace(started=False, launch_attempted=True)
        case = SimpleNamespace(names=['host','seat'], runs={'host':host}, guard=unittest.mock.Mock())
        def idle(seconds):
            self.assertFalse(host.started)
            host.started = True
        with patch.object(spread.time, 'sleep', side_effect=idle) as sleep:
            spread.wait_started_host(case, 'seat')
        sleep.assert_called_once_with(.1)
        case.guard.assert_called_once()
        host.started, host.launch_attempted = False, False
        with patch.object(spread.time, 'sleep') as sleep:
            spread.wait_started_host(case, 'seat')
        sleep.assert_not_called()
        host.launch_attempted, host.start_failure = True, 'named native floor refused'
        with self.assertRaisesRegex(spread.SpreadRefusal, 'requested host did not start.*floor'):
            spread.wait_started_host(case, 'seat')

    def test_session_wait_returns_the_hosts_named_admission_failure_without_launching_a_seat(self):
        case = object.__new__(spread.Case)
        case.names, case.peer_ports, case.match = ['host','seat'], {}, spread.Match(51580)
        case.members = {'host':(dict(name='EROL-PC'),), 'seat':(dict(name='Linux'),)}
        case.runs = {'host':SimpleNamespace(start_failure='capacity update is busy; skip this box')}
        case.synchronize = unittest.mock.Mock()
        case.refuse = lambda name, box, reason:spread.SpreadRefusal(f'spread peer {name} on {box}: {reason}')
        with patch('e2e_video.directory_session') as query, patch.object(spread.time, 'sleep') as sleep:
            with self.assertRaisesRegex(spread.SpreadRefusal, 'seat on Linux.*host on EROL-PC.*capacity update is busy'):
                case.published_session('seat')
            query.assert_not_called()
            sleep.assert_not_called()
            case.synchronize.assert_not_called()

    def session_admission_fake(self, clock, failure=None):
        host = SimpleNamespace(started=False, launch_attempted=True, finished=False, start_failure=None)
        case = object.__new__(spread.Case)
        case.names, case.peer_ports = ['host', 'seat'], {}
        case.match = spread.Match(51580, parameters={'runner_wait':1800})
        case.directory = {'DIRECTORY_ROOT':'unused'}
        case.members = {'host':(dict(name='EROL-PC'),), 'seat':(dict(name='Linux'),)}
        case.runs = {'host':host}
        def synchronize():
            if failure and clock[0] >= 20:
                host.start_failure = failure
            elif clock[0] >= 100:
                host.started = True
        def idle(seconds):
            clock[0] = round(clock[0]+seconds, 6)
            synchronize()
        case.guard, case.synchronize = synchronize, synchronize
        case.refuse = lambda name, box, reason: spread.SpreadRefusal(f'spread peer {name} on {box}: {reason}')
        return case, host, idle

    def test_session_lookup_starts_after_the_requested_host_is_admitted(self):
        clock, queries = [0.0], []
        case, host, idle = self.session_admission_fake(clock)
        def query(*args):
            queries.append(clock[0])
            return 'native-session' if host.started else None
        with patch.object(spread.time, 'monotonic', side_effect=lambda:clock[0]), \
             patch.object(spread.time, 'sleep', side_effect=idle), patch('e2e_video.directory_session', side_effect=query):
            self.assertEqual(case.published_session('seat'), 'native-session')
        self.assertEqual(queries, [100.0])

    def test_session_lookup_still_refuses_after_eighty_seconds_without_publication(self):
        clock = [0.0]
        case, host, idle = self.session_admission_fake(clock)
        with patch.object(spread.time, 'monotonic', side_effect=lambda:clock[0]), \
             patch.object(spread.time, 'sleep', side_effect=idle), patch('e2e_video.directory_session', return_value=None):
            with self.assertRaisesRegex(spread.SpreadRefusal, 'seat on Linux.*no session.*51580'):
                case.published_session('seat')
        self.assertTrue(host.started)
        self.assertEqual(clock[0], 180.0)

    def test_host_refusal_during_admission_reaches_session_waiter_before_lookup(self):
        clock = [0.0]
        case, host, idle = self.session_admission_fake(clock, 'named native floor refused')
        with patch.object(spread.time, 'monotonic', side_effect=lambda:clock[0]), \
             patch.object(spread.time, 'sleep', side_effect=idle), patch('e2e_video.directory_session') as query:
            with self.assertRaisesRegex(spread.SpreadRefusal, 'seat on Linux.*host on EROL-PC.*floor refused'):
                case.published_session('seat')
            query.assert_not_called()
        self.assertFalse(host.started)
        self.assertEqual(clock[0], 20.0)

    def test_native_adapter_ships_its_existing_named_route_dependency(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root/'box_facts.py').write_text('def facts():\n    from pool_cohort import overlay\n')
            (root/'pool_run.py').write_text('def launch(): return True\n')
            (root/'pool_cohort.py').write_text('def overlay(value, target): return value\n')
            sources = spread.native_adapter_sources(root/'box_facts.py')
            self.assertEqual(list(sources), ['pool_cohort.py', 'pool_run.py'])
            self.assertEqual(sources['pool_cohort.py'], (root/'pool_cohort.py').read_text())
            self.assertEqual(sources['pool_run.py'], (root/'pool_run.py').read_text())

    def test_named_route_reader_is_shipped_from_the_installed_kit_when_facts_has_no_copy(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            kit = root / 'kit'
            kit.mkdir()
            (kit/'pool_cohort.py').write_text('NAME="named"\n')
            sources = spread.native_adapter_sources(root/'box_facts.py', {'box_facts.py': 'import pool_cohort\n'}, kit=kit)
            self.assertEqual(list(sources), ['pool_cohort.py', 'box_facts.py'])
            self.assertEqual(sources['pool_cohort.py'], (kit/'pool_cohort.py').read_text())

    def test_native_bootstrap_loads_route_reader_before_facts_and_adapter_after(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root/'pool_cohort.py').write_text("NAME = 'named'\n")
            (root/'pool_run.py').write_text('import box_facts\nNAME = box_facts.NAME\n')
            existing = {'box_facts.py': 'import pool_cohort\nNAME = pool_cohort.NAME\n',
                        'pool_worker.py': 'import box_facts\n'}
            sources = spread.native_adapter_sources(root/'box_facts.py', existing)
            boot = ('import json,sys,types\n'
                    'for name,source in json.load(sys.stdin).items():\n'
                    ' m=types.ModuleType(name[:-3]);sys.modules[name[:-3]]=m;exec(source,m.__dict__)\n'
                    'assert sys.modules["pool_run"].NAME=="named"\n')
            done = subprocess.run([sys.executable, '-B', '-c', boot], input=json.dumps(sources),
                                  capture_output=True, text=True, timeout=10)
            self.assertEqual(done.returncode, 0, done.stderr)

    def test_cpu_guard_waits_on_the_named_box_and_quiet_peer_waits_for_idle(self):
        for quiet, loads in ((False, [(100, 1), (20, 1)]),
                             (True, [(100, 1), (20, 1), (20, 0)])):
            with self.subTest(quiet=quiet):
                clock, locked, launched, seen, renewed = [0.0], [False], [], [], []
                box, claim = dict(name='NAMED', kind='local'), dict(token='owned')
                needs = SimpleNamespace(peer_id='host', alone=quiet)
                @contextlib.contextmanager
                def mutex(*args, **kwargs):
                    self.assertFalse(locked[0])
                    locked[0] = True
                    try: yield
                    finally: locked[0] = False
                def probe(target, **kwargs):
                    self.assertTrue(locked[0])
                    self.assertIs(target, box)
                    self.assertEqual(kwargs['ignore_token'], 'owned')
                    seen.append(clock[0])
                    cpu, engines = loads[len(seen)-1]
                    clock[0] += 10  # the native CPU sample takes ten seconds
                    return dict(cpu=cpu, engines=engines)
                def reason(target, requested, state):
                    if state['cpu'] > 90:
                        return 'CPU 100.0% exceeds 90% over the last 10 seconds'
                    if requested.alone and state['engines']:
                        return 'alone run needs an idle box'
                def sleep(seconds):
                    self.assertFalse(locked[0])
                    self.assertEqual(launched, [])
                    self.assertEqual(seconds, 20)
                    clock[0] += seconds
                with patch.object(spread.time, 'monotonic', side_effect=lambda:clock[0]), \
                     patch.object(spread.time, 'sleep', side_effect=sleep):
                    with spread.named_launch_capacity(box, needs, claim, probe=probe, live_reason=reason,
                            mutex=mutex, root=Path('/own'), renew=lambda value:renewed.append(value)) as state:
                        self.assertTrue(locked[0])
                        launched.append(state)
                self.assertEqual(seen, [0, 30, 60] if quiet else [0, 30])
                self.assertEqual(renewed, [claim]*(len(seen)-1))
                self.assertEqual(len(launched), 1)
                self.assertFalse(locked[0])

    def test_capacity_lock_waits_forty_seconds_then_launches_within_the_runner_budget(self):
        for wait, held, expected in ((1800, 40, 'start'), (20, 40, 'expire'),
                                     (1800, 200, 'expire'), (0, 40, 'legacy')):
            with self.subTest(wait=wait, held=held):
                clock, locked, starts, budgets = [0.0], [False], [], []
                box = dict(name='NAMED', kind='local')
                needs = SimpleNamespace(peer_id='host', alone=False)
                claim = dict(token='owned', runner_wait=wait)
                @contextlib.contextmanager
                def mutex(path, *, wait):
                    self.assertEqual(path, Path('/named/.capacity.lock'))
                    budgets.append(wait)
                    spread.time.sleep(min(wait, held-clock[0]))
                    if clock[0] < held:
                        raise RuntimeError('capacity update is busy; skip this box')
                    locked[0] = True
                    try: yield
                    finally: locked[0] = False
                def probe(target, **kwargs):
                    self.assertTrue(locked[0])
                    self.assertEqual(kwargs['ignore_token'], 'owned')
                    self.assertIs(target, box)
                    return dict(free_gb=20, engines=0)
                with patch.object(spread.time, 'monotonic', side_effect=lambda:clock[0]), \
                     patch.object(spread.time, 'sleep', side_effect=lambda seconds:clock.__setitem__(0,clock[0]+seconds)):
                    if expected == 'start':
                        with spread.named_launch_capacity(box, needs, claim, probe=probe, live_reason=lambda *args:None,
                                mutex=mutex, root=Path('/named'), renew=lambda value:None):
                            starts.append(clock[0])
                        self.assertEqual(starts, [40])
                        self.assertEqual(budgets, [180])
                    else:
                        with self.assertRaisesRegex(RuntimeError, r'NAMED.*host.*capacity.lock.*expired'):
                            with spread.named_launch_capacity(box, needs, claim, probe=probe, live_reason=lambda *args:None,
                                    mutex=mutex, root=Path('/named'), renew=lambda value:None):
                                self.fail('capacity lock cannot admit before it is free')
                        self.assertEqual(starts, [])
                        self.assertEqual(clock[0], 15 if expected == 'legacy' else min(wait,180))
                self.assertFalse(locked[0])

    def test_named_cpu_retry_expires_after_ten_minutes_and_preserves_other_guards(self):
        clock = [0.0]
        box = dict(name='NAMED', kind='posix')
        needs = SimpleNamespace(peer_id='seat2', alone=False)
        reason = 'capacity refused: CPU 100.0% exceeds 90% over the last 10 seconds'
        backend = SimpleNamespace(rpc=unittest.mock.Mock(side_effect=RuntimeError(reason)))
        with patch.object(spread.time, 'monotonic', side_effect=lambda:clock[0]), \
             patch.object(spread.time, 'sleep', side_effect=lambda seconds:clock.__setitem__(0,clock[0]+seconds)):
            with self.assertRaisesRegex(spread.SpreadRefusal, 'expired after 600s.*CPU 100'):
                spread.claim_named_peer(backend, box, needs, {})
        self.assertEqual(clock[0], 600)
        self.assertEqual(backend.rpc.call_count, 21)
        self.assertTrue(all(call.args[0] is box for call in backend.rpc.call_args_list))
        for refusal in ('capacity refused: live engine capacity is in use',
                        'capacity refused: free memory 11 GB is below floor 12 GB'):
            backend.rpc.reset_mock(side_effect=True)
            backend.rpc.side_effect = RuntimeError(refusal)
            with self.subTest(refusal=refusal), patch.object(spread.time, 'sleep') as sleep:
                with self.assertRaisesRegex(RuntimeError, 'capacity refused'):
                    spread.claim_named_peer(backend, box, needs, {})
                sleep.assert_not_called()

    def test_initial_capacity_claim_has_the_same_lock_budget_and_rpc_allowance(self):
        source = ("def capacity_claim(box,needs,request):\n"
                  "    root=root_for(box)\n"
                  "    with mutex(root/'.capacity.lock',wait=15):\n"
                  "        return dict(token=request['token'])\n"
                  "def launch():\n"
                  "    if True:\n"
                  "        if True:\n"
                  "            with mutex(root_for(box)/'.capacity.lock',wait=15):\n"
                  "                before=capacity_state(box,refresh_display=False,ignore_token=claim['token'])\n"
                  "                if reason:=live_reason(box,Needs(**claim['needs']),before):raise RuntimeError('capacity changed before launch: '+reason)\n"
                  "                pass\n"
                  "if __name__=='__main__':raise SystemExit(main())\n")
        clock, calls = [0.0], []
        box = dict(name='NAMED', kind='local')
        @contextlib.contextmanager
        def mutex(path, *, wait):
            self.assertEqual((path,wait),(Path('/named/.capacity.lock'),180))
            spread.time.sleep(40)
            yield
        namespace = dict(__name__='test_worker', capacity_state=lambda *args, **kwargs:{},
                         root_for=lambda box:Path('/named'), mutex=mutex)
        exec(compile(spread.native_cpu_wait_source(source), '<claim-worker>', 'exec'), namespace)
        request = dict(token='owned')
        def rpc(target, action, body, **kwargs):
            self.assertIs(target, box)
            self.assertEqual(action, 'claim')
            self.assertEqual(kwargs['timeout'], 210)
            self.assertEqual(body['request']['runner_wait'], 1800)
            self.assertEqual(body['request']['capacity_wait_remaining'], 1800)
            calls.append(clock[0])
            return namespace['capacity_claim'](target, body['needs'], body['request'])
        with patch.object(spread.time, 'monotonic', side_effect=lambda:clock[0]), \
             patch.object(spread.time, 'sleep', side_effect=lambda seconds:clock.__setitem__(0,clock[0]+seconds)):
            result = spread.claim_named_peer(SimpleNamespace(rpc=rpc), box, SimpleNamespace(peer_id='host'), request, wait=1800)
        self.assertEqual((result,clock[0],calls),(dict(token='owned'),40,[0]))
        self.assertEqual(request,dict(token='owned'))

    def test_native_admission_wait_ends_before_the_case_timer_and_checks_ownership(self):
        states = [dict(token='owned'), dict(token='owned', driver_pid=7)]
        def rpc(box, action, body, **kwargs):
            if action == 'run-state':
                return dict(state=states.pop(0))
            if body['path'].endswith('/progress.json') and not states:
                return dict(text=json.dumps(dict(pid=11, identity=dict(machine_id='named'))))
            return dict(text=None)
        backend = SimpleNamespace(rpc=rpc)
        with patch.object(spread.time, 'sleep') as sleep:
            spread.wait_native_admission(backend, dict(name='NAMED'), dict(token='owned', root='/own'))
        sleep.assert_called_once_with(2)
        backend.rpc = lambda *args, **kwargs:dict(state=dict(token='foreign', driver_pid=7))
        with self.assertRaisesRegex(spread.SpreadRefusal, 'changed owner'):
            spread.wait_native_admission(backend, dict(name='NAMED'), dict(token='owned', root='/own'))

    def test_final_engine_admission_retries_only_cpu_and_retains_its_receipt(self):
        class LoadRefusal(SystemExit): pass
        values = dict(engines=0, max_engines=4, free_gb=20, free_floor_gb=12,
                      cpu_busy_percent=100, cpu_busy_limit=90, cpu_sample_s=10, alone=None)
        calls, clock, entered, renewed = [], [0.0], [False], []
        @contextlib.contextmanager
        def admission(argv, environment, record, save):
            calls.append(clock[0])
            if len(calls) == 1:
                record.update(not_started='box overburdened', result_status='not started', exit_code=3,
                              refusal=dict(values=values))
                raise LoadRefusal(3)
            entered[0] = True
            try: yield dict(values, cpu_busy_percent=20)
            finally: entered[0] = False
        load = SimpleNamespace(admission=admission, LoadRefusal=LoadRefusal)
        record = {}
        def sleep(seconds):
            self.assertFalse(entered[0])
            clock[0] += seconds
        with patch.object(spread.time, 'monotonic', side_effect=lambda:clock[0]), \
             patch.object(spread.time, 'sleep', side_effect=sleep), \
             spread.named_engine_cpu_wait(load, dict(name='NAMED'), SimpleNamespace(peer_id='host', alone=False),
                                          lambda:renewed.append('owned')):
            with load.admission(['engine'], {}, record, lambda:None):
                self.assertTrue(entered[0])
                self.assertNotIn('not_started', record)
                self.assertEqual(record['admission_retries'][0]['values']['cpu_busy_percent'], 100)
        self.assertEqual(calls, [0, 30])
        self.assertEqual(renewed, ['owned'])
        self.assertIs(load.admission, admission)
        calls.clear()
        values['free_gb'] = 11
        with patch.object(spread.time, 'sleep') as pause, \
             spread.named_engine_cpu_wait(load, dict(name='NAMED'), SimpleNamespace(peer_id='host', alone=False), lambda:None):
            with self.assertRaises(LoadRefusal):
                with load.admission(['engine'], {}, {}, lambda:None):
                    self.fail('memory floor must refuse before launch')
            pause.assert_not_called()

    def test_native_worker_extension_refuses_an_unrecognized_guard(self):
        with self.assertRaisesRegex(spread.SpreadRefusal, 'guard differs'):
            spread.native_cpu_wait_source('def run_request(request):\n    start_without_guard(request)\n')

    def test_native_claim_keeps_the_runners_fresh_root_and_live_ownership(self):
        import cross_peers as cross
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary)/'host'
            marker = out/'.spread-run-owner.json'
            events = []
            def factory():
                out.mkdir(exist_ok=False)
                events.append('staged')
                return SimpleNamespace(close=lambda:events.append('closed'))
            @contextlib.contextmanager
            def claim(box, peer):
                self.assertTrue(out.is_dir())
                marker.write_text('owned')
                events.append('claimed')
                try: yield
                finally:
                    marker.unlink()
                    events.append('released')
            with patch.object(cross, 'peer_run_scope', side_effect=claim), contextlib.ExitStack() as ownership:
                run = spread.stage_native_run(factory, ownership, {}, {})
                self.assertTrue(marker.is_file())
                self.assertEqual(events, ['staged','claimed'])
            self.assertEqual(events, ['staged','claimed','released'])
            self.assertFalse(marker.exists())

    def test_post_shipment_fifo_wait_uses_the_same_box_and_claim(self):
        states = [{'reason':'earlier work request is waiting: menus'},
                  {'reason':'box launch refused; owner=menus (pid=7); since now'},
                  {'reason':None}]
        seen, renewed = [], []
        def capacity(box, **kwargs):
            seen.append((box, kwargs))
            return states.pop(0)
        worker = SimpleNamespace(capacity_state=capacity, renew_claim=lambda claim:renewed.append(claim))
        pool = SimpleNamespace(Needs=lambda **values:SimpleNamespace(**values), live_reason=lambda box, needs, state:state['reason'])
        box = dict(name='RecorderBox', kind='local')
        claim = dict(token='owned', needs=dict(peer_id='host'))
        with patch.object(spread.time, 'sleep'):
            spread.wait_named_launch(worker, pool, box, claim, wait=1800)
        self.assertEqual([row[0] for row in seen], [box, box, box])
        self.assertTrue(all(row[1]['ignore_token']=='owned' and row[1]['read_only'] for row in seen))
        self.assertEqual(renewed, [claim, claim])

    def test_post_shipment_wait_preserves_real_refusals(self):
        for reason in ('free memory 11 GB is below floor 12 GB',
                       'box launch refused; owner=unrecorded owner; since now'):
            worker = SimpleNamespace(capacity_state=unittest.mock.Mock(return_value={'reason':reason}),
                                     renew_claim=unittest.mock.Mock())
            pool = SimpleNamespace(Needs=lambda **values:SimpleNamespace(**values),
                                   live_reason=lambda box, needs, state:state['reason'])
            with self.subTest(reason=reason), patch.object(spread.time, 'sleep') as pause:
                with self.assertRaisesRegex(spread.SpreadRefusal, 'capacity changed before launch'):
                    spread.wait_named_launch(worker, pool, dict(name='RecorderBox', kind='local'),
                                             dict(token='owned', needs=dict(peer_id='host')),
                                             wait=1800, wait_for_holder='restore')
                pause.assert_not_called()
                worker.renew_claim.assert_not_called()

    def staging_case(self, root, token='owned'):
        case = object.__new__(spread.Case)
        case.out, case.id, case.closed = root.resolve(), 'owned', False
        root.mkdir()
        (root/'.spread-case-owner.json').write_text(json.dumps(dict(token=token, case_id=token)))
        return case

    def test_video_stages_in_its_own_live_controller_root(self):
        import e2e_video as video
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)/'run0'
            case = self.staging_case(root)
            video.prepare_run_root(root, case)
            self.assertEqual(json.loads((root/'.spread-case-owner.json').read_text())['token'], 'owned')

    def test_video_refuses_a_replaced_or_closed_controller_claim(self):
        import e2e_video as video
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)/'run0'
            case = self.staging_case(root, token='foreign')
            with self.assertRaisesRegex(spread.SpreadRefusal, 'RUN ROOT CONFLICT'):
                video.prepare_run_root(root, case)
            self.assertEqual(json.loads((root/'.spread-case-owner.json').read_text())['token'], 'foreign')
            case.closed = True
            with self.assertRaisesRegex(spread.SpreadRefusal, 'RUN ROOT CONFLICT'):
                video.prepare_run_root(root, case)

    def test_single_box_video_still_requires_a_fresh_run_root(self):
        import e2e_video as video
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)/'run0'
            video.prepare_run_root(root)
            with self.assertRaises(FileExistsError):
                video.prepare_run_root(root)

    def test_named_fifo_wait_covers_its_predecessor_window_on_the_same_box(self):
        box = dict(name='RecorderBox', kind='local')
        before = RuntimeError('capacity refused: earlier work request is waiting: restore')
        held = RuntimeError('capacity refused: box launch refused: own-marker; owner=restore (pid=7, machine=RecorderBox); since now')
        backend = SimpleNamespace(rpc=unittest.mock.Mock(side_effect=[before, held, dict(token='owned')]))
        with patch.object(spread.time, 'sleep'):
            claim = spread.claim_named_peer(backend, box, SimpleNamespace(peer_id='host'), {}, wait=1800)
        self.assertEqual(claim, dict(token='owned'))
        self.assertEqual([call.args[0] for call in backend.rpc.call_args_list], [box, box, box])

    def test_named_wait_never_retries_a_floor_or_ownerless_marker_refusal(self):
        for reason in ('capacity refused: free memory 11 GB is below floor 12 GB',
                       'capacity refused: box launch refused: other-marker; owner=unrecorded owner; since now'):
            backend = SimpleNamespace(rpc=unittest.mock.Mock(side_effect=RuntimeError(reason)))
            with self.subTest(reason=reason), patch.object(spread.time, 'sleep') as pause:
                with self.assertRaisesRegex(RuntimeError, 'capacity refused'):
                    spread.claim_named_peer(backend, dict(name='RecorderBox', kind='local'),
                                            SimpleNamespace(peer_id='host'), {}, wait=1800, wait_for_holder='restore')
                pause.assert_not_called()
                self.assertEqual(backend.rpc.call_count, 1)




    def test_explicit_task_keeps_only_the_registered_named_payload(self):
        slots = [dict(slot=1, task='session-one'), dict(slot=2, task='session-two')]
        box = dict(name='HostBox', kind='windows-task', task_slots=slots, engines_max=4)
        selected = spread.named_task_box(box, {'host':'session-two'}, ['host','seat'], 'host')
        self.assertEqual(selected['task_slots'], [slots[1]])
        self.assertEqual(selected['engines_max'], box['engines_max'])
        self.assertEqual(box['task_slots'], slots)
        with self.assertRaisesRegex(spread.SpreadRefusal, 'not registered'):
            spread.named_task_box(box, {'host':'missing-task'}, ['host','seat'], 'host')

    def test_public_directory_has_no_fixture_service_or_tunnel(self):
        case = object.__new__(spread.Case)
        case.match = spread.Match(51580, parameters=dict(public_directory=True))
        case.names, case.members = ['host','seat'], {'host': ({'name':'HostBox'},)}
        case.stack = unittest.mock.Mock()
        case.connect_directory()
        self.assertEqual(case.network, 'ice')
        self.assertTrue(case.public_directory)
        self.assertIsNone(case.directory)
        case.stack.enter_context.assert_not_called()

    def test_public_code_comes_from_that_hosts_registration_log(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root/'stdout.log').write_text('[net-directory] registered session_id=own-code heartbeat_s=15\n', encoding='utf-8')
            case = object.__new__(spread.Case)
            case.public_directory, case.match, case.peer_ports = True, spread.Match(51580), {}
            case.names, case.runs = ['host','seat'], {'host': SimpleNamespace(out=root, output_name='host', finished=False)}
            backend = SimpleNamespace(rpc=unittest.mock.Mock(return_value={'text': (root/'stdout.log').read_text()}))
            box, claim = dict(name='HostBox'), dict(case_root='native-owned')
            case.members = {'host': (box, claim, {}, backend)}
            case.guard = unittest.mock.Mock()
            case.synchronize = unittest.mock.Mock()
            self.assertEqual(case.published_session('seat'), 'own-code')
            case.synchronize.assert_not_called()
            case.guard.assert_called_once()
            backend.rpc.assert_called_once_with(box, 'text', dict(path='native-owned/host/stdout.log'), timeout=15)
            script = b'settext TextJoinAddress 127.0.0.1\nsettext TextJoinAddress deliberately-invalid\n'
            self.assertEqual(spread.map_script(script, [], 'own-code'),
                             b'settext TextJoinAddress session:own-code\nsettext TextJoinAddress deliberately-invalid\n')

    def test_public_settings_key_is_accepted_by_the_directorys_actual_parser(self):
        from session_directory.session_directory import valid_install_key
        repo = Path(__file__).resolve().parents[1]
        host = spread.public_directory_settings(repo, 'a'*32, 'host')
        client = spread.public_directory_settings(repo, 'a'*32, 'client')
        self.assertTrue(valid_install_key(host['SessionDirectoryInstallKey']))
        self.assertTrue(valid_install_key(client['SessionDirectoryInstallKey']))
        self.assertNotEqual(host['SessionDirectoryInstallKey'], client['SessionDirectoryInstallKey'])
        self.assertEqual(host['SessionDirectoryCertSha256'], '')
        self.assertEqual((host['NetworkIceEnable'],host['NetworkHostGameListing']),('1','unlisted'))

    def test_native_load_exit_preserves_peer_box_and_exact_reason(self):
        class LoadExit(SystemExit):
            def __str__(self):
                return 'RecorderBox: not started: memory is below the floor'
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            spec = root/'peer-spec.json'
            spec.write_text(json.dumps(dict(role='host', box=dict(name='RecorderBox'))))
            with patch.object(spread, 'native_execute', side_effect=LoadExit(3)):
                with self.assertRaises(LoadExit):
                    spread.main(['--native', str(spec), '--out', str(root/'results')])
            receipt = json.loads((root/'results/peer-result.json').read_text())
            self.assertEqual((receipt['topology'], receipt['peer'], receipt['box']), ('spread', 'host', 'RecorderBox'))
            self.assertIn('RecorderBox: not started: memory is below the floor', receipt['error'])
            self.assertEqual(json.loads((root/'progress.json').read_text())['record'], receipt)

    def test_routed_wait_updates_one_existing_holder_and_keeps_its_claim(self):
        sent = []
        box = dict(name='RecorderBox', kind='local')
        claim = dict(root='/own/run', token='owned')
        argv = ['python', '/lead/box_hold.py', '--wait', '0', '--label', 'fixture', '--', 'python', 'peer.py']
        def rpc(box, action, body, **kwargs):
            sent.append(body)
        backend = SimpleNamespace(rpc=rpc)
        def launch(box, claim, request):
            backend.rpc(box, 'write', dict(path=claim['root']+'/request.json',
                        value=dict(claim=claim, argv=argv, environment={'CCCP_HEADLESS':'1'})))
        backend.launch = launch
        spread.launch_native(backend, box, claim, {}, 1800)
        self.assertEqual(sent[0]['value']['argv'], argv[:3]+['1800']+argv[4:])
        self.assertEqual(sent[0]['value']['environment'], {'CCCP_HEADLESS':'1'})
        self.assertEqual(sent[0]['value']['claim'], claim)
        self.assertIs(backend.rpc, rpc)
        self.assertEqual(argv[3], '0')

    def test_live_controller_result_root_refuses_without_overwriting_its_receipt(self):
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary)/'run'
            out.mkdir()
            marker = out/'.spread-case-owner.json'
            marker.write_text(json.dumps(dict(case_id='other-case', peer_id='host', token='foreign')))
            receipt = out/'spread-result.json'
            receipt.write_bytes(b'existing live receipt')
            transport = SimpleNamespace(worker=SimpleNamespace(facts=SimpleNamespace()))
            with patch.object(spread, 'installed_pool', return_value=(None, transport, Path(temporary)/'kit.py')):
                with self.assertRaisesRegex(spread.SpreadRefusal, 'host on RecorderBox.*RUN ROOT CONFLICT.*other-case'):
                    spread.Case(Path(temporary), out, [spread.Peer('host')], spread.Match(51580), peer_boxes='host=RecorderBox')
            self.assertEqual(receipt.read_bytes(), b'existing live receipt')
            self.assertEqual(json.loads(marker.read_text())['token'], 'foreign')

    def test_compatible_pending_release_never_reads_the_retired_queue(self):
        released = []
        case = object.__new__(spread.Case)
        case.pending = [(Path('host-ticket'), 'host'), (Path('seat-ticket'), 'seat')]
        case.pending_peers = dict(host='host', seat='seat')
        case.transport_module = SimpleNamespace(worker=SimpleNamespace(facts=SimpleNamespace(release_reservation=lambda *args:released.append(args))))
        case.release_pending('host')
        case.release_pending()
        self.assertEqual(released, [])
        self.assertEqual(case.pending, [(Path('host-ticket'), 'host'), (Path('seat-ticket'), 'seat')])

    def test_linux_host_with_loopback_hostname_uses_its_native_interface(self):
        with patch.object(spread.sys, 'platform', 'linux'), patch.object(spread.shutil, 'which', return_value=None), \
                patch.object(spread.socket, 'getaddrinfo', return_value=[(None, None, None, None, ('127.0.1.1', 0))]), \
                patch.object(spread.subprocess, 'run', side_effect=[SimpleNamespace(returncode=1, stdout=''),
                                                                  SimpleNamespace(returncode=0, stdout='192.168.50.128 127.0.0.1 ')]) as read:
            self.assertEqual(spread.native_address_probe(), '192.168.50.128')
        self.assertEqual(read.call_args.args[0], ['hostname', '-I'])

    def test_linux_route_source_wins_over_a_docker_bridge(self):
        with patch.object(spread.sys, 'platform', 'linux'), patch.object(spread.shutil, 'which', return_value=None), \
                patch.object(spread.socket, 'getaddrinfo', return_value=[(None, None, None, None, ('172.17.0.1', 0))]), \
                patch.object(spread.subprocess, 'run', return_value=SimpleNamespace(returncode=0, stdout='[{"prefsrc":"192.0.2.20"}]')) as read:
            self.assertEqual(spread.native_address_probe(), '192.0.2.20')
        self.assertEqual(read.call_args.args[0], ['ip', '-j', 'route', 'get', '192.0.2.1'])

    def place_fake(self, peers, pins=None):
        case = object.__new__(spread.Case)
        case.registry, case.out, case.control = Path('fake.json'), Path('fake'), Path('fake/control')
        case.peers, case.names, case.pins = peers, [peer.name for peer in peers], pins or {}
        case.id = 'fake-case'
        case.members, case.pending, case.match, case.lane, case.peer_ports = {}, [], spread.Match(51580), 'fake', {}
        case.stack = contextlib.ExitStack()
        self.addCleanup(case.stack.close)
        boxes = [dict(name=name, kind='local' if name == 'RecorderBox' else 'windows-task', os='windows',
                      hostname=name, engines_max=6) for name in ('RecorderBox', 'HostBox', 'PeerBox')]
        claims, requests = {}, []
        def rpc(box, action, value, **kwargs):
            self.assertEqual(action, 'claim')
            needs, request = SimpleNamespace(**value['needs']), value['request']
            name = box['name']
            busy = claims.get(name, [])
            if needs.alone and busy or any(item.alone for item in busy):
                raise RuntimeError('alone run needs an idle box')
            if len(busy) + needs.engines > box['engines_max']:
                raise RuntimeError('live engine capacity is in use')
            claims.setdefault(name, []).append(needs)
            requests.append(request)
            return dict(token=request['token'])
        case.backend = lambda: SimpleNamespace(rpc=rpc, control=lambda box:'fake/control',
                                               probe=lambda box,**kwargs:dict(hostname=box['hostname']))
        def retired(*args):
            raise AssertionError('retired placement was consulted')
        case.pool = SimpleNamespace(load_registry=lambda path:dict(boxes=boxes), static_reason=lambda *args:None,
                                    Needs=lambda **kwargs:SimpleNamespace(**(dict(alone=False, only_box=None, excluded=(), share_ok=False, reviewed=False, held=False) | kwargs)),
                                    queue_root=retired, write_ticket=retired, candidates=retired, priority_blocker=retired)
        facts = SimpleNamespace(machine_name=lambda:'controller', process_start=lambda pid:1, release_reservation=lambda *args:None)
        case.transport_module = SimpleNamespace(worker=SimpleNamespace(facts=facts, mutex=lambda *args,**kwargs:contextlib.nullcontext()))
        case.refuse = lambda name, box, reason:spread.SpreadRefusal(f'{name} on {box}: {reason}')
        case.allocate()
        self.assertTrue(all(request['case_id'] == case.id for request in requests))
        self.assertEqual({request['peer_id'] for request in requests}, set(case.names))
        return case, claims

    def test_shareable_screen_peers_use_one_fitting_box_without_reviewed_peer(self):
        case, claims = self.place_fake([spread.Peer('host', reviewed=True), spread.Peer('a'), spread.Peer('b'), spread.Peer('c')],
                                      dict(host='HostBox', a='RecorderBox', b='RecorderBox', c='PeerBox'))
        self.assertEqual([item[0]['name'] for item in case.members.values()], ['HostBox', 'RecorderBox', 'RecorderBox', 'PeerBox'])
        self.assertEqual(len(claims['RecorderBox']), 2)
        self.assertTrue(all(not needs.alone for needs in claims['RecorderBox']))
        self.assertTrue(all('enqueued_at' not in request for _, _, request, _ in case.members.values()))

    def test_only_the_lead_can_explicitly_share_all_eligible_peers(self):
        case, claims = self.place_fake([spread.Peer('a'), spread.Peer('b')], dict(a='RecorderBox', b='RecorderBox'))
        self.assertEqual({item[0]['name'] for item in case.members.values()}, {'RecorderBox'})
        self.assertEqual(len(claims['RecorderBox']), 2)

    def test_retained_runtime_keeps_settings_and_private_overlay_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            settings = root/'Userdata/Settings.ini'
            settings.parent.mkdir()
            settings.write_bytes(b'SettingsMan\n\tNetworkDisplayName = retained\n')
            overlay = root/'Data/Base.rte/GUIs/old.ini'
            overlay.parent.mkdir(parents=True)
            overlay.write_bytes(b'old GUI bytes\x00\xff')
            files = spread.retained_files(root)
            self.assertEqual(files[Path('Userdata/Settings.ini')], settings.read_bytes())
            self.assertEqual(files[Path('Data/Base.rte/GUIs/old.ini')], overlay.read_bytes())
            repo = root/'repo'
            (repo/'Data/Base.rte/GUIs').mkdir(parents=True)
            source = repo/'Data/Base.rte/GUIs/old.ini'
            source.write_bytes(b'current GUI')
            runtime = spread.native_retained_runtime(repo, root/'native/host', files, [])
            self.assertEqual((runtime/'Data/Base.rte/GUIs/old.ini').read_bytes(), overlay.read_bytes())
            self.assertEqual(source.read_bytes(), b'current GUI')

    def test_retained_incomplete_runtime_and_private_ticket_refuse(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with self.assertRaisesRegex(ValueError, 'retained runtime is incomplete'):
                spread.retained_files(root)
            (root/'Userdata').mkdir()
            (root/'Userdata/Settings.ini').write_text('SettingsMan\n')
            (root/'Userdata/host.ticket').write_bytes(b'private fixture')
            with self.assertRaisesRegex(spread.SpreadRefusal, 'existing credential delivery channel'):
                spread.retained_files(root)

    def exercise_private_module_contents(self, platform, link_error=None):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            repo, base = root/'repo', root/'repo/Data/Base.rte'
            (base/'Actors').mkdir(parents=True)
            (base/'Activities').mkdir()
            (base/'Actors/actor.bin').write_bytes(b'actor pixels')
            (base/'Activities/P4AlphaDuel.lua').write_bytes(b'unchanged duel')
            (base/'Index.ini').write_bytes(b'DataModule')
            other = repo/'Data/Other.rte'
            other.mkdir()
            (other/'Index.ini').write_bytes(b'Other')
            aliases = []
            def alias(source, target):
                aliases.append(target)
                target.mkdir()
            files = {Path('Data/Base.rte/Activities/P4AlphaDuel.lua'): b'observed duel',
                     Path('Userdata/Settings.ini'): b'owned settings'}
            with patch.object(spread.sys, 'platform', platform), patch.object(spread, 'link_directory', side_effect=alias):
                if link_error:
                    with patch.object(spread.os, 'link', side_effect=link_error):
                        runtime = spread.native_retained_runtime(repo, root/'native/seat', files, [])
                else:
                    runtime = spread.native_retained_runtime(repo, root/'native/seat', files, [])
            native_base = runtime/'Data/Base.rte'
            if platform != 'win32':
                self.assertFalse([path for path in aliases if path.is_relative_to(native_base)],
                                 'module content must have no directory links')
                self.assertEqual((native_base/'Actors/actor.bin').read_bytes(), b'actor pixels')
                self.assertEqual((native_base/'Index.ini').read_bytes(), b'DataModule')
                self.assertEqual({path.relative_to(native_base).as_posix() for path in native_base.rglob('*') if path.is_file()},
                                 {'Index.ini', 'Actors/actor.bin', 'Activities/P4AlphaDuel.lua'})
            else:
                self.assertIn(native_base/'Actors', aliases)
            self.assertIn(runtime/'Data/Other.rte', aliases)
            self.assertEqual((native_base/'Activities/P4AlphaDuel.lua').read_bytes(), b'observed duel')
            (native_base/'Activities/P4AlphaDuel.lua').write_bytes(b'private later edit')
            self.assertEqual((base/'Activities/P4AlphaDuel.lua').read_bytes(), b'unchanged duel',
                             'overlay never writes to immutable source')

    def test_posix_private_module_preserves_all_files_without_content_links(self):
        for platform in ('linux', 'darwin'):
            with self.subTest(platform=platform):
                self.exercise_private_module_contents(platform)

    def test_posix_private_module_cross_filesystem_fallback_preserves_contents(self):
        self.exercise_private_module_contents('linux', OSError(errno.EXDEV, 'different filesystem'))

    def test_windows_private_module_keeps_existing_directory_aliases(self):
        self.exercise_private_module_contents('win32')

    def test_real_menu_pairs_keep_every_original_page_assertion_and_capture(self):
        import test_menu_readback as menu
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for case in ('net-chat', 'lobby-name'):
                original, _ = menu.scripts(case, 51580, root, '1280x720')
                texts, probes = menu.spread_menu_scripts(case, original, 51580, root)
                self.assertEqual(original['host'].count('dump_'), texts['host'].count('dump_'))
                self.assertTrue(all(line in texts['host'] for line in original['host'].splitlines() if line.startswith('assert_')))
                self.assertIn('wait_connected 2 60', texts['client'])
                self.assertIn('wait_label LabelLobbyPlayer0 ', texts['client'])
                self.assertNotIn('dump_', texts['client'])
                self.assertEqual(probes['host']['steps'][0]['screen'], 'MultiplayerScreen')
                if case == 'net-chat':
                    self.assertIn('hello-from-client', texts['host'])
                    self.assertIn('hello-from-host', texts['client'])
                    self.assertIn('host-received-client', texts['client'])

    def test_named_claim_matches_identity_without_excluding_an_existing_binary(self):
        import cross_peers as cross
        box = dict(name='RecorderBox', kind='windows-local', executable='D:/own/game.exe', scratch='D:/own')
        peer = dict(case_id='case', peer_id='seat')
        claim = dict(case_id='case', peer_id='seat', share_ok=True, engines=1, token='owned')
        assignment = dict(claim='owned-claim', token='owned', box=box, executable=box['executable'])
        module = SimpleNamespace(assignment_for_launch=lambda:assignment,
                                 box_facts=SimpleNamespace(read_reservation=lambda path, **kwargs:claim))
        with patch.dict(sys.modules, pool_run=module, box_load=SimpleNamespace(memory=lambda:(20,48))), \
                patch.object(cross,'inventory_guard',return_value=None), \
                patch.object(cross,'box_load',return_value=[dict(Name='Cortex Command',ExecutablePath=box['executable'])]), \
                patch.object(cross,'scratch_bytes',return_value=0):
            cross.assert_box_guard(box)
            cross.assert_box_guard(box,pool_peer=peer)
            for change in (dict(share_ok=False),dict(reviewed=True),dict(held=True),dict(alone=True)):
                before = dict(claim)
                claim.update(change)
                cross.assert_box_guard(box,pool_peer=peer)
                claim.clear(); claim.update(before)
            for change in (dict(case_id='other'),dict(peer_id='other'),dict(token='foreign')):
                before = dict(claim)
                claim.update(change)
                with self.assertRaisesRegex(RuntimeError,'native capacity owner or executable differs'):
                    cross.assert_box_guard(box,pool_peer=peer)
                claim.clear(); claim.update(before)

    def test_unshareable_and_quiet_peers_reserve_distinct_idle_boxes(self):
        case, claims = self.place_fake([spread.Peer('host', share_ok=False), spread.Peer('seat', quiet=True)],
                                      dict(host='RecorderBox', seat='HostBox'))
        self.assertEqual(len({item[0]['name'] for item in case.members.values()}), 2)
        self.assertTrue(claims[case.members['seat'][0]['name']][0].alone)
        self.assertTrue(all(not needs.share_ok for rows in claims.values() for needs in rows))
        self.assertFalse(case.peers[1].share_ok)

    def test_reviewed_and_held_peers_cannot_share_even_when_requested(self):
        peers = [spread.Peer('host', reviewed=True, share_ok=True), spread.Peer('held', held=True, share_ok=True)]
        self.assertTrue(all(not peer.share_ok for peer in peers))
        with self.assertRaisesRegex(spread.SpreadRefusal, 'TWO PEERS ON ONE BOX WITHOUT share_ok'):
            self.place_fake(peers, dict(host='RecorderBox', held='RecorderBox'))

    def test_reviewed_readback_uses_the_named_box_without_choosing(self):
        case, _ = self.place_fake([spread.Peer('host', reviewed=True, readback=True), spread.Peer('a'), spread.Peer('b')],
                                  dict(host='HostBox', a='RecorderBox', b='RecorderBox'))
        self.assertEqual(case.members['host'][0]['name'], 'HostBox')
        self.assertEqual(case.members['a'][0]['name'], 'RecorderBox')
        self.assertEqual(case.members['b'][0]['name'], 'RecorderBox')

    def test_worked_example_retains_wrong_peer_port_after_video_parses(self):
        import argparse
        import spread_example as example
        received = []
        def video_main():
            parser = argparse.ArgumentParser()
            spread.add_arguments(parser)
            options, _ = parser.parse_known_args()
            spread.configure(options)
            received.extend(options.peer_port)
            return 2
        with tempfile.TemporaryDirectory() as directory, patch.object(sys, 'argv', ['spread_example.py', '--out', directory,
                '--peer-boxes', 'host=RecorderBox,seat2=FirstPosixBox', '--pool-dispatcher', 'dispatcher.py', '--port', '51580', '--port-block', '51580-51589',
                '--peer-port', 'seat2=51581']), patch.object(example.video, 'main', video_main), patch.object(example.video, 'run_one'):
            self.assertEqual(example.main(), 2)
        self.assertEqual(received, ['seat2=51581'])
        spread.configure(None)

    def test_worked_example_uses_one_shared_case_and_the_original_staging(self):
        import spread_example as example
        case = SimpleNamespace(result=lambda:dict(peer_boxes=dict(host='RecorderBox', client='FirstPosixBox')))
        calls = []
        def run_case(repo, out, peers, match, **kwargs):
            calls.append(kwargs['peer_boxes'])
            self.assertEqual([peer.os for peer in peers], ['windows', 'any'])
            return dict(driver_result=kwargs['drive'](case))
        def video_main():
            value = example.video.run_one(SimpleNamespace(repo=Path('.'), size='960x540', port=51580),
                                          dict(size='960x540', timeout_s=170), dict(name='run0'), 0, Path('.'))
            self.assertEqual(value['peer_boxes'], dict(host='RecorderBox', client='FirstPosixBox'))
            self.assertTrue(all(peer['record']['topology']=='spread' and peer['record']['box']==peer['box'] for peer in value['peers']))
            return 0
        with tempfile.TemporaryDirectory() as directory, patch.object(sys, 'argv', ['spread_example.py', '--out', directory,
                '--peer-boxes', 'host=RecorderBox,seat2=FirstPosixBox', '--port', '51580', '--port-block', '51580-51589']), \
                patch.object(example, 'run_case', side_effect=run_case), patch.object(example.video, 'main', video_main), \
                patch.object(example.video, '_run_one', return_value=dict(peers=[dict(peer='host',record={}),dict(peer='client',record={})])) as staged, \
                patch.object(example.video, 'run_one'):
            self.assertEqual(example.main(), 0)
            staged.assert_called_once()
        self.assertEqual(calls, ['host=RecorderBox,seat2=FirstPosixBox'])
        spread.configure(None)

    def test_pool_control_bootstrap_needs_no_caller_repository_imports(self):
        script = "import pathlib,sys,types; p=pathlib.Path(sys.argv[1]); m=types.ModuleType('spread_peers'); m.__file__=str(p); sys.modules[m.__name__]=m; exec(compile(p.read_text(encoding='utf-8'),str(p),'exec'),m.__dict__)"
        done = subprocess.run([sys.executable, '-I', '-c', script, str(Path(spread.__file__).resolve())], capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr)

    def test_peer_cannot_hide_two_engines_on_one_machine(self):
        with self.assertRaisesRegex(ValueError, "exactly one"):
            spread.Peer("host", engines=2)

    def test_safe_peer_name(self):
        with self.assertRaises(ValueError):
            spread.Peer("../owner")

    def test_declared_unicode_output_alias_preserves_the_logical_peer(self):
        with tempfile.TemporaryDirectory() as temporary:
            case = object.__new__(spread.Case)
            case.out = Path(temporary).resolve()
            alias = '\u00e9' * 64
            peer = spread.Peer('host', output_name=alias)
            case.output_names, case.members, case.runs = {peer.name: peer.output_name}, {'host': ()}, {}
            with patch.object(spread, 'Run') as factory:
                case.make_run(case.out, [], case.out / alias)
            self.assertEqual(factory.call_args.kwargs['role'], 'host')

    def test_output_alias_cannot_escape_the_case(self):
        with self.assertRaises(ValueError):
            spread.Peer('host', output_name='../owner')

    def test_legacy_script_bytes_are_not_transcoded(self):
        data = b'mark ' + bytes(range(128, 256)) + b'\nwait_file X:\\test-inputs\\lane\\ready.json 5000\n'
        mapped = spread.map_script(data, [('X:\\test-inputs\\lane', '/native/lane')])
        self.assertEqual(mapped, data.replace(b'X:\\test-inputs\\lane\\ready.json', b'/native/lane/ready.json'))

    def test_control_port_is_not_game_port(self):
        with self.assertRaises(ValueError):
            spread.Match(46630, 46630)

    def test_box_aliases_preserve_actual_peer_names(self):
        values = spread.pairs("host=ONE,seat2=TWO,Dee=THREE")
        self.assertEqual(spread.role_value(values, ["Host", "Ana", "Dee"], "Ana"), "TWO")
        self.assertEqual(spread.role_value(values, ["Host", "Ana", "Dee"], "Dee"), "THREE")

    def test_case_alias_cannot_overwrite_box_assignment(self):
        with self.assertRaises(ValueError):
            spread.pairs("host=ONE,HOST=TWO")

    def test_windows_paths_inside_probe_json_map_to_native_root(self):
        text = json.dumps({"path": "X:\\test-inputs\\lane\\host_probe\\done.json"})
        mapped = spread.map_text(text, [("X:\\test-inputs\\lane", "/native/lane")])
        self.assertEqual(json.loads(mapped)["path"], "/native/lane/host_probe/done.json")

    def test_script_and_argument_paths_map_the_whole_windows_tail(self):
        mappings = [("X:\\test-inputs\\lane", "/native/lane")]
        self.assertEqual(spread.map_text("X:\\test-inputs\\lane\\host\\feel", mappings), "/native/lane/host/feel")
        self.assertEqual(spread.map_text("wait_file X:\\test-inputs\\lane\\ready.json 5000\n", mappings),
                         "wait_file /native/lane/ready.json 5000\n")

    def test_probe_path_mapping_preserves_unrelated_regex_escapes(self):
        text = json.dumps({"path": "X:\\test-inputs\\lane\\done.json", "regex": r"\d+\s+"})
        self.assertEqual(json.loads(spread.map_text(text, [("X:\\test-inputs\\lane", "/native/lane")]))["regex"], r"\d+\s+")

    def test_mapping_prefers_case_input_over_case_root(self):
        result = spread.map_text("X:/test-inputs/lane/input.txt", [("X:/test-inputs/lane", "/root"), ("X:/test-inputs/lane/input.txt", "/input/schedule.txt")])
        self.assertEqual(result, "/input/schedule.txt")

    def test_signals_include_probe_and_controller_rendezvous(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            directory = root/"Host_probe"
            directory.mkdir()
            (directory/"probe.json").write_text(json.dumps({"steps": [{"op": "signal", "name": "request-seen"},
                                                                          {"op": "wait_file", "path": str(root/"newcomer-frozen.json")}] }))
            signals = spread.declared_signals(root)
            self.assertIn("Host_probe/request-seen.json", signals)
            self.assertIn("newcomer-frozen.json", signals)
            self.assertIn("Host_probe/done.json", signals)

    def test_publish_rejects_undeclared_or_escaping_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            data = base64.b64encode(b"{}").decode()
            with self.assertRaisesRegex(ValueError, "declared"):
                spread.publish_signals(Path(temporary), {"owner.json": data}, {"done.json"})
            with self.assertRaises(ValueError):
                spread.publish_signals(Path(temporary), {"../owner.json": data}, {"../owner.json"})

    def test_partial_signal_is_not_published(self):
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(ValueError):
                spread.publish_signals(Path(temporary), {"done.json": base64.b64encode(b"{").decode()}, {"done.json"})
            self.assertFalse((Path(temporary)/"done.json").exists())

    def test_declared_empty_presence_marker_keeps_its_exact_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            spread.publish_signals(Path(temporary), {"ready.json": ""}, {"ready.json"})
            self.assertEqual((Path(temporary)/"ready.json").read_bytes(), b"")

    def test_presence_markers_are_declared_and_keep_opaque_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'menu.txt').write_text('touch_file client_done.mark\nwait_file client_done.mark 30\n')
            self.assertIn('client_done.mark', spread.declared_signals(root))
            data = b'ready\n'
            spread.publish_signals(root, {'client_done.mark': base64.b64encode(data).decode()}, {'client_done.mark'})
            self.assertEqual((root / 'client_done.mark').read_bytes(), data)

    def test_explicit_direct_mode_retains_its_host_address_without_directory(self):
        case = object.__new__(spread.Case)
        case.names = ['host']
        case.match = spread.Match(51580, parameters={'network': 'direct', 'host_address': '100.64.1.2'})
        case.members = {'host': ({'name': 'ONE'}, {}, {}, None)}
        case.connect_directory()
        self.assertIsNone(case.directory)
        self.assertEqual(case.host_address, '100.64.1.2')

    def test_direct_mode_rejects_a_loopback_host(self):
        case = object.__new__(spread.Case)
        case.names = ['host']
        case.match = spread.Match(51580, parameters={'network': 'direct', 'host_address': '127.0.0.1'})
        case.members = {'host': ({'name': 'ONE'}, {}, {}, None)}
        case.refuse = lambda name, box, reason: spread.SpreadRefusal(f'{name} on {box}: {reason}')
        with self.assertRaisesRegex(spread.SpreadRefusal, 'real machine'):
            case.connect_directory()

    def test_direct_mode_maps_only_loopback_inputs_and_preserves_ice_off(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root/'client-stage').mkdir()
            menu = root/'client-stage/menu.txt'
            menu.write_bytes(b'settext TextJoinAddress 127.0.0.1\nsettext TextJoinAddress invalid-seat\n')
            specs = []
            backend = SimpleNamespace(rpc=lambda box, action, body: specs.append(body['value']), launch=lambda *args: None)
            claim = dict(case_root='/native/case', repo='/native/repo', root='/owned', control='/control', exe_sha256='same')
            case = SimpleNamespace(id='fake-case', lane='one-lane', out=root, peer_ports={}, names=['host', 'client'], match=spread.Match(51580),
                                   directory=None, network='direct', host_address='100.64.1.2',
                                   members={'client': ({'name': 'SEAT', 'os': 'windows'}, claim, {}, backend)},
                                   peers=[spread.Peer('client')], signals=lambda: [], release_pending=lambda *args: None,
                                   transport_module=SimpleNamespace(Transport=SimpleNamespace(native_claim=lambda value: value)))
            run = object.__new__(spread.Run)
            run.retained = None
            run.case, run.role, run.output_name = case, 'client', 'client'
            run.repo, run.cwd, run.started = root/'repo', root/'client/runtime', False
            run.argv = ['engine', '-headless', '-net-ice', 'off', '-net-join', 'localhost', '-net-port', '51580']
            run.env, run.expected, run.fixtures, run.timeout = {}, [], [], 30
            with patch.object(spread, 'wait_native_admission'):
                run.start()
            spec = specs[0]
            self.assertEqual(spec['args'], ['-net-ice', 'off', '-net-join', '100.64.1.2', '-net-port', '51580'])
            self.assertEqual(base64.b64decode(spec['files']['client-stage/menu.txt']),
                             b'settext TextJoinAddress 100.64.1.2\nsettext TextJoinAddress invalid-seat\n')

    def test_blocked_port_is_a_declared_test_port(self):
        with self.assertRaisesRegex(ValueError, 'declared test ports'):
            spread.Peer('seat', block_udp=(80,))

    def test_declared_binary_match_config_keeps_every_byte(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = root / 'launch-config.bin'
            data = bytes(range(256)) + b'\x00\xff\n\r\x80'
            config.write_bytes(data)
            specs = []
            backend = SimpleNamespace(rpc=lambda box, action, body:specs.append(body['value']), launch=lambda *args:None)
            claim = dict(case_root='/native/case', repo='/native/repo', root='/owned', control='/control', exe_sha256='same')
            run = object.__new__(spread.Run)
            run.retained = None
            run.role, run.output_name, run.started = 'host', 'host', False
            run.repo, run.cwd = root/'repo', root/'host/runtime'
            run.argv = ['engine', '-headless', '-net-match-service-config', str(config)]
            run.env, run.expected, run.fixtures, run.timeout = {}, [], [], 30
            run.case = SimpleNamespace(id='fake-case', lane='one-lane', out=root, members={'host':({'name':'HOST'},claim,{},backend)}, names=['host'], peer_ports={},
                match=spread.Match(51580), directory=None, peers=[spread.Peer('host')], signals=lambda:[], release_pending=lambda *args:None,
                transport_module=SimpleNamespace(Transport=SimpleNamespace(native_claim=lambda value:value)))
            with patch.object(spread, 'wait_native_admission'):
                run.start()
            self.assertEqual(base64.b64decode(specs[0]['files']['launch-config.bin']), data)
            self.assertEqual(specs[0]['args'], ['-net-match-service-config', '/native/case/launch-config.bin'])

    def test_native_submit_refusal_names_the_peer_and_box_before_start(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            def launch(*args):
                raise RuntimeError('native memory 4.4 GB is below floor 4.5 GB')
            backend = SimpleNamespace(rpc=lambda *args:None, launch=launch)
            claim = dict(case_root='/native', repo='/repo', root='/owned', control='/control', exe_sha256='same')
            run = object.__new__(spread.Run)
            run.retained = None
            run.role, run.output_name, run.started, run.argv = 'seat', 'seat', False, ['engine','-headless']
            run.cwd, run.repo, run.env, run.expected, run.fixtures, run.timeout = root/'seat/runtime', root/'repo', {}, [], [], 30
            run.case = SimpleNamespace(id='fake-case', lane='one-lane', members={'seat':({'name':'REMOTE'},claim,{},backend)}, names=['seat'], peer_ports={},
                                       match=spread.Match(51580), directory=None, out=root, peers=[spread.Peer('seat')], signals=lambda:[],
                                       transport_module=SimpleNamespace(Transport=SimpleNamespace(native_claim=lambda value:value)),
                                       refuse=lambda peer,box,reason:spread.SpreadRefusal(f'spread peer {peer} on {box}: {reason}'))
            with self.assertRaisesRegex(spread.SpreadRefusal, 'spread peer seat on REMOTE: native memory 4.4 GB is below floor 4.5 GB'):
                run.start()
            self.assertFalse(run.started)
            self.assertTrue(run.launch_attempted)

    def test_prelaunch_capacity_refusal_is_named_and_recorded_before_start(self):
        reason = 'capacity changed before launch: CPU 100.0% exceeds 90% over the last 10 seconds'
        with tempfile.TemporaryDirectory() as temporary:
            root, refusals = Path(temporary), []
            backend = SimpleNamespace(rpc=lambda *args:None, launch=unittest.mock.Mock())
            box = dict(name='RecorderBox', kind='local')
            claim = dict(case_root='/native', repo='/repo', root='/owned', control='/control', exe_sha256='same')
            def refuse(peer, box, text):
                refusals.append(dict(peer=peer, box=box, reason=text))
                return spread.SpreadRefusal(f'spread peer {peer} on {box}: {text}')
            run = object.__new__(spread.Run)
            run.retained = None
            run.role, run.output_name, run.started, run.argv = 'host', 'host', False, ['engine','-headless']
            run.cwd, run.repo, run.env, run.expected, run.fixtures, run.timeout = root/'host/runtime', root/'repo', {}, [], [], 30
            run.case = SimpleNamespace(id='fake-case', lane='one-lane', members={'host':(box,claim,{},backend)},
                names=['host'], peer_ports={}, match=spread.Match(51580, parameters=dict(runner_wait=1800)),
                directory=None, out=root, peers=[spread.Peer('host')], signals=lambda:[], pool=None,
                transport_module=SimpleNamespace(worker=None, Transport=SimpleNamespace(native_claim=lambda value:value)), refuse=refuse)
            with patch.object(spread, 'wait_named_launch', side_effect=spread.SpreadRefusal(reason)):
                with self.assertRaisesRegex(spread.SpreadRefusal, 'spread peer host on RecorderBox: capacity changed'):
                    run.start()
            self.assertEqual(refusals, [dict(peer='host', box='RecorderBox', reason=reason)])
            self.assertFalse(run.started)
            backend.launch.assert_not_called()

    def test_native_holder_refusal_preserves_its_reason_without_other_log_bytes(self):
        log = 'private-value\n[box-hold] waiting: earlier owned job settles\n[box-hold] REFUSED: no turn within the wait\n'
        reason = spread.native_refusal(dict(reason='native launch refused', exit_code=3), log)
        self.assertEqual(reason, '[box-hold] waiting: earlier owned job settles\n[box-hold] REFUSED: no turn within the wait')
        self.assertNotIn('private-value', reason)

    def test_caller_directory_forwards_the_clients_own_proxy_without_replacing_it(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            descriptor = dict(DIRECTORY_URL='127.0.0.1:51581', DIRECTORY_PIN='a'*64, DIRECTORY_ROOT=str(root))
            case = object.__new__(spread.Case)
            case.match = spread.Match(51580, parameters=dict(directory=descriptor, peer_directory_urls={'client':'127.0.0.1:51582'}, join_by_session=False))
            case.names, case.peers, case.control, case.tunnels = ['host', 'client'], [spread.Peer('host'), spread.Peer('client')], root, []
            case.members = {'host': ({'name':'HOST', 'kind':'local'}, {'directory_port':51585}, {}, None),
                            'client': ({'name':'SEAT', 'kind':'windows-task', 'ssh':'native-seat'}, {'directory_port':51586}, {}, None)}
            with patch.object(spread.subprocess, 'Popen', return_value=SimpleNamespace(poll=lambda: None)) as popen, patch.object(spread.time, 'sleep'):
                case.connect_directory()
            for process, log in case.tunnels:
                log.close()
            self.assertEqual(case.members['host'][1]['signal_port'], 51581)
            self.assertEqual(case.members['client'][1]['signal_port'], 51586)
            self.assertIn('127.0.0.1:51586:127.0.0.1:51582', popen.call_args.args[0])
            self.assertEqual(case.directory['DIRECTORY_PIN'], descriptor['DIRECTORY_PIN'])
            self.assertEqual(descriptor, dict(DIRECTORY_URL='127.0.0.1:51581', DIRECTORY_PIN='a'*64, DIRECTORY_ROOT=str(root)))
            self.assertTrue(case.directory['preserve_settings'])

    def test_caller_directory_rejects_credentials_or_a_nonlocal_service(self):
        for endpoint in ['https://user:password@localhost:51581', 'https://remote.example:51581', 'http://localhost:51581']:
            with self.assertRaisesRegex(ValueError, 'loopback TLS test endpoint'):
                spread.directory_endpoint(endpoint)

    def test_named_native_refusal_does_not_probe_or_choose_another_box(self):
        events = []
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            facts = SimpleNamespace(machine_name=lambda:'controller', process_start=lambda pid:1,
                                    release_reservation=lambda path, token:events.append(('release',path,token)))
            case = object.__new__(spread.Case)
            case.registry, case.names, case.members, case.pending, case.control, case.out = root/'catalog.json', ['host'], {}, [], root, root
            case.peers, case.pins, case.match, case.lane, case.peer_ports = [spread.Peer('host', reviewed=True, quiet=True, lane='in-match')], dict(host='ONE'), spread.Match(51580), 'case', {}
            case.id = 'fake-case'
            case.stack = contextlib.ExitStack()
            def probe(box, **kwargs):
                events.append(('probe', box['name']))
                return {}
            def claim(box, action, value, **kwargs):
                events.append(('claim', box['name'], value['request']['label']))
                raise RuntimeError('alone run needs an idle box')
            case.pool = SimpleNamespace(load_registry=lambda path:{'boxes':[{'name':'ONE','kind':'local','os':'windows'},
                                                                           {'name':'FREE','kind':'local','os':'windows'}]},
                                        Needs=lambda **kwargs:SimpleNamespace(**kwargs), static_reason=lambda *args:None)
            case.transport_module = SimpleNamespace(worker=SimpleNamespace(facts=facts, mutex=lambda *args,**kwargs:contextlib.nullcontext()))
            case.backend = lambda:SimpleNamespace(probe=probe,rpc=claim)
            case.refuse = lambda peer, box, reason:spread.SpreadRefusal(f'{peer} on {box}: {reason}')
            clock = [0.0]
            with patch.object(spread.time, 'monotonic', side_effect=lambda:clock[0]), \
                 patch.object(spread.time, 'sleep', side_effect=lambda seconds:clock.__setitem__(0,clock[0]+seconds)), \
                 self.assertRaisesRegex(spread.SpreadRefusal, 'expired after 600s.*alone run needs an idle box'):
                case.allocate()
            case.stack.close()
            self.assertEqual(events, [('probe', 'ONE')]+[('claim', 'ONE', 'in-match: spread')]*21)
            self.assertEqual(case.pending, [])

    def test_credentials_and_tickets_do_not_travel_as_evidence(self):
        for name in (".env", "key.pem", "host.ticket", "id_ed25519"):
            self.assertFalse(spread.public_file(Path(name)))

    def test_default_proof_mark(self):
        self.assertEqual(spread.topology(count=4), "single-box: not proof")

    def test_declared_game_port_block_bounds_all_case_ports(self):
        self.assertEqual(spread.check_port_block(51580, '51580-51589', 10), (51580, 51589))
        for port, block, count in ((51580, '51580-51589', 11), (51579, '51580-51589', 1), (80, '80-89', 1),
                                   (65535, '65535-65536', 1), (51580, '51580:51589', 1)):
            with self.assertRaises(ValueError):
                spread.check_port_block(port, block, count)

    def cohort(self, client_hash="same"):
        class Transport:
            def __init__(self, digest):
                self.snapshot = None
                self.digest = digest
            def prepare(self, box, claim, request):
                if self.snapshot is None:
                    self.snapshot = {"head": "frozen"}
                claim.update(exe_sha256=self.digest, head=self.snapshot["head"])
            def committed_inputs(self):
                self.snapshot = {"head": "frozen"}
                return self.snapshot
        case = object.__new__(spread.Case)
        case.names = ["host", "client"]
        case.lane = "example"
        case.id = "unique"
        case.match = spread.Match(51580)
        case.guard = lambda: None
        case.members = {name: ({"name": name, "os": "windows", "scratch": "X:/test-inputs"}, {"ports": [47660, 47664]}, {}, Transport(digest))
                        for name, digest in (("host", "same"), ("client", client_hash))}
        return case

    def test_every_peer_uses_one_frozen_snapshot(self):
        case = self.cohort()
        case.prepare()
        self.assertIs(case.members["host"][3].snapshot, case.members["client"][3].snapshot)

    def test_changed_binary_between_shipments_is_refused(self):
        with self.assertRaisesRegex(spread.SpreadRefusal, "between peer shipments"):
            self.cohort("changed").prepare()

    def lever_handle(self, os_name="windows"):
        class Transport:
            def rpc(self, box, action, body):
                self.target = box["name"]
                self.body = body
        backend = Transport()
        run = object.__new__(spread.Run)
        run.role = "seat2"
        run.started, run.finished = True, False
        run.native_progress = {"pid": 77}
        def synchronize(**kwargs):
            run.native_progress["action"] = {**backend.body["value"], "box": backend.target, "pid": 77}
        def refuse(peer, box, reason):
            return spread.SpreadRefusal(f"{peer} on {box}: {reason}")
        run.case = SimpleNamespace(peers=[spread.Peer("seat2", held=True)], members={"seat2": ({"name": "REMOTE", "os": os_name}, {"root": "/owned"}, {}, backend)},
                                   synchronize=synchronize, refuse=refuse)
        return run, backend

    def test_hold_lever_targets_the_held_peers_native_box(self):
        run, backend = self.lever_handle()
        receipt = run.suspend()
        self.assertEqual(backend.target, "REMOTE")
        self.assertEqual(backend.body["path"], "/owned/action.json")
        self.assertEqual((receipt["action"], receipt["box"], receipt["pid"]), ("suspend", "REMOTE", 77))

    def test_windows_hold_is_not_silently_replaced_on_posix(self):
        run, backend = self.lever_handle("linux")
        with self.assertRaisesRegex(spread.SpreadRefusal, "seat2 on REMOTE: Windows process suspension is unavailable"):
            run.suspend()
        self.assertFalse(hasattr(backend, "target"))


if __name__ == "__main__":
    unittest.main()
