import copy
import json
from pathlib import Path
import tempfile
import unittest

import cross_peers
import cross_report
from compare_sim_traces import CORE


class HostLossEvidence(unittest.TestCase):
    def test_recorded_endpoint_allows_true_host_kill_plan(self):
        with tempfile.TemporaryDirectory() as folder:
            schedule = Path(folder) / 'schedule.json'
            schedule.write_text(json.dumps([dict(id='loss', peer='erol', tick=601, action='host-kill')]))
            try:
                plan = cross_peers.make_plan(cross_peers.parse_args([
                    '--lane', 'test-lane', '--mac-guard', str(Path(folder) / 'guard'),
                    '--scenario', 'match', '--schedule', str(schedule)]))
            except ValueError as error:
                self.fail(str(error))
        self.assertEqual(plan['faults'][0]['action'], 'host-kill')
        self.assertFalse(next(s for s in plan['specs'] if s['role'] == 'host')['recoveries'])

    def test_host_loss_requires_native_successor_and_moderation_proof(self):
        reducer = getattr(cross_report, 'host_loss_evidence', None)
        self.assertTrue(callable(reducer), 'the host-loss oracle is absent')
        names = ['erol', 'edith', 'mac', 'linux']
        fault = dict(id='loss', peer='erol', action='host-kill', incarnation=0, tick=3)
        manifest = dict(host='erol', faults=[fault], ticks=6, instances=[dict(name=n) for n in names])
        context = dict(session='session', match='match', history_branch=0, source_round=1, round=1)
        actual = dict(context, budget_tick=3, applied_frame=3, host_peer=1, authority_generation=1)
        receipt = dict(id='loss', peer='erol', incarnation=0, execution='process-0/0', action='host-kill',
                       engine_pid=44, process_terminated=True, exit_code=-9, actual=actual,
                       before_wall_ms=10, after_wall_ms=11)
        state = dict(held_seats=[dict(seat=4, cause='capacity')], bans=['ban-digest'],
                     tickets=[dict(seat=2, incarnation=1, identity_sha256='a' * 64)])
        before = dict(context, type='moderation_snapshot', stage='before', tick=3, host_peer=1,
                      authority_generation=1, incarnation=0, execution='process-0/0', state=state)
        after = dict(context, type='moderation_snapshot', stage='after', tick=4, host_peer=2,
                     authority_generation=2, incarnation=0, execution='process-0/0', state=state)
        events = {n: [dict(after, instance=n)] for n in names[1:]}
        events['erol'] = [dict(before, instance='erol')]
        live = {n: [dict(context, tick=t, phase='live', instance=n, execution='process-0/0', incarnation=0,
                         host_peer=1 if t <= 3 else 2, authority_generation=1 if t <= 3 else 2,
                         sim_gated='a' * 64, subsystems={s: 'a' * 64 for s in CORE | {'controller'}})
                    for t in range(1, 4 if n == 'erol' else 7)] for n in names}
        peers = {n: dict(feel_gated=True, feel_pass=True, record=dict(exit_code=0, timed_out=False),
                         native_completion=dict(completion='completed'), configs=[dict(peer=i + 1)])
                 for i, n in enumerate(names)}
        peers['erol']['record'] = dict(pid=44, exit_code=-9, injected_termination='scheduled crash loss')
        good = reducer(manifest, peers, events, live, [receipt])
        self.assertTrue(good['passed'], good)
        for change in ('missing', 'alive', 'wrong_pid', 'state', 'authority', 'history', 'feel'):
            e, h, p, r = copy.deepcopy(events), copy.deepcopy(live), copy.deepcopy(peers), copy.deepcopy([receipt])
            if change == 'missing': r = []
            if change == 'alive': r[0]['process_terminated'] = False
            if change == 'wrong_pid': r[0]['engine_pid'] = None
            if change == 'state': e['mac'][0]['state']['bans'] = []
            if change == 'authority': h['mac'][-1]['host_peer'] = 3
            if change == 'history': h['linux'][-1]['subsystems']['controller'] = 'b' * 64
            if change == 'feel': p['edith']['feel_pass'] = False
            with self.subTest(change=change):
                self.assertFalse(reducer(manifest, p, e, h, r)['passed'])

    def test_termination_receipt_requires_finished_own_process(self):
        fault = dict(id='loss', action='host-kill')
        spec = dict(peer='erol', incarnation=0)
        record = dict(exit_code=-9, injected_termination='scheduled crash loss')
        self.assertTrue(cross_peers.termination_receipt(fault, spec, 44, {}, record, -9, 10, 11)['process_terminated'])
        for polled, pid in ((None, 44), (-9, None), (0, 44)):
            self.assertFalse(cross_peers.termination_receipt(fault, spec, pid, {}, record, polled, 10, 11)['process_terminated'])


if __name__ == '__main__':
    unittest.main()
