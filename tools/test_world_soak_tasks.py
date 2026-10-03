from copy import deepcopy
import json
import os
from pathlib import Path
import tempfile
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


if __name__ == '__main__':
    unittest.main(verbosity=2)
