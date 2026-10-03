from copy import deepcopy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import world_soak_tasks as tasks
import world_mod_cross as world


def profiles():
    return [dict(name=name, kind='windows-task', ssh=alias, runner='cortex-session1', task_script='D:/mx/session1/run.ps1',
                 scratch='D:/mx/test', helpers='D:/mx/test/helpers-a', tree=tree, executable=tree+'/Cortex Command.exe',
                 exclusive_marker='D:/mx/FEEL-MATRIX-RUNNING', peers_per_box=count, launch_floor_gib=floor,
                 ports=[49120,49139], python='python', sampler='engine-process-handle', directory_port=49139)
            for name,alias,tree,count,floor in [('Z13','z13','D:/Projects/z13-build',1,6.5),
                                               ('EDITH','edith','D:/Projects/inventory-build',2,10)]]


def preflight_plan():
    boxes=profiles()
    values={box['name']:dict(machine_id=box['name'], executable_sha256='e'*64,
                            build=dict(commit='a'*40, executable_sha256='e'*64),
                            content={'Base.rte/Index.ini':'f'*64}, modules={}, fixture={}, load=[],
                            acceptance_driver_sources=dict.fromkeys(world.DRIVER_FILES, 'b'*64)) for box in boxes}
    return dict(boxes=boxes, preflights=values, acceptance_row='world-soak', driver_sources=dict.fromkeys(world.DRIVER_FILES, 'b'*64))


