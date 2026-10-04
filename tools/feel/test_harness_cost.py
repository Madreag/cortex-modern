import json
from pathlib import Path
import tempfile
import unittest

from feel import report


def complete_cost_log(first=300, last=1200, costs=None, disabled=()):
    from feel.harness_cost import INSTRUMENTS
    scope = dict(version=2, process=44, incarnation=0, round=1, first_frame=first, last_frame=last,
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
        self.assertEqual(good['instrumentation']['max_frame_ms'], len(INSTRUMENTS))
        self.assertTrue(good['product_pass'])
        high = self.measured(complete_cost_log(costs={name: 7 for name in INSTRUMENTS}))
        self.assertFalse(high['instrument_valid'])
        self.assertTrue(high['product_pass'], 'invalid instrumentation is not a product failure')
        self.assertEqual(high['instrumentation']['max_frame_ms'], 7 * len(INSTRUMENTS))
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


def receipt_log(segments, *, round_id=1, enabled=('recorder', 'screen_watches'), closed=True, opened=False, outside=None, frame_ms=None, version=2):
    """Receipts as the engine writes them: per segment an optional opening receipt and outside-frame costs, its frames, its close."""
    from feel.harness_cost import VERSION_INSTRUMENTS
    instruments = {name: name in enabled for name in VERSION_INSTRUMENTS[version]}
    rows = []
    for segment, (first, last) in enumerate(segments):
        base = dict(process=44, incarnation=0, round=round_id, segment=segment)
        if opened:
            rows.append('[harness-cost-scope-open] ' + json.dumps(dict(base, version=version, first_frame=first, instruments=instruments)))
        if outside is not None:
            rows.append('[harness-cost-outside] ' + json.dumps(dict(base, before_frame=first, costs_ms={name: outside.get(name, 0) for name in enabled})))
        for frame in range(first, last + 1):
            costs = {name: (frame_ms or {}).get((frame, name), 1.0) for name in enabled}
            rows.append('[harness-cost-frame] ' + json.dumps(dict(base, frame=frame, partition_valid=True, costs_ms=costs)))
        if closed:
            rows.append('[harness-cost-scope] ' + json.dumps(dict(base, version=version, first_frame=first, last_frame=last, instruments=instruments)))
    return '\n'.join(rows) + '\n'


class HarnessCostReceipts(unittest.TestCase):
    def reduce(self, text, **window):
        from feel.harness_cost import reduce_costs
        with tempfile.TemporaryDirectory() as folder:
            log = Path(folder) / 'stdout.log'
            log.write_text(text)
            return reduce_costs([log], **window)

    def test_segments_of_one_round_are_separate_scopes(self):
        # A pause menu splits round 0 into two runs of frames; the menu's own stay is a round of its own.
        text = receipt_log([(1, 181), (182, 1200)], round_id=0) + receipt_log([(1, 40)], round_id=(1 << 62) | 1)
        result = self.reduce(text, first_frame=300, last_frame=1200)
        self.assertEqual(result['status'], 'PASS', result['reason'])
        self.assertEqual(result['measured_frames'], 1240)

    def test_window_must_be_covered_by_one_match_round(self):
        result = self.reduce(receipt_log([(1, 181), (183, 1200)], round_id=0), first_frame=300, last_frame=1200)
        self.assertEqual(result['status'], 'PASS', result['reason'])
        result = self.reduce(receipt_log([(1, 181), (183, 1200)], round_id=0), first_frame=1, last_frame=1200)
        self.assertEqual(result['status'], 'FAIL')
        self.assertIn('requested 1..1200', result['reason'])
        result = self.reduce(receipt_log([(1, 1200)], round_id=(1 << 62) | 1), first_frame=300, last_frame=1200)
        self.assertEqual(result['status'], 'FAIL', 'a menu stay is not the match window')

    def test_opening_receipt_owns_the_frames_of_a_process_ended_before_its_close(self):
        self.assertIn('no owning instrumentation scope', self.reduce(receipt_log([(1, 900)], closed=False))['reason'])
        result = self.reduce(receipt_log([(1, 900)], closed=False, opened=True))
        self.assertEqual(result['status'], 'PASS', result['reason'])
        self.assertFalse(result['scope_receipts'][0]['closed'])
        self.assertEqual(self.reduce(receipt_log([(1, 900)], opened=True))['status'], 'PASS')
        disagreeing = receipt_log([(1, 900)], opened=True).replace('"first_frame": 1, "last_frame"', '"first_frame": 2, "last_frame"')
        self.assertEqual(self.reduce(disagreeing)['status'], 'FAIL')

    def test_work_before_a_run_of_frames_is_listed_outside_every_frame(self):
        result = self.reduce(receipt_log([(1, 300)], opened=True, outside=dict(recorder=59.7)))
        self.assertEqual(result['status'], 'PASS', result['reason'])
        self.assertEqual(result['instruments']['recorder']['outside_frames_ms'], 59.7)
        self.assertEqual(result['max_frame_ms'], 2.0)
        self.assertTrue(any('59.700' in line for line in result['table']), result['table'])
        unowned = receipt_log([(1, 300)], opened=True, outside=dict(recorder=59.7)).replace('"before_frame": 1', '"before_frame": 2')
        self.assertIn('outside-frame costs have no owning scope', self.reduce(unowned)['reason'])
        extra = receipt_log([(1, 300)], opened=True, outside=dict(recorder=1)).replace('"costs_ms": {"recorder": 1, ', '"costs_ms": {"recorder": 1, "census": 0, ')
        self.assertIn('invalid outside-frame costs', self.reduce(extra)['reason'])

    def test_an_instrument_over_budget_is_named_with_its_number(self):
        result = self.reduce(receipt_log([(1, 300)], enabled=('stall_sampler', 'tick_end'), frame_ms={(22, 'stall_sampler'): 61.7}))
        self.assertEqual(result['status'], 'FAIL')
        self.assertIn('stall_sampler alone 61.7 ms', result['reason'])
        self.assertIn('peak frame 22 of round 1 = 62.7 ms (stall_sampler=61.7, tick_end=1)', result['reason'])
        self.assertEqual(result['over_budget_instruments'], {'stall_sampler': 61.7})

    def test_disabled_instrument_has_no_observations(self):
        quiet = self.reduce(receipt_log([(1, 300)]) + '[localpred] previews=354 actor_ticks=4820 ms_total=2142.5 avg_ms=6.05 known_copy_avg_ms=0\n')
        self.assertEqual(quiet['instruments']['preview_fidelity']['status'], 'DISABLED')
        self.assertEqual(quiet['status'], 'PASS', quiet['reason'])
        contradicted = self.reduce(receipt_log([(1, 300)]) + '[localpred] previews=354 harness_ms_total=0.305 harness_avg_ms=0.0009\n')
        self.assertIn('preview_fidelity declared disabled but native cost observations exist', contradicted['reason'])


    def test_version_one_receipts_still_read_and_version_two_names_every_instrument(self):
        old = self.reduce(receipt_log([(1, 300)], version=1))
        self.assertEqual(old['status'], 'PASS', old['reason'])
        self.assertEqual(old['instruments']['controller_trace']['status'], 'DISABLED')
        short = receipt_log([(1, 300)], version=1).replace('"version": 1', '"version": 2')
        self.assertIn('invalid or duplicate instrumentation scope', self.reduce(short)['reason'])
        traced = self.reduce(receipt_log([(1, 300)], enabled=('controller_trace', 'feel_recorder')))
        self.assertEqual(traced['instruments']['controller_trace']['frame_samples'], 300)


if __name__ == '__main__':
    unittest.main()
