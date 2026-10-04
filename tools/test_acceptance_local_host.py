"""NOTE 12 host placement and transport checks without starting an engine."""
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import acceptance_remote_tasks as remote
from test_acceptance_remote_tasks import profiles, options
from test_world_mod_cross import mods


def local_profiles(row='mod-match'):
    boxes = profiles(row)
    tree = 'D:/Projects/fencing-warm' if row == 'world-join' else 'D:/Projects/takeover-build'
    boxes[0].update(name='EROL-PC', kind='windows-local', hostname='EROL-PC',
                    tree=tree, executable=tree+'/Cortex Command.exe',
                    guard_file='D:/mx/BOX-FREE-FOR-CROSS', launch_floor_gib=10,
                    max_engines=4, affinity_mask='0x0000FFFF', engine_memory_gb=12)
    boxes[0].pop('ssh', None)
    boxes[0].pop('task_script', None)
    boxes[0]['runner'] = 'tools/run_sim_test.py / tools/win32_test_runner.py'
    for box in boxes[2:]:
        box.update(acceptance_marker=str(Path(box['scratch']).parent/'ACCEPTANCE-STREAM-RUNNING').replace('\\','/'),
                   exclusive_marker=box['scratch']+'/FEEL-MATRIX-RUNNING')
    return boxes


class LocalHost(unittest.TestCase):
    def test_original_local_host_plan_uses_the_ruled_executable(self):
        for row in remote.ROWS:
            boxes = local_profiles(row)
            plan = remote.make_plan(options(row), boxes, mods() if row.startswith('mod-') else None)
            host = next(spec for spec in plan['specs'] if spec['role'] == 'host')
            self.assertEqual(host['box'], 'EROL-PC')
            self.assertEqual(Path(host['root']), options(row).out)
            self.assertEqual(host['executable'], boxes[0]['executable'])
            self.assertNotIn('driver', plan)
            self.assertNotIn('Z13', str(plan['boxes']))

    def test_retired_host_is_refused_before_any_transport(self):
        box = dict(name='Z13', kind='windows-task', ssh='z13')
        operations = (lambda: remote.remote_python(box, 'print(1)'),
                      lambda: remote.read_json(box, '/virtual/payload.json'),
                      lambda: remote.publish_new(box, Path('/virtual/a'), '/virtual/b'))
        with patch.object(remote.cross, 'command') as command, patch.object(remote, 'RemoteBox') as task:
            for operation in operations:
                with self.assertRaisesRegex(ValueError, 'retired|authorized'):
                    operation()
        command.assert_not_called()
        task.assert_not_called()

    def test_local_transport_is_a_native_python_process(self):
        box = local_profiles()[0]
        with patch.object(remote.cross, 'command', return_value='ok') as command:
            self.assertEqual(remote.remote_python(box, 'print(1)'), 'ok')
        self.assertEqual(command.call_args.args[0], [box['python'], '-c', 'print(1)'])

    def test_pc_limits_guard_and_path_cannot_be_relaxed(self):
        for key, value in [('tree','D:/Projects/ux-smalls-2'), ('executable','D:/mx/private/Cortex Command.exe'),
                ('guard_file',None), ('launch_floor_gib',6.5), ('max_engines',6),
                ('affinity_mask','0xFFFFFFFF'), ('engine_memory_gb',20), ('ssh','localhost')]:
            with self.subTest(key=key), self.assertRaises(ValueError):
                box = local_profiles()[0]; box[key] = value
                remote.validate_profile(box, 'test')

    def test_local_outer_lease_checks_the_physical_free_guard_before_claim(self):
        import json
        import acceptance_box_lease as lease
        payload = dict(box=local_profiles()[0])
        with patch.object(Path,'read_text',return_value=json.dumps(payload)), \
                patch.object(lease.platform,'node',return_value='EROL-PC'), \
                patch.object(lease,'frozen_receipt',return_value={'frozen':True}), \
                patch.object(lease.cross,'assert_box_guard',side_effect=RuntimeError('physical free marker absent')), \
                patch.object(lease.cross,'acquire_reservation') as acquire, patch.object(lease,'write_json'):
            self.assertEqual(lease.hold('/virtual/payload.json'), 1)
        acquire.assert_not_called()

    def test_local_preflight_keeps_its_native_root_and_never_fetches_over_ssh(self):
        box = local_profiles()[0]
        root = Path('/virtual/run')
        box['payload_root'] = root.as_posix()
        plan = dict(run='run',boxes=[box])
        with patch.object(remote,'start_leases'), patch.object(remote,'stop_leases'), \
                patch.object(remote.cross,'command',return_value='') as command, \
                patch.object(remote,'read_json',return_value={'native':True}), patch.object(remote,'write_json'), \
                patch.object(remote,'validate_preflights'), patch.object(remote.world,'fetch_preserved') as fetch:
            remote.measure_preflight(plan,root)
        fetch.assert_not_called()
        self.assertEqual(command.call_args.args[0][-1], root.as_posix()+'/payload.json')
        self.assertEqual(command.call_args.args[0][0], box['python'])
        self.assertEqual(plan['preflights']['EROL-PC'], {'native':True})


if __name__ == '__main__': unittest.main()
