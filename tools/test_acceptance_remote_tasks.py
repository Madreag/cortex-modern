"""Remote acceptance safety and control checks; no engine or filesystem fixture."""
from copy import deepcopy
from contextlib import contextmanager
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import world_mod_cross as world
from test_world_mod_cross import baseline, mods
import acceptance_remote_tasks as remote
import acceptance_native_runtime as native_runtime


def profiles(row=None):
    from test_world_soak_tasks import profiles as windows_profiles
    boxes = windows_profiles()
    boxes[0].update(name='EROL-PC', kind='windows-local', hostname='EROL-PC',
                    tree='D:/Projects/takeover-build', executable='D:/Projects/takeover-build/Cortex Command.exe',
                    guard_file='D:/mx/BOX-FREE-FOR-CROSS', launch_floor_gib=10, max_engines=4,
                    affinity_mask='0x0000FFFF', engine_memory_gb=12,
                    runner='tools/run_sim_test.py / tools/win32_test_runner.py')
    boxes[0].pop('ssh', None); boxes[0].pop('task_script', None)
    for box in boxes:
        box['peers_per_box'] = 1
        box['hostname'] = box['name']
        if row == 'world-join':
            box['tree'] = 'D:/Projects/fencing-warm' if box['name'] == 'EROL-PC' else 'D:/mx/test/engine-tip'
            box['executable'] = box['tree']+'/Cortex Command.exe'
    for name, alias, root, host in (('Mac', 'Erol-Mac', '/Users/erol/cortex-workers/test', 'Erol-Mac'),
                                    ('Linux', '3090', '/home/erol/cortex-workers/test', 'linux-host')):
        boxes.append(dict(name=name, ssh=alias, kind='posix-ssh', peers_per_box=1, hostname=host,
                          scratch=root, helpers=root+'/helpers', tree=root+'/repo', executable=root+'/repo/build-gcc/CortexCommand',
                          runner='tools/posix_test_runner.py via run_sim_test.make_run over ssh',
                          python='python3', ports=[49120,49139], directory_port=49139, sampler='engine-pid-ps',
                          exclusive_marker=root+'/FEEL-MATRIX-RUNNING',
                          acceptance_marker=str(Path(root).parent/'ACCEPTANCE-STREAM-RUNNING').replace('\\','/')))
    return boxes


def options(row):
    return SimpleNamespace(row=row, lane='test', source_sha='a'*40, out=Path('D:/mx/test/resume-2/unit'),
                           template_boxes=Path(__file__).parent/'cross_peers/boxes.json')


