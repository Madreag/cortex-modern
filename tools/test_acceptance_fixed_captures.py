import os
from pathlib import Path
import tempfile
import unittest

from acceptance_fixed_gates import capture_evidence


class NativeCaptureRequirements(unittest.TestCase):
    def setUp(self):
        retained = os.environ.get('CC_ACCEPTANCE_TEST_ROOT')
        if retained:
            self.root = Path(tempfile.mkdtemp(prefix='acceptance-captures-', dir=retained))
        else:
            temporary = tempfile.TemporaryDirectory()
            self.addCleanup(temporary.cleanup)
            self.root = Path(temporary.name)
        self.names = ['erol', 'edith']
        self.inputs = dict(manifest=dict(acceptance_row='mod-match'), peers=dict.fromkeys(self.names, {}),
                           paths={name:self.root/name for name in self.names},
                           live=dict(erol=[dict(tick=t, round=1, phase='live', gameplay_tick=True, effective_start_frame=1)
                                           for t in range(1, 182)]))
        for name in self.names:
            path = self.root/name/'engine/stdout.log'
            path.parent.mkdir(parents=True)
            path.write_text('[net-lockstep] start round=1 frame=1 local_peer=1 peers=2\n'
                            + ''.join(self.sample(t) for t in (1, 60, 120, 180)), encoding='utf-8')

    def sample(self, tick, label='sample'):
        tag = 'fullstate' if label == 'sample' else 'fullstate-'+label
        return (f'[fullstate-context] tick={tick} round=1 label={label} path=/run/process-1/round-1/capture-{tick}/{label}\n'
                f'[{tag}] tick={tick} hash=0123456789abcdef sections=header:0123456789abcdef,scene:0123456789abcdef round=1\n'
                f'[fullstate-scope] tick={tick} round=1 label={label} per_peer=camera\n')

    def append(self, name, text):
        with (self.root/name/'engine/stdout.log').open('a', encoding='utf-8') as stream:
            stream.write(text)

    def judge(self, first=1):
        return capture_evidence(self.inputs, [(self.names, first, 181)], 60)

    def test_complete_native_samples_are_comparable(self):
        result = self.judge()
        self.assertTrue(result['passed'], result)

    def test_missing_landed_result_stays_owed_even_when_nobody_wrote_it(self):
        for name in self.names:
            self.append(name, '[net-lockstep] return of peer 2 at 90 delay=4 neutral_through=100 revision=2 incarnation=1\n')
        result = self.judge()
        self.assertFalse(result['passed'])
        self.assertIn((1, 160, 'landed'), result['comparisons'][0]['obligations']['expected'])

    def test_announced_image_cannot_vanish_without_a_writer_result(self):
        self.append('erol', '[fullstate-context] tick=125 round=1 label=canonical path=/run/process-1/round-1/capture-125/canonical\n')
        result = self.judge()
        self.assertFalse(result['passed'])
        self.assertEqual(result['comparisons'][0]['missing_announced_captures'][0]['key'], (1, 125, 'canonical'))

    def test_late_world_requires_a_compared_restored_image(self):
        self.inputs['manifest']['acceptance_row'] = 'world-join'
        self.assertFalse(self.judge(121)['passed'])
        self.append('erol', self.sample(100, 'canonical'))
        self.append('edith', self.sample(100, 'restored'))
        self.assertTrue(self.judge(121)['passed'])
        self.append('edith', self.sample(100, 'restored').replace('scene:0123456789abcdef', 'scene:ffffffffffffffff'))
        self.assertFalse(self.judge(121)['passed'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
