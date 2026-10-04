import json
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import acceptance_spectator_tasks as tasks


def box(name):
    if name=='Linux':
        root='/home/erol/cortex-workers/lane'
        return dict(name=name,ssh='3090',kind='posix-ssh',hostname='ubuntu',tree=root+'/repo',
                    executable=root+'/repo/build-gcc/CortexCommand',scratch=root,helpers=root+'/helpers',
                    exclusive_marker=root+'/FEEL-MATRIX-RUNNING',acceptance_marker='/home/erol/cortex-workers/ACCEPTANCE-STREAM-RUNNING',
                    peers_per_box=1,ports=[50310,50339])
    if name=='EROL-PC':
        from test_acceptance_local_host import local_profiles
        value=local_profiles('world-join')[0]
        value.update(scratch='D:/mx/lane',helpers='D:/mx/lane/helpers',peers_per_box=2)
        return value
    tree='D:/mx/lane/engine-tip'
    return dict(name=name, ssh='z13' if name=='EROL-PC' else 'edith', kind='windows-task', hostname='EROL-TABLET' if name=='EROL-PC' else 'EDITH',
                tree=tree,executable=tree+'/Cortex Command.exe',scratch='D:/mx/lane',helpers='D:/mx/lane/helpers',
                exclusive_marker='D:/mx/FEEL-MATRIX-RUNNING',runner='cortex-session1',task_script='D:/mx/session1/run.ps1',
                launch_floor_gib=6.5 if name=='EROL-PC' else 10,peers_per_box=2,ports=[49120,49149])


class SpectatorTaskBoundaries(unittest.TestCase):
    def test_note12_three_box_placement_obeys_each_native_capacity(self):
        tasks.validate(box('EROL-PC'),'lane',('host','seated-one'))
        tasks.validate(box('EDITH'),'lane',('seated-two','seated-three'))
        tasks.validate(box('Linux'),'lane',('spectator',))
        with self.assertRaises(ValueError): tasks.validate(box('EDITH'),'lane',('seated-one','seated-two','spectator'))

    def test_wrong_physical_host_is_refused_before_engine_or_file_operations(self):
        with patch.object(tasks.platform,'node',return_value='EROL-TABLET'):
            with self.assertRaises(ValueError): tasks.validate(box('EROL-PC'),'lane',('host','seated-one'),native=True)

    def test_acceptance_and_engineer_trees_cannot_be_substituted(self):
        for path in ('D:/Projects/z13-build','D:/Projects/z13-dev-build'):
            value={**box('EROL-PC'),'tree':path,'executable':path+'/Cortex Command.exe'}
            with self.assertRaises(ValueError): tasks.validate(value,'lane',('host','seated-one'))

    def test_probes_obey_native_timeout_and_require_host_release(self):
        root=Path(tasks.__file__).parent/'e2e'
        for name in ('host','leave','join'):
            value=json.loads((root/f'world-spectator.{name}.probe.json').read_text())
            self.assertLessEqual(value['timeout_ms'],180000)
        host=json.loads((root/'world-spectator.host.probe.json').read_text())
        self.assertEqual([s['stable_seat'] for s in host['steps'] if s['op']=='remove_participant'],[1])
        self.assertTrue(any(s.get('name')=='done' for s in host['steps']))

    def test_native_probes_bracket_the_crawl_before_allowing_departure(self):
        for peer in tasks.PEERS:
            captured=[]
            with patch.object(Path,'mkdir'), patch.object(tasks,'write_json',side_effect=lambda path,value:captured.append(value)):
                tasks.staged_probe(Path('/virtual'),peer)
            probe=captured[0]
            steps=probe['steps']
            if peer in tasks.SEATED:
                commands=[step.get('command') for step in steps]
                start=commands.index('dump_world_ownership crawl-start')
                end=commands.index('dump_world_ownership crawl-end')
                self.assertLess(start,end)
                self.assertTrue(steps[start-1]['path'].endswith('timing-start.json'))
                self.assertTrue(steps[end-1]['path'].endswith('spectator-stage/probe/crawl-complete.json'))
                if peer=='seated-one':
                    self.assertTrue(steps[end+1]['path'].endswith('timing-complete.json'))
                    self.assertLess(end,commands.index('dump_world_ownership departing-seat'))
            if peer=='spectator':
                commands=[step.get('command') for step in steps]
                armed=commands.index('dump_world_ownership crawl-armed')
                self.assertTrue(steps[armed-1]['path'].endswith('timing-begun.json'))
                self.assertLess(armed,commands.index('dump_world_ownership crawl-complete'))


if __name__=='__main__': unittest.main()