class TaskSoak(unittest.TestCase):
    def setUp(self):
        retained=os.environ.get('CC_ACCEPTANCE_TEST_ROOT')
        if retained:
            self.root=Path(tempfile.mkdtemp(prefix='task-soak-test-', dir=retained))
        else:
            temporary=tempfile.TemporaryDirectory()
            self.addCleanup(temporary.cleanup)
            self.root=Path(temporary.name)

    def test_only_the_authorized_task_roster_and_floors_are_accepted(self):
        for box in profiles():
            tasks.task_profile(box, 'test')
        for key,value in [('ssh','ally'), ('kind','windows-local'), ('peers_per_box',2), ('launch_floor_gib',2),
                          ('helpers','D:/mx/test/../../other'), ('tree','D:/Projects/other')]:
            with self.subTest(field=key), self.assertRaises(ValueError):
                tasks.task_profile({**profiles()[0],key:value}, 'test')

    def test_plan_has_no_local_engine_and_releases_only_the_second_edith_seat_late(self):
        options=SimpleNamespace(template_boxes=Path(__file__).parent/'cross_peers/boxes.json', lane='test', out=self.root/'r5-unit')
        plan=tasks.make_plan(options,profiles())
        self.assertEqual({box['name'] for box in plan['boxes']},{'Z13','EDITH'})
        self.assertTrue(all(box['kind']=='windows-task' for box in plan['boxes']))
        self.assertEqual(plan['world_host_box'],'Z13')
        self.assertEqual(plan['coordinator_only']['engine_instances'],0)
        self.assertEqual(plan['ticks'],219601)
        self.assertEqual(plan['late_join'],dict(peer='edith',host_elapsed_s=3000))
        self.assertEqual([(spec['peer'],spec['box']) for spec in plan['specs']],
                         [('erol','Z13'),('edith-first','EDITH'),('edith','EDITH')])
        self.assertEqual([spec['peer'] for spec in plan['specs'] if spec.get('defer_until_session')],['edith'])
        self.assertEqual([spec['port_block'] for spec in plan['specs'] if spec['box']=='EDITH'],[[49125,49129],[49120,49124]])
        host=next(spec for spec in plan['specs'] if spec['role']=='host')
        self.assertNotIn('CC_TEST_NET_UI_SCRIPT',host['env'],'a dedicated host has no local player for buy/pie assertions')

    def test_measured_build_content_and_driver_bytes_bind_both_boxes(self):
        plan=preflight_plan()
        tasks.validate_preflights(plan)
        self.assertEqual(plan['source_sha'], 'a'*40)
        for field,value in [('machine_id','Z13'), ('executable_sha256','c'*64), ('content',{}),
                            ('acceptance_driver_sources',{}), ('load',[{'Name':'Cortex Command.exe'}])]:
            with self.subTest(field=field), self.assertRaises((ValueError,RuntimeError)):
                bad=deepcopy(plan); bad['preflights']['EDITH'][field]=value
                tasks.validate_preflights(bad)
        bad=deepcopy(plan); bad['preflights']['EDITH']['build']['commit']='c'*40
        with self.assertRaises(ValueError): tasks.validate_preflights(bad)

    def test_late_world_seat_does_not_run_actor_ui_probe_during_private_catchup(self):
        options=SimpleNamespace(template_boxes=Path(__file__).parent/'cross_peers/boxes.json', lane='test', out=self.root/'r5-ui')
        plan=tasks.make_plan(options,profiles())
        by_peer={spec['peer']:spec for spec in plan['specs']}
        self.assertNotIn('CC_TEST_NET_UI_SCRIPT',by_peer['edith']['env'])
        self.assertIn('CC_TEST_NET_UI_SCRIPT',by_peer['edith-first']['env'])
        self.assertTrue(by_peer['edith']['defer_until_session'])

    def test_publication_is_observed_before_the_first_live_tick(self):
        own=self.root/'host'; (own/'engine').mkdir(parents=True)
        (own/'engine/stdout.log').write_text('[net-directory] registered session_id=fixture-session heartbeat_s=10\n', encoding='utf-8')
        spec=dict(acceptance_row='world-soak', role='host', task_control=True, own=str(own), root=str(self.root))
        with patch('world_mod_cross.latest_tick', return_value=None):
            world.observe_soak(spec, SimpleNamespace(cwd=self.root), 10)
        value=json.loads((self.root/'host-control.json').read_text())
        self.assertEqual(value['session'], 'fixture-session')
        self.assertIsNone(value['host_tick'])
        self.assertIsNone(value['host_elapsed_s'])
        self.assertNotIn('_soak_clock', spec)

    def test_native_host_clock_starts_only_after_live_evidence(self):
        own=self.root/'host'; (own/'engine').mkdir(parents=True)
        (own/'engine/stdout.log').write_text('', encoding='utf-8')
        spec=dict(acceptance_row='world-soak', role='host', task_control=True, own=str(own), root=str(self.root))
        with patch('world_mod_cross.latest_tick', return_value=60):
            world.observe_soak(spec, SimpleNamespace(cwd=self.root), 100)
        with patch('world_mod_cross.latest_tick', return_value=120):
            world.observe_soak(spec, SimpleNamespace(cwd=self.root), 101)
        value=json.loads((self.root/'host-control.json').read_text())
        self.assertEqual(value['host_elapsed_s'], 1)
        self.assertEqual(value['host_tick'], 120)

    def test_payload_reuses_the_cross_runner_inside_its_own_reservation(self):
        box=profiles()[0]
        payload=self.root/'payload.json'
        payload.write_text(json.dumps(dict(box=box, specs=[dict(peer='erol', acceptance_row='world-soak')])))
        claim=dict(record=dict(pid=123, token='fixture-token'))
        from feel import launch_budget
        previous=launch_budget.MIN_FREE_BYTES
        calls=[]
        def run(path):
            self.assertEqual(path,payload)
            self.assertEqual(launch_budget.MIN_FREE_BYTES,int(6.5*1024**3))
            calls.append('run'); return 7
        with patch.object(tasks.cross,'acquire_reservation', side_effect=lambda *args: calls.append('claim') or claim), \
             patch.object(tasks.cross,'run_payload', side_effect=run), \
             patch.object(tasks.cross,'release_reservation', side_effect=lambda value: calls.append('release') or True):
            self.assertEqual(tasks.run_payload(payload),7)
        self.assertEqual(calls,['claim','run','release'])
        self.assertEqual(launch_budget.MIN_FREE_BYTES,previous)
        self.assertNotIn('token',json.loads((self.root/'reservation.json').read_text()))

    @unittest.skipUnless(sys.platform == 'win32', 'native Windows file sharing regression')
    def test_coordinator_read_allows_atomic_publication_while_handle_is_open(self):
        path=self.root/'control.json'; opened=self.root/'reader-open'; release=self.root/'reader-release'
        tasks.cross.write_json(path, dict(tick=1))
        class LocalTransport:
            def ssh(self, command):
                hold = ("[IO.File]::WriteAllText("+tasks.ps_quote(opened)+", 'open'); "
                        "$until=[DateTime]::UtcNow.AddSeconds(10); "
                        "while (-not (Test-Path -LiteralPath "+tasks.ps_quote(release)+") -and [DateTime]::UtcNow -lt $until) { Start-Sleep -Milliseconds 10 }; ")
                command=command.replace('$reader =', hold+'$reader =')
                return subprocess.check_output(['pwsh','-NoProfile','-Command',command],text=True,
                                               creationflags=subprocess.CREATE_NO_WINDOW)
        result=[]
        reader=threading.Thread(target=lambda: result.append(tasks.read_json(LocalTransport(), str(path))))
        reader.start()
        try:
            deadline=time.monotonic()+10
            while not opened.is_file() and time.monotonic()<deadline: time.sleep(.01)
            self.assertTrue(opened.is_file(), 'reader never opened the native file')
            release_timer=threading.Timer(.15, lambda: release.write_text('release'))
            release_timer.start()
            world.publish_control(path, dict(tick=2))
            release_timer.join()
        finally:
            release.write_text('release')
            reader.join(15)
        self.assertFalse(reader.is_alive())
        self.assertEqual(result,[dict(tick=1)], 'the open reader retains the old complete snapshot')
        self.assertEqual(json.loads(path.read_text()),dict(tick=2))

    def test_persistent_control_publication_denial_is_not_ignored(self):
        with patch.object(tasks.cross,'write_json',side_effect=PermissionError('persistent denial')), \
             patch.object(world.time,'monotonic',side_effect=[0,3]):
            with self.assertRaises(PermissionError):
                world.publish_control(self.root/'control.json',dict(tick=1))

    def test_packed_evidence_retains_native_bytes_and_record_part_discovery(self):
        import io
        import tarfile
        from acceptance_evidence import rows
        from cross_report import event_paths
        from feel.records import open_record
        archive=self.root/'evidence.tar.gz'; target=self.root/'fetched'
        native={'peer/live.jsonl':b'{"tick":1}\n{"tick":2}\n',
                'peer/events.jsonl':b'{"type":"progress"}\n',
                'peer/events.jsonl.part1':b'{"type":"tick_timing"}\n',
                'peer/engine/stdout.log':b'native stdout\n'}
        with tarfile.open(archive,'w:gz') as stream:
            for name,data in native.items():
                item=tarfile.TarInfo(name); item.size=len(data); stream.addfile(item,io.BytesIO(data))
        before=archive.read_bytes()
        self.assertEqual(world.extract_preserved(archive,target,compress_records=True),len(native))
        self.assertEqual(archive.read_bytes(),before)
        self.assertEqual(list(rows(target/'peer/live.jsonl')),[dict(tick=1),dict(tick=2)])
        self.assertEqual([path.name for path in event_paths(target/'peer')],['events.jsonl','events.jsonl.part1'])
        for name,data in native.items():
            with open_record(target/name,'rb') as stream: self.assertEqual(stream.read(),data)
        receipt=json.loads((target/'fetch-compression.json').read_text())
        self.assertEqual(len(receipt['files']),3)
        self.assertEqual(receipt['removed_files'],0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