class RemoteSafety(unittest.TestCase):
    def test_storage_refusal_precedes_staging_and_every_remote_launch(self):
        from acceptance_runtime import StorageLimit
        with patch('acceptance_runtime.retained_bytes', return_value=5_000_000_000), \
             patch.object(Path, 'mkdir') as mkdir, patch.object(remote, 'stage') as stage, \
             patch.object(remote, 'launch') as launch:
            with self.assertRaises(StorageLimit):
                remote.main(['--lane','test','--out','D:/mx/test/resume-2/unit',
                             '--evidence','D:/Projects/virtual-evidence','--row','mod-match'])
        mkdir.assert_not_called(); stage.assert_not_called(); launch.assert_not_called()

    def test_remote_plan_has_only_the_named_four_machines(self):
        for row in remote.ROWS:
            with self.subTest(row=row):
                plan = remote.make_plan(options(row), profiles(row), mods() if row.startswith('mod-') else None)
                self.assertEqual({spec['peer']:spec['box'] for spec in plan['specs']}, remote.PEERS)
                self.assertEqual(plan['coordinator_game_peer']['engine_instances'], 1)
                self.assertEqual(sum(box['kind'] == 'windows-local' for box in plan['boxes']), 1)
                for spec in plan['specs']:
                    box = next(box for box in plan['boxes'] if box['name'] == spec['box'])
                    self.assertTrue(spec['own'].startswith(remote.native_root(plan, box)+'/'))
                    self.assertEqual(spec['executable'], box['executable'])
                    self.assertEqual(spec['env']['CCCP_HEADLESS'], '1')

    def test_frozen_coordinator_commit_does_not_follow_the_live_worktree(self):
        frozen = dict(frozen_commit='7b9ff5067bc2901cfdac497d89a5961bb42afdb0',
                      frozen_export='/virtual/export', coordinator_commit='b'*40)
        boxes = profiles('mod-match')
        actual_context = remote.driver_git_context
        @contextmanager
        def pinned_context(root):
            # Preserve the real Git binding when this test runs in an export.
            with actual_context(root):
                yield frozen
        with patch.object(remote, 'driver_git_context', pinned_context), \
                patch.object(remote.cross, 'coordinator', create=True):
            plan = remote.make_plan(options('mod-match'), boxes, mods())
        self.assertEqual(plan['driver_commit'], frozen['coordinator_commit'])
        self.assertIn('source_worktree_observed', plan)

    def test_remote_host_keeps_the_standard_cross_workload(self):
        plan=remote.make_plan(options('mod-match'),profiles(),mods())
        self.assertEqual(next(spec for spec in plan['specs'] if spec['role']=='host')['initial_skill'],50)

    def test_profile_cannot_substitute_local_box_runner_floor_or_unowned_helpers(self):
        for field, value in (('name','Z13'), ('kind','windows-task'), ('runner','bare-exe'),
                             ('hostname',''), ('helpers','D:/mx/other/helpers'), ('peers_per_box',2),
                             ('exclusive_marker','D:/mx/test/private-marker'), ('launch_floor_gib',2)):
            with self.subTest(field=field), self.assertRaises(ValueError):
                box = profiles()[0]; box[field] = value
                remote.validate_profile(box, 'test')

    def test_wrong_physical_host_is_refused_before_capability_engine(self):
        import json
        payload = dict(box=profiles()[0], specs=[dict(peer='erol', acceptance_row='mod-match')])
        with patch.object(Path,'read_text',return_value=json.dumps(payload)), \
             patch.object(remote.platform,'node',return_value='EROL-TABLET'), \
             patch.object(remote.cross,'run_payload') as native, \
             patch.object(remote.cross,'acquire_reservation') as reserve:
            with self.assertRaisesRegex(ValueError,'wrong physical host'):
                remote.run_payload('/virtual/payload.json')
        native.assert_not_called(); reserve.assert_not_called()

    def test_native_preflights_require_expected_source_and_equal_windows_bytes(self):
        plan = remote.make_plan(options('world-join'), profiles('world-join'))
        # Keep this synthetic preflight's helper inside its synthetic lane;
        # the real planner binds the local helper to its actual bundle path.
        for box in plan['boxes']:
            if box['name'] == 'EROL-PC':
                box['helpers'] = box['scratch']+'/helpers'
        plan.setdefault('frozen_tools', dict(commit='c'*40))
        plan['preflights'] = {box['name']:dict(hostname=box['hostname'], machine_id=box['name'],
            executable_sha256=('e' if box['kind'].startswith('windows-') else 'f')*64,
            build=dict(commit='a'*40, executable_sha256=('e' if box['kind'].startswith('windows-') else 'f')*64),
            frozen_tools=dict(commit=plan['frozen_tools']['commit'], frozen_files_modified=0),
            content={'Base.rte/Index.ini':'d'*64}, modules={}, fixture={}, load=[],
            acceptance_driver_sources={key:plan['driver_sources'][key] for key in world.DRIVER_FILES},
            acceptance_remote_driver_sha256=plan['driver_sources']['acceptance_remote_tasks.py']) for box in plan['boxes']}
        remote.validate_preflights(plan)
        missing_frozen = deepcopy(plan)
        missing_frozen['preflights']['Mac'].pop('frozen_tools')
        with self.assertRaisesRegex(ValueError, 'frozen NOTE 11 tool identity'):
            remote.validate_preflights(missing_frozen)
        held=deepcopy(plan)
        held['reservation_holders']={name:{} for name in remote.ALIASES}
        for value in held['preflights'].values(): value['preflight_reservation']=dict(borrowed=True)
        remote.validate_preflights(held)
        held['preflights']['Mac']['preflight_reservation']['borrowed']=False
        with self.assertRaisesRegex(ValueError,'did not hold'):
            remote.validate_preflights(held)
        for field, value in (('machine_id','EROL-PC'), ('hostname','EROL-PC'), ('content',{}),
                             ('build',dict(commit='b'*40,executable_sha256='e'*64)),
                             ('acceptance_remote_driver_sha256','b'*64), ('load',[{'Name':'ninja'}])):
            with self.subTest(field=field), self.assertRaises((ValueError,RuntimeError)):
                bad = deepcopy(plan); bad['preflights']['EDITH'][field] = value
                remote.validate_preflights(bad)
        bad = deepcopy(plan)
        bad['preflights']['EDITH']['executable_sha256'] = 'b'*64
        bad['preflights']['EDITH']['build']['executable_sha256'] = 'b'*64
        with self.assertRaisesRegex(ValueError, 'Windows executable bytes differ'):
            remote.validate_preflights(bad)

    def test_control_publication_preserves_the_transferred_candidate(self):
        box = profiles()[1]
        with patch.object(remote.cross, 'command') as transfer, patch.object(remote,'remote_python') as publish:
            remote.publish_new(box, Path('/virtual/session.json'), 'D:/mx/test/session.json')
        self.assertEqual(transfer.call_args.args[0][0], 'scp')
        self.assertEqual(publish.call_args.args[1], 'import os,sys; os.link(sys.argv[1],sys.argv[2])')

    def test_private_posix_reservation_cannot_bypass_active_acceptance_stream(self):
        for box in profiles()[2:]:
            box['exclusive_marker'] = box['scratch']+'/private-reservation'
            with self.assertRaisesRegex(ValueError, 'shared acceptance stream marker'):
                remote.validate_profile(box, 'test')

    def test_frozen_posix_profiles_require_both_reservations(self):
        for box in profiles()[2:]:
            remote.validate_profile(box, 'test')
            box['acceptance_marker'] = box['scratch']+'/private-stream'
            with self.assertRaisesRegex(ValueError, 'shared acceptance stream marker'):
                remote.validate_profile(box, 'test')

    def test_note8_rows_cannot_use_acceptance_or_engineer_development_files(self):
        for name, tree in [('EROL-PC','D:/Projects/z13-build'), ('EROL-PC','D:/Projects/z13-dev-build'),
                           ('EDITH','D:/Projects/inventory-build')]:
            with self.subTest(name=name, tree=tree):
                boxes=profiles('world-join')
                selected=next(box for box in boxes if box['name']==name)
                selected.update(tree=tree,executable=tree+'/Cortex Command.exe')
                with self.assertRaises(ValueError):
                    remote.make_plan(options('world-join'), boxes)
        boxes=profiles('world-join')
        with self.assertRaises(ValueError):
            remote.make_plan(options('mod-match'), boxes, mods())
        with self.assertRaises(ValueError):
            remote.make_plan(options('world-join'), profiles())

    def test_remote_failure_releases_only_the_claim_and_restores_environment(self):
        import json, os
        from feel import launch_budget
        payload = dict(box=profiles()[0], specs=[dict(peer='erol', acceptance_row='mod-match')])
        claim = dict(record=dict(pid=123, token='fixture'))
        before = dict(os.environ)
        with patch.object(Path,'read_text',return_value=json.dumps(payload)), \
             patch.object(remote, 'frozen_receipt', return_value=None), \
             patch.object(remote.platform,'node',return_value='EROL-PC'), \
             patch.object(native_runtime,'run_payload',side_effect=RuntimeError('native failure')), \
             patch.object(remote.cross,'acquire_reservation',return_value=claim), \
             patch.object(remote.cross,'release_reservation',return_value=True) as release, \
             patch.object(launch_budget,'install_memory_guard'), patch.object(remote,'write_json'):
            with self.assertRaisesRegex(RuntimeError,'native failure'):
                remote.run_payload('/virtual/payload.json')
        release.assert_called_once_with(claim)
        self.assertEqual(dict(os.environ), before)

    def test_external_native_build_receipt_requires_its_original_log_hash(self):
        import json
        box = profiles()[2]
        box['build_receipt'] = '/virtual/build.json'
        payload = dict(box=box, specs=[dict(peer='mac', acceptance_row='world-join')])
        result = dict(executable_sha256='e'*64)
        build = dict(commit='a'*40, executable_sha256='e'*64, configuration='release',
                     build_exit_code=0, build_log='/virtual/build.log', build_log_sha256='b'*64)
        def read(path, **kwargs):
            return json.dumps(build if path.name == 'build.json' else payload if path.name == 'payload.json' else result)
        for log_hash in ('b'*64, 'c'*64):
            with self.subTest(log_hash=log_hash), patch.object(Path,'read_text',read), \
                 patch.object(remote, 'frozen_receipt', return_value=None), \
                 patch.object(remote.platform,'node',return_value=box['hostname']), \
                 patch.object(remote.cross,'preflight_payload',return_value=0), \
                 patch.object(remote,'sha256',side_effect=lambda path:log_hash if path.name=='build.log' else 'd'*64), \
                 patch.object(remote,'write_json') as write:
                if log_hash == 'b'*64:
                    self.assertEqual(remote.preflight_payload('/virtual/payload.json'),0)
                    self.assertEqual(write.call_args.args[1]['build'],build)
                else:
                    with self.assertRaisesRegex(ValueError,'build log differs'):
                        remote.preflight_payload('/virtual/payload.json')


