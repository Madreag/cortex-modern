import json
from pathlib import Path
import tempfile
import unittest

from feel import report


def jitter_run(root):
    """A two-peer jitter arm whose logs carry a live delay change and a fake-link receipt on both peers."""
    manifest = dict(ticks=1200, jitter_ms=40, launches_complete=True)
    for peer, seat in (('host', 1), ('client', 2)):
        (root / peer).mkdir()
        row = dict(round=7, peer=seat, jitter_ms=40, jitter_packets=12)
        (root / peer / 'stdout.log').write_text(f'[net-lockstep] start round=7 frame=1 local_peer={seat} peers=2\n'
                                                f'[net-fake-link] {json.dumps(row)}\n'
                                                '[net-match] delay change peer=2 frame=600 delay=8 revision=2\n')
    (root / 'manifest.json').write_text(json.dumps(manifest))
    (root / 'host_report.json').write_text(json.dumps(dict(lockstep=dict(next_frame=1201, sim_tick_ms=1000 / 60,
        missing_frame_stalls=0, steady_missing_frame_stalls=0))))
    return [dict(type='committed', tick=tick, wall_ms=tick * 1000 / 60) for tick in (300, 1200)]


def paths_of(value, where='root'):
    if isinstance(value, Path):
        yield where
    elif isinstance(value, dict):
        for key, item in value.items():
            yield from paths_of(item, f'{where}.{key}')
    elif isinstance(value, (list, tuple)):
        for index, item in enumerate(value):
            yield from paths_of(item, f'{where}[{index}]')


class ReportSerialization(unittest.TestCase):
    def test_impairment_pin_is_json_native(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            result = report.item9a_gates(root, rows=jitter_run(root))
            pin = result['pins']['item9a_impairment_effects_and_resize']
            self.assertEqual(list(paths_of(result)), [])
            self.assertTrue(all(isinstance(path, str) for path in pin['value']['evidence']), pin['value']['evidence'])
            json.dumps(result, allow_nan=False)

    def test_completion_keeps_every_pin_when_one_value_is_not_json(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            completion = dict(measurement_complete=True, peers=dict(host=dict(pins=dict(
                good=dict(status='PASS', value=1, evidence=[root / 'host/stdout.log']),
                broken=dict(status='PASS', value=dict(evidence=[root / 'client/stdout.log'], handle=object())),
                ratio=dict(status='FAIL', value=float('nan'))))))
            report.write_json(root / 'completion.json', completion)
            written = json.loads((root / 'completion.json').read_text(encoding='utf-8'))
            pins = written['peers']['host']['pins']
            self.assertEqual(pins['good']['evidence'], [str(root / 'host/stdout.log')])
            self.assertEqual(pins['broken']['value']['evidence'], [str(root / 'client/stdout.log')])
            self.assertTrue(written['measurement_complete'])
            failed = written['serialization_failures']
            self.assertEqual([row['where'] for row in failed], ['peers.host.pins.broken.value.handle', 'peers.host.pins.ratio.value'])
            self.assertEqual(pins['broken']['value']['handle']['unserializable'], 'object')
            self.assertEqual(pins['ratio']['value']['unserializable'], 'float')

    def test_one_arm_failure_keeps_every_arm_verdict(self):
        import feel_measure
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for name in ('good-arm', 'bad-arm'):
                (root / name).mkdir()
            good = dict(name='good-arm', peers={}, measurement_complete=True, launches_complete=True, off_wire_pass=True,
                        item9a_pass=True, proof=dict(pair=dict(first_divergence=948)), report_path=str(root / 'good-arm/feel-report.json'))
            report.write_json(root / 'good-arm/feel-report.json', good)
            def broken():
                raise TypeError('Object of type WindowsPath is not JSON serializable')
            results = [feel_measure.reduce_arm(root / 'good-arm', 'good-arm', lambda: good),
                       feel_measure.reduce_arm(root / 'bad-arm', 'bad-arm', broken)]
            gates = feel_measure.write_case_gates(root, results)
            self.assertFalse(gates['passed'])
            for name in ('good-arm', 'bad-arm'):
                written = json.loads((root / name / 'feel-report.json').read_text(encoding='utf-8'))
                verdict = written['verdict']
                self.assertFalse(verdict['passed'])
                self.assertTrue(verdict['reasons'], verdict)
                self.assertIn('first_divergence', verdict)
            bad = json.loads((root / 'bad-arm/feel-report.json').read_text(encoding='utf-8'))['verdict']
            self.assertTrue(any('reduction failed: TypeError' in reason for reason in bad['reasons']), bad)
            self.assertEqual(json.loads((root / 'good-arm/feel-report.json').read_text(encoding='utf-8'))['verdict']['first_divergence'], 948)


if __name__ == '__main__':
    unittest.main()
