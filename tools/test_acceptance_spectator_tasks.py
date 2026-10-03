import json
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import acceptance_spectator_tasks as tasks


def box(name):
    tree='D:/Projects/z13-rows-build' if name=='Z13' else 'D:/mx/lane/engine-tip'
    return dict(name=name, ssh='z13' if name=='Z13' else 'edith', kind='windows-task', hostname='EROL-TABLET' if name=='Z13' else 'EDITH',
                tree=tree,executable=tree+'/Cortex Command.exe',scratch='D:/mx/lane',helpers='D:/mx/lane/helpers',
                exclusive_marker='D:/mx/FEEL-MATRIX-RUNNING',runner='cortex-session1',task_script='D:/mx/session1/run.ps1',
                launch_floor_gib=6.5 if name=='Z13' else 10,peers_per_box=1 if name=='Z13' else 4)


class SpectatorTaskBoundaries(unittest.TestCase):
    def test_all_seated_and_watcher_processes_share_edith_clock(self):
        tasks.validate(box('Z13'),'lane',('host',))
        tasks.validate(box('EDITH'),'lane',(*tasks.SEATED,'spectator'))
        with self.assertRaises(ValueError): tasks.validate(box('EDITH'),'lane',('seated-one','seated-two','spectator'))

    def test_physical_pc_is_refused_before_engine_or_file_operations(self):
        with patch.object(tasks.platform,'node',return_value='EROL-PC'):
            with self.assertRaises(ValueError): tasks.validate(box('Z13'),'lane',('host',),native=True)

    def test_acceptance_and_engineer_trees_cannot_be_substituted(self):
        for path in ('D:/Projects/z13-build','D:/Projects/z13-dev-build'):
            value={**box('Z13'),'tree':path,'executable':path+'/Cortex Command.exe'}
            with self.assertRaises(ValueError): tasks.validate(value,'lane',('host',))

    def test_probes_obey_native_timeout_and_require_host_release(self):
        root=Path(tasks.__file__).parent/'e2e'
        for name in ('host','leave','join'):
            value=json.loads((root/f'world-spectator.{name}.probe.json').read_text())
            self.assertLessEqual(value['timeout_ms'],180000)
        host=json.loads((root/'world-spectator.host.probe.json').read_text())
        self.assertEqual([s['stable_seat'] for s in host['steps'] if s['op']=='remove_participant'],[1])
        self.assertTrue(any(s.get('name')=='done' for s in host['steps']))


if __name__=='__main__': unittest.main()
