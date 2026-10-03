import json
from pathlib import Path
import tempfile
import unittest

from feel import report


def complete_cost_log(first=300, last=1200, costs=None, disabled=()):
    from feel.harness_cost import INSTRUMENTS
    scope = dict(version=1, process=44, incarnation=0, round=1, first_frame=first, last_frame=last,
                 instruments={name: name not in disabled for name in INSTRUMENTS})
    rows = ['[harness-cost-scope] ' + json.dumps(scope)]
    for frame in range(first, last + 1):
        values = {name: (costs or {}).get(name, 0) if frame == first else 0 for name in INSTRUMENTS if name not in disabled}
        rows.append('[harness-cost-frame] ' + json.dumps(dict(process=44, incarnation=0, round=1, frame=frame,
                     partition_valid=True, costs_ms=values)))
    return '\n'.join(rows) + '\n'


class HarnessCostEvidence(unittest.TestCase):
    def measured(self, log):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'host').mkdir()
            (root / 'host/stdout.log').write_text(log)
            (root / 'manifest.json').write_text(json.dumps(dict(ticks=1200)))
            (root / 'host_report.json').write_text(json.dumps(dict(runner=dict(lockstep=dict(
                next_frame=1201, missing_frame_stalls=0, steady_missing_frame_stalls=0, sim_tick_ms=1000 / 60)))))
            return report.item9a_gates(root, rows=[dict(type='committed', tick=300, wall_ms=5000),
                                                  dict(type='committed', tick=1200, wall_ms=20000)])

    def test_absent_instrument_cost_cannot_be_zero(self):
        result = self.measured('')
        self.assertNotEqual(result['pins']['item9a_harness_cost']['status'], 'PASS', result)

    def test_census_cost_is_not_hidden_by_cheap_sim_dump(self):
        result = self.measured('[sim-dump] ticks=600 mean_ms=0.1 max_ms=0.2\n'
                               '[mem-census] tick=600 census_us=lua:71000,cow:1\n')
        self.assertNotEqual(result['pins']['item9a_harness_cost']['status'], 'PASS', result)

    def test_all_cost_components_and_aggregate_bound(self):
        from feel.harness_cost import INSTRUMENTS
        good = self.measured(complete_cost_log(costs={name: 1 for name in INSTRUMENTS}))
        self.assertTrue(good['instrument_valid'], good)
        self.assertEqual(good['instrumentation']['max_frame_ms'], 8)
        self.assertTrue(good['product_pass'])
        high = self.measured(complete_cost_log(costs={name: 7 for name in INSTRUMENTS}))
        self.assertFalse(high['instrument_valid'])
        self.assertTrue(high['product_pass'], 'invalid instrumentation is not a product failure')
        self.assertEqual(high['instrumentation']['max_frame_ms'], 56)
        for name in INSTRUMENTS:
            with self.subTest(instrument=name):
                log = complete_cost_log().replace('"' + name + '": 0', '"missing_' + name + '": 0')
                self.assertFalse(self.measured(log)['instrument_valid'])

    def test_native_summaries_remain_named_partial_evidence(self):
        log = ('[harness-cost] tick=600 window=600 tick_end_us p50=1 p95=2 max=3000\n'
               '[fullstate-cost] tick=600 freeze_us=1000 hash_us=2000 image_bytes=4096\n'
               '[mem-census] tick=600 census_us=lua:100,cow:200\n'
               '[prediction] previews=42 harness_ms_total=18.5 harness_avg_ms=0.4\n')
        result = self.measured(log)['instrumentation']
        self.assertFalse(result['passed'])
        self.assertEqual(result['instruments']['tick_end']['measured_max_ms'], 3)
        self.assertEqual(result['instruments']['census']['native_samples'][0]['total_ms'], .30000000000000004)
        self.assertEqual(result['instruments']['preview_fidelity']['native_samples'][0]['cumulative_total_ms'], 18.5)

    def test_frame_partition_cannot_understate_native_cost(self):
        from feel.harness_cost import INSTRUMENTS
        log = complete_cost_log(costs={name: 6 for name in INSTRUMENTS})
        log += '[fullstate-cost] tick=300 freeze_us=10000 hash_us=0 image_bytes=4096\n'
        self.assertFalse(self.measured(log)['instrument_valid'])

    def test_missing_frame_wrong_process_and_disabled_contradiction(self):
        valid = complete_cost_log()
        for broken in (valid.rsplit('\n', 2)[0], valid.replace('"process": 44', '"process": 45', 1),
                       complete_cost_log(disabled=('sim_dump',)) + '[sim-dump] ticks=1200 mean_ms=1 max_ms=1\n'):
            self.assertFalse(self.measured(broken)['instrument_valid'])


if __name__ == '__main__':
    unittest.main()
