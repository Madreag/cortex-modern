"""A recorded brain loss permits spectating, never a lost host, stalled fight or missing minute."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

import e2e_video as video


class BrainLossEvidenceTest(unittest.TestCase):
    def setUp(self):
        self.spec = dict(count=20, minimum_tps=58.0, allow_brain_loss=True)
        self.sample = dict(at_ms=60000, lockstep_frame=3600, service='Running', paused=False, editing=False,
                           local_actor_alive=False, spectating=True,
                           brain_losses=[dict(player=0, uid=123, tick=2400, local=True, outcome='spectating')],
                           minute=dict(number=1, elapsed_ms=60000, pace=60))

    def verdict(self, sample=None, spec=None):
        document = dict(steps=[dict(op='measure_minute', observed=sample or self.sample)])
        return video.minute_intervals(document, spec or self.spec)[0]['pass']

    def test_confirmed_loss_keeps_the_full_minute(self):
        self.assertTrue(self.verdict())

    def test_legacy_scenes_keep_the_alive_requirement(self):
        self.assertFalse(self.verdict(spec=dict(count=20, minimum_tps=58)))

    def test_unrecorded_remote_and_future_losses_do_not_excuse_a_dead_local_actor(self):
        for losses in ([], [dict(local=False, tick=2400)], [dict(local=True, tick=3601)]):
            sample = copy.deepcopy(self.sample)
            sample['brain_losses'] = losses
            self.assertFalse(self.verdict(sample), losses)

    def test_losses_do_not_excuse_stops_or_slow_ticks(self):
        for field, value in [('service', 'HostLost'), ('paused', True), ('editing', True), ('spectating', False)]:
            sample = copy.deepcopy(self.sample)
            sample[field] = value
            self.assertFalse(self.verdict(sample), (field, value))
        sample = copy.deepcopy(self.sample)
        sample['minute']['pace'] = 57
        self.assertFalse(self.verdict(sample))

    def test_twenty_full_minutes_are_still_required(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'probe.json'
            steps = []
            for minute in range(1, 21):
                sample = copy.deepcopy(self.sample)
                sample.update(at_ms=minute*60000, lockstep_frame=minute*3600)
                sample['minute']['number'] = minute
                steps.append(dict(op='measure_minute', observed=sample))
            path.write_text(json.dumps(dict(steps=steps)))
            self.assertTrue(video.continuous_minute_evidence(path, self.spec)['pass'])
            path.write_text(json.dumps(dict(steps=steps[:-1])))
            self.assertFalse(video.continuous_minute_evidence(path, self.spec)['pass'])

    def test_scene_policy_keeps_duration_and_rendezvous(self):
        for name in ('fight15-s10', 'fight15-s11', 'fight15-s12'):
            scene = json.loads((video.SCENARIO_DIR / (name+'.json')).read_text())
            self.assertEqual(scene['fight_hash_gate']['running_ticks'], 72000)
            for role in scene['fight_hash_gate']['peers']:
                probe = json.loads(scene['scripts'][role+'.probe'])
                self.assertEqual(probe['brain_loss_policy'], 'spectate')
                self.assertEqual(sum(s['op']=='measure_minute' for s in probe['steps']), 20)
                for step in probe['steps']:
                    if step['op'] in ('signal','wait_file','measure_minute','finish') or 'lockstep_frame_at_least' in step:
                        self.assertNotIn('skip_if_brain_lost', step)
        scene = json.loads((video.SCENARIO_DIR / 'fight15-s13.json').read_text())
        for text in (s for k,s in scene['scripts'].items() if k.endswith('.probe')):
            self.assertNotIn('brain_loss_policy', json.loads(text))
            self.assertNotIn('skip_if_brain_lost', text)


if __name__ == '__main__':
    unittest.main()
