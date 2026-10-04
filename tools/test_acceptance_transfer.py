import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import world_mod_cross as world


def native_probe(total=123):
    records = []
    for at, received, size, service in ((0, 0, 0, 'Joining'), (1000, 1, total, 'Joining'),
                                       (2000, 80, total, 'Joining'), (2500, 0, 0, 'Loading'),
                                       (3500, 0, 0, 'Running')):
        records.append(dict(at_ms=at, why='boundary', screen='MultiplayerScreen', service=service,
                            transfer=dict(received_bytes=received, total_bytes=size),
                            lines=[dict(source='menu', control='status', text='native visible text')]))
    return dict(schema=1, pid=42, complete=True, **{'pass': True}, label_dumps=records)


def native_log(probe, total=123):
    lines = []
    for index, record in enumerate(probe['label_dumps']):
        if index == 3:
            lines.append(f'[net-match] state transfer complete: {total} bytes')
        transfer = record['transfer']
        lines.append(f'[net-ui-probe] label dump at_ms={record["at_ms"]} screen={record["screen"]} '
                     f'service={record["service"]} transfer={transfer["received_bytes"]}/{transfer["total_bytes"]}'
                     + (' boundary' if record['why'] == 'boundary' else ''))
    return '\n'.join(lines)+'\n'


class TransferStaging(unittest.TestCase):
    def test_late_world_join_enables_native_label_dump_before_activation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root/'engine').mkdir()
            run = SimpleNamespace(env={}, record={})
            spec = dict(acceptance_row='world-join', peer='edith', defer_until_session=True, own=str(root))
            with patch('feel_measure.private_settings'):
                world.stage_activity(run, spec)
            self.assertIn('CC_TEST_NET_UI_SCRIPT', run.env)
            script = json.loads(Path(run.env['CC_TEST_NET_UI_SCRIPT']).read_text())
            self.assertEqual(script['label_dump']['every_ms'], 1000)
            self.assertFalse(script.get('repeat_rounds', False))
            self.assertFalse(any(step['op'] in ('assert_buy', 'assert_pie') for step in script['steps']))
            self.assertEqual(run.record['env_set']['CC_TEST_NET_UI_SCRIPT'], run.env['CC_TEST_NET_UI_SCRIPT'])

    def test_soak_does_not_add_the_r3_label_probe(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root/'engine').mkdir()
            run = SimpleNamespace(env={}, record={})
            with patch('feel_measure.private_settings'):
                world.stage_activity(run, dict(acceptance_row='world-soak', peer='edith',
                                              defer_until_session=True, own=str(root)))
            self.assertNotIn('CC_TEST_NET_UI_SCRIPT', run.env)


class TransferReceipts(unittest.TestCase):
    def collect(self, probe=None, log=None, pid=42):
        from acceptance_transfer import native_transfer
        probe = native_probe() if probe is None else probe
        return native_transfer(probe, native_log(probe) if log is None else log, pid)

    def test_native_boundaries_keep_their_clock_and_sampling_uncertainty(self):
        result = self.collect()
        self.assertEqual((result['start_ms'], result['end_ms'], result['received_bytes']), (1000, 2500, 123))
        self.assertEqual(result['elapsed_s'], 1.5)
        self.assertEqual(result['native_progress_duration_bounds_s'], [1.0, 2.5])
        self.assertEqual([r['at_ms'] for r in result['labels']], [0, 1000, 2000, 2500])
        self.assertEqual(result['labels'][1]['texts'], ['native visible text'])

    def test_wrong_pid_and_unfinished_probe_are_refused(self):
        for change in ({'pid': 43}, {'complete': False}, {'pass': False}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.collect({**native_probe(), **change})

    def test_native_bytes_and_order_must_agree(self):
        probe = native_probe()
        for log in (native_log(probe, 124), native_log(probe).replace('123 bytes', '0 bytes'),
                    native_log(probe)+'[net-match] state transfer complete: 123 bytes\n',
                    native_log(probe).replace('[net-match] state transfer complete: 123 bytes\n', '')+
                        '[net-match] state transfer complete: 123 bytes\n'):
            with self.subTest(log=log), self.assertRaises(ValueError):
                self.collect(probe, log)

    def test_missing_clock_boundary_and_sparse_or_changed_label_evidence_are_refused(self):
        for change in ('missing-start', 'missing-end', 'clock-gap', 'duplicate-time', 'changed-log', 'changed-total', 'backward-bytes'):
            with self.subTest(change=change):
                probe = native_probe()
                records = probe['label_dumps']
                if change == 'missing-start': records[1]['transfer'] = dict(received_bytes=0,total_bytes=0); records[2]['transfer'] = dict(received_bytes=0,total_bytes=0)
                elif change == 'missing-end': del records[3:]
                elif change == 'clock-gap': records[2]['at_ms'] = 8000
                elif change == 'duplicate-time': records[2]['at_ms'] = 1000
                elif change == 'changed-total': records[2]['transfer']['total_bytes'] = 124
                elif change == 'backward-bytes': records[2]['transfer']['received_bytes'] = 0
                log = native_log(probe)
                if change == 'changed-log': log = log.replace('at_ms=1000', 'at_ms=1001')
                with self.assertRaises(ValueError): self.collect(probe,log)


if __name__ == '__main__':
    unittest.main()
