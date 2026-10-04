"""A driver-only acceptance row reads the public directory default from the coordinator's own tree."""
import contextlib
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

import cross_peers
import edith_cross
import test_directory_ice_join as directory
import world_mod_cross
from edith.remote_box import RemoteBox

REPO = Path(__file__).resolve().parents[1]


class DriverOnlyPublicRow(unittest.TestCase):
    def test_a_driver_only_row_checks_the_directory_from_the_coordinator_tree(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            manifest = json.loads((REPO/'tools/cross_peers/boxes.json').read_text())
            box = next(box for box in manifest['boxes'] if box['name'] == 'EROL-PC')
            box.update(name='Z13', kind='windows-task', ssh='z13', runner='cortex-session1', task_script='D:/mx/session1/run.ps1',
                       directory_port=49985)
            box.pop('guard_file', None); box.pop('exclusive_marker', None)
            for peer in manifest['instances']:
                if peer['box'] == 'EROL-PC': peer.update(box='Z13', name='z13')
            manifest['driver'] = dict(name='EROL-PC', kind='coordinator', directory_port=49918)
            boxes = root/'boxes.json'; boxes.write_text(json.dumps(manifest))
            options = cross_peers.parse_args(['--boxes', str(boxes), '--host', 'z13', '--scenario', 'match', '--roster', 'four-way',
                '--out', str(root/'run'), '--lane', 'unit', '--mac-guard', str(root/'mac-ready'), '--dry-run'])
            with patch.dict(os.environ, CC_CROSS_PEERS_LANE='unit', CC_CROSS_PEERS_MAC_GUARD=str(root/'mac-ready')):
                plan = cross_peers.make_plan(options)
            self.assertFalse(any(box['kind'] == 'windows-local' for box in plan['boxes']))
            plan['acceptance_row'] = 'world-join'
            plan['driver_sources'] = {name: 'd'*64 for name in world_mod_cross.DRIVER_FILES}
            asked = []
            class LaunchObserved(BaseException): pass
            def command(argv, **kwargs):
                if argv[0] == 'scp' and 'preflight.json' in str(argv[-1]):
                    target = Path(argv[-1]); name = target.parent.name
                    target.write_text(json.dumps(dict(machine_id=name, content={}, modules={}, fixture={}, executable_sha256='b'*64,
                        build=dict(commit=plan['driver_commit'], executable_sha256='b'*64),
                        acceptance_driver_sources=plan['driver_sources'])))
                if any('Get-ScheduledTask' in str(arg) for arg in argv): return 'Ready'
                if any('Start-ScheduledTask' in str(arg) for arg in argv): raise LaunchObserved()
                return ''
            def available(repo):
                asked.append(Path(repo))
                return False
            process = Mock(); process.poll.return_value = None
            with patch.object(cross_peers, 'SCRATCH', root), patch.object(cross_peers, 'command', side_effect=command), \
                 patch.object(cross_peers, 'stage_remote'), patch.object(cross_peers, 'remote_exists', return_value=True), \
                 patch.object(cross_peers, 'inventory_guard', return_value='USER-AT-DESK'), \
                 patch.object(cross_peers, 'acquire_reservation', side_effect=AssertionError('PC game reservation requested')), \
                 patch.object(cross_peers.subprocess, 'Popen', return_value=process), patch.object(cross_peers.time, 'sleep'), \
                 patch.object(world_mod_cross, 'public_directory_available', side_effect=available), \
                 patch.object(RemoteBox, 'start_task', side_effect=LaunchObserved()), \
                 patch.object(edith_cross, 'make_cert', return_value=(root/'cert', root/'key', 'pin')), \
                 patch.object(directory, 'start_service', return_value=process), contextlib.redirect_stdout(io.StringIO()) as printed:
                try:
                    cross_peers.run_plan(plan, root/'run')
                except LaunchObserved:
                    pass
                else:
                    self.fail('the row stopped before any launch: ' + printed.getvalue().strip()[-300:])
            self.assertEqual(asked, [cross_peers.HERE.parent])


if __name__ == '__main__':
    unittest.main()
