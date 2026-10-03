"""Section authorization and deferred evidence never hide completed failures."""
import contextlib
import io
import json
from pathlib import Path
from types import SimpleNamespace
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import acceptance_collection as collection
import cross_peers
import cross_report
from feel.test_attempt_requirements import attempt
from test_acceptance_resume import fixture, reader, write

REPO = Path(__file__).resolve().parents[1]


class AcceptanceSections(unittest.TestCase):
    def test_a_ready_capture_cannot_invent_the_awaited_readback_proof(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); plan, required = fixture(root,boxes=('EDITH',))
            value = json.loads(plan.read_text())
            value['rows'][0].update(section=2,engine_boxes={'EDITH':3},
                blocked_reason='blocked by the EDITH readback fix (engine row A57.2)')
            value['box_policy'] = dict(version=2)
            write(plan,value)
            schedule = json.loads((root/'split-plan.json').read_text())
            write(root/'sections/2.json',dict(schema=1,section=2,source_sha='a'*40,collection_id='one',
                window=dict(variant='WITH'),capabilities={'EDITH.readback':dict(passed=True)},
                rows=[dict(share='S1',id='case',state='READY',reason='')]))
            schedule['section_receipts'] = {'2':dict(path='sections/2.json',sha256=reader.digest(root/'sections/2.json'))}
            write(root/'split-plan.json',schedule)
            result = reader.build_manifest(plan,required)
            self.assertFalse(result['rows'][0]['passed'],result['rows'][0])

    def test_reader_and_native_identity_bind_the_new_game_roster(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); plan, required = fixture(root,item='18',boxes=('Z13','EDITH','Mac'))
            doc = json.loads(plan.read_text())
            doc['rows'][0]['engine_boxes'] = {'Z13':1,'EDITH':1,'Mac':1}
            write(plan,doc)
            judged = reader.build_manifest(plan,required)
            self.assertTrue(judged['rows'][0]['passed'],judged['rows'][0]['errors'])

    def test_native_identity_binds_the_new_game_roster(self):
        manifest, _, peers, _, _ = attempt()
        manifest['driver'] = dict(name='EROL-PC',kind='coordinator',directory_port=49918)
        manifest['host'] = 'z13'
        for value in manifest['instances']:
            if value['name']=='erol':value.update(name='z13',box='Z13')
        for value in manifest['specs']:
            if value['peer']=='erol':value.update(peer='z13',box='Z13')
        for value in manifest['boxes']:
            if value['name']=='EROL-PC':value['name']='Z13'
        manifest['preflights']['Z13'] = manifest['preflights'].pop('EROL-PC')
        peers['z13'] = peers.pop('erol')
        checked = cross_report.acceptance_identity(manifest,peers)
        self.assertTrue(checked['roster_passed'],checked['roster_errors'])

    def test_remote_windows_host_is_not_selected_as_the_crash_restart_client(self):
        peers = [dict(name='z13',box='Z13'),dict(name='edith',box='EDITH'),dict(name='mac',box='Mac')]
        boxes = {'Z13':dict(kind='windows-task'),'EDITH':dict(kind='windows-task'),'Mac':dict(kind='posix-ssh')}
        options = SimpleNamespace(schedule=None,scenario='soak',host='z13',ticks=72000,recovery_deadline_ms=120000)
        faults = cross_peers.schedule_for(options,peers,boxes)
        self.assertEqual({row['peer'] for row in faults if row['action']=='crash-restart'}, {'edith'})

    def test_native_feel_groups_preserve_every_arm_and_the_engine_cap(self):
        with tempfile.TemporaryDirectory() as folder:
            plans = {}
            for group in ('all', 'pair', 'three'):
                argv = [sys.executable, '-B', str(REPO/'tools/feel_measure.py'), '--out', str(Path(folder)/group),
                        '--port', '48231', '--dry-run', '--matrix-group', group]
                result = subprocess.run(argv, capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr)
                plans[group] = json.loads(result.stdout[result.stdout.index('{'):result.stdout.rindex('}')+1])['arms']
            self.assertTrue(all(len(arm['peers']) <= 2 for arm in plans['pair']))
            self.assertEqual({arm['arm'] for arm in plans['pair']} | {arm['arm'] for arm in plans['three']},
                             {arm['arm'] for arm in plans['all']})
            self.assertEqual(sum(len(arm['peers']) == 3 for arm in plans['three']), 6)
            self.assertFalse(any(Path(folder).iterdir()))

    def test_a_declared_matrix_group_cannot_pass_on_a_generic_green_product(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); plan, required = fixture(root, item='20')
            value = json.loads(plan.read_text())
            value['rows'][0].update(matrix_group='pair', arms=['100ms-60hz-on','100ms-60hz-off'])
            write(plan, value)
            result = reader.build_manifest(plan, required)
            self.assertFalse(result['rows'][0]['passed'])
            self.assertIn('declared matrix arms', ' '.join(result['rows'][0]['errors']))

    def test_window_deferred_row_is_awaiting_with_its_exact_reason(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); plan, required = fixture(root, item='20')
            value = json.loads(plan.read_text())
            value['rows'][0].update(section=3, window_required=True, engine_boxes={'EROL-PC':2})
            value['box_policy'] = dict(version=2)
            write(plan, value)
            schedule = json.loads((root/'split-plan.json').read_text())
            section = dict(schema=1, section=3, source_sha='a'*40, collection_id='one',
                           window=dict(variant='WITHOUT', reason='deferred to the EROL-PC window',
                                       marker_token='USER-AT-DESK', marker_sha256='b'*64),
                           rows=[dict(share='S1', id='case', state='AWAITING', reason='deferred to the EROL-PC window')])
            write(root/'sections/3.json', section)
            schedule['section_receipts'] = {'3':dict(path='sections/3.json', sha256=reader.digest(root/'sections/3.json'))}
            write(root/'split-plan.json', schedule)
            (root/'S1/case/command.json').unlink()
            (root/'S1/case/result.json').unlink()
            write(root/'S1/DEFECTS.json', dict(collection_id='one',source_sha='a'*40,collection_complete=True,hard_count=0,
                  commands=[dict(id='case',state='awaiting',reason=section['rows'][0]['reason'],engine_started=False)],
                  evidence_sha256={}))
            result = reader.build_manifest(plan, required)
            row = result['rows'][0]
            self.assertEqual(row['status'], 'AWAITING', row)
            self.assertEqual(row.get('reason'), 'deferred to the EROL-PC window')
            self.assertFalse(row['passed'])

    def test_section_start_reads_marker_and_cannot_be_rewritten_after_a_launch(self):
        start_section = getattr(collection, 'start_section', None)
        self.assertTrue(callable(start_section), 'the section start must persist the marker-bound choice')
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); plan, _ = fixture(root, item='20')
            value = json.loads(plan.read_text())
            value['rows'][0].update(section=3, window_required=True, engine_boxes={'EROL-PC':2})
            write(root/'acceptance-plan.json', value)
            (root/'S1/case/command.json').unlink()
            (root/'S1/case/result.json').unlink()
            marker = root/'marker';write(marker, dict(token='USER-AT-DESK'))
            result = start_section(root, 3, marker=marker)
            self.assertEqual(result['rows'][0]['state'], 'AWAITING')
            write(marker, dict(token='named-window'))
            with self.assertRaisesRegex(ValueError, 'already'):
                start_section(root, 3, marker=marker)

    def test_cross_dry_run_supports_a_driver_without_a_local_game_peer(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            document = json.loads((REPO/'tools/cross_peers/boxes.json').read_text())
            local = next(box for box in document['boxes'] if box['kind']=='windows-local')
            local.update(name='Z13', kind='windows-task', ssh='z13', runner='cortex-session1',
                         task_script='D:/mx/session1/run.ps1', directory_port=49985)
            local.pop('exclusive_marker',None);local.pop('guard_file',None)
            for peer in document['instances']:
                if peer['box']=='EROL-PC':peer.update(name='z13',box='Z13')
            document['driver'] = dict(name='EROL-PC',kind='coordinator',directory_port=49918)
            boxes = root/'boxes.json';write(boxes,document)
            with contextlib.redirect_stdout(io.StringIO()):
                options = cross_peers.parse_args(['--boxes',str(boxes),'--host','z13','--scenario','match',
                    '--roster','four-way','--out',str(root/'unit'),'--lane','unit','--mac-guard',str(root/'guard'),'--dry-run'])
                plan = cross_peers.make_plan(options)
                try:
                    cross_peers.dry_run(plan)
                except (StopIteration, ValueError) as error:
                    self.fail(f'remote-only game roster rejected: {error}')
            self.assertFalse(any(box['kind']=='windows-local' for box in plan['boxes']))


if __name__ == '__main__':
    unittest.main()