class NativeControl(unittest.TestCase):
    def test_remote_rows_publish_directory_before_first_live_tick(self):
        for row in ('mod-match', 'mod-refusal', 'world-join'):
            with self.subTest(row=row):
                spec = dict(acceptance_row=row, role='host', task_control=True,
                            own='/virtual/host', root='/virtual')
                with patch.object(Path, 'is_file', return_value=True), \
                     patch.object(Path, 'read_text', return_value='[net-directory] registered session_id=fixture heartbeat_s=10\n'), \
                     patch.object(world, 'latest_tick', return_value=None), \
                     patch.object(world, 'publish_control') as publish:
                    world.observe_soak(spec, SimpleNamespace(cwd=Path('/virtual')), 100)
                publish.assert_called_once()
                self.assertEqual(publish.call_args.args[1]['session'], 'fixture')
                self.assertIsNone(publish.call_args.args[1]['host_tick'])

    def test_world_budget_covers_fixed_memory_warmup(self):
        plan = world.configure_plan(baseline(), 'world-join')
        self.assertGreaterEqual(plan['ticks'], 12001)

    def test_short_rows_keep_the_normal_census_cadence(self):
        for row in ('mod-match', 'mod-refusal', 'world-join'):
            with self.subTest(row=row):
                original = baseline()
                for spec in original['specs']:
                    spec['flags'] += ['-memory-census-ticks', '1800']
                plan = world.configure_plan(original, row, mods() if row.startswith('mod-') else None)
                for spec in plan['specs']:
                    flags = spec['flags']
                    self.assertEqual(flags[flags.index('-memory-census-ticks')+1], '1800')


if __name__ == '__main__':
    unittest.main()
