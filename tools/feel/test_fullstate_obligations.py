import contextlib
import copy
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import cross_peers
import cross_report
from feel import report


def sample(round_id, tick, process='1'):
    return dict(key=(round_id, tick, 'sample'), hash='a' * 16, sections={'header': 'a' * 16},
                scope_valid=True, scope={'per_peer': []},
                context=dict(path=f'/run/process-{process}/round-{round_id}/capture-{tick}/sample'),
                log=f'process-{process}/stdout.log', line=tick)


def document(samples):
    return dict(samples=samples, refusals=[], coalesced=[], holds=[])


def cross_fixture(root):
    plan = cross_peers.make_plan(cross_peers.parse_args(['--lane', 'unit-oracle', '--mac-guard', 'unit-marker', '--dry-run']))
    plan.update(ticks=601, fullstate_every=600, capture_rows_pending=[])
    plan['preflights'] = {box['name']: dict(machine_id=box['name'], executable_sha256='c' * 64) for box in plan['boxes']}
    (root / 'manifest.json').write_text(json.dumps(plan))
    for spec in plan['specs']:
        own = cross_report.peer_root(root, plan, spec)
        (own / 'engine').mkdir(parents=True)
        (own / 'record.json').write_text(json.dumps(dict(started=True, exit_code=0, elapsed_seconds=10,
                                                       timed_out=False, exe_sha256='c' * 64)))
        (own / 'trace.json').write_text(json.dumps(dict(runs=[dict(strings=dict(completion='completed'))])))
        (own / 'match-report.json').write_text('{}')
        live = [dict(session='s', match='1', history_branch='initial', source_round=1, round=1, tick=tick,
                     instance=spec['peer'], execution='one', incarnation=0, phase='live', wall_ms=tick * 1000 / 60,
                     gameplay_tick=True, effective_start_frame=1, sim_gated='a' * 64,
                     subsystems={key: 'b' * 64 for key in cross_report.REQUIRED_SUBSYSTEMS}) for tick in range(1, 602)]
        (own / 'live.jsonl').write_text('\n'.join(map(json.dumps, live)))
        (own / 'events.jsonl').write_text('')
        log = '[net-lockstep] start round=1 frame=1 local_peer=1 peers=3\n'
        log += '[net-lockstep] return of peer 2 at 360 delay=4 neutral_through=370 revision=2 incarnation=1 next=360 clock=6000\n'
        for tick in (1, 600):
            log += (f'[fullstate-context] tick={tick} round=1 label=sample path=/instance/process-1/round-1/capture-{tick}/sample\n'
                    f'[fullstate] tick={tick} hash=0123456789abcdef sections=header:0123456789abcdef round=1\n'
                    f'[fullstate-scope] tick={tick} round=1 label=sample per_peer=camera\n')
        (own / 'engine/stdout.log').write_text(log)
    return plan


class FullstateObligations(unittest.TestCase):
    def test_p20_globally_missing_landed_sample_does_not_erase_the_obligation(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            cross_fixture(root)
            with patch.object(cross_report, 'write_page'), patch.object(cross_report, 'write_index'), \
                 patch.object(cross_report, 'write_rerun_command', return_value='unit'), contextlib.redirect_stdout(io.StringIO()):
                result = cross_report.build_report(root)
        self.assertFalse(result['fullstate']['passed'], result['fullstate'])
        self.assertTrue(any(row['key'][2] == 'landed' for row in result['fullstate']['missing']))

    def test_p21_coalescing_in_another_round_cannot_excuse_a_missing_sample(self):
        host = document([sample(1, 600), sample(1, 900), sample(2, 900)])
        client = document([sample(1, 600, '2'), sample(2, 900, '2')])
        client['coalesced'] = [dict(tick=1000, replaced=900, round=2, process='2', capture='7',
                                    log='process-2/stdout.log', line=22)]
        result = report.compare_fullstate_histories({'host': host, 'client': client},
            [(1, 600, 'sample'), (1, 900, 'sample'), (2, 900, 'sample')])
        self.assertFalse(result['passed'], result)

    def test_valid_receipt_in_another_round_cannot_be_spent_here(self):
        host = document([sample(1, 600), sample(1, 900), sample(2, 1000)])
        client = document([sample(1, 600, '2'), sample(2, 1000, '2')])
        identity = ('process-2/stdout.log', '2', '2', '7', 'sample')
        client['coalesced'] = [dict(valid=True, tick=1000, replaced=900, round=2, process='2',
            capture_identity=identity, context=dict(log='process-2/stdout.log', capture_identity=identity))]
        result = report.compare_fullstate_histories({'host': host, 'client': client}, [(1, 600, 'sample'), (1, 900, 'sample')])
        self.assertFalse(result['passed'], result)
        self.assertEqual(result['missing'], [dict(peer='client', key=(1, 900, 'sample'))])

    def test_agreed_gap_end_drives_the_landed_tick_and_conflicts_fail(self):
        receipt = dict(round=1, peer=2, frame=100, delay=4, neutral_through=130, revision=3, incarnation=1)
        ranges = [dict(match='1', first=1, last=300)]
        peers = {name: dict(reclaims=[dict(receipt)]) for name in ('a', 'b')}
        result = report.reclaim_sample_obligations(peers, ranges)
        self.assertIn((1, 190, 'landed'), result['expected'])
        peers['b']['reclaims'][0]['neutral_through'] = 131
        self.assertTrue(report.reclaim_sample_obligations(peers, ranges)['invalid'])

    def test_coalesce_cannot_borrow_another_process_capture(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'stdout.log'
            path.write_text(
                '[fullstate-context] tick=60 round=1 label=sample path=/run/process-1/round-1/capture-1/sample\n'
                '[fullstate-context] tick=120 round=1 label=sample path=/run/process-2/round-1/capture-2/sample\n'
                '[fullstate-coalesced] tick=120 replaced=60 writing=0 waiting_bound=1\n')
            parsed = report.parse_fullstate([path])
        self.assertFalse(parsed['coalesced'][0]['valid'])


if __name__ == '__main__':
    unittest.main()
