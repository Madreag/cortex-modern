import contextlib
import copy
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import feel_measure


class MatrixPredicate(unittest.TestCase):
    def test_p09_main_matrix_rejects_failed_proof_and_missing_input_response(self):
        pins = dict(item9a_wall_tps=dict(status='PASS', value=60),
                    item9a_missing_frame_stalls=dict(status='PASS', value=0),
                    input_carried=dict(status='FAIL', value=99), input_response=dict(status='MISS', value=None))
        result = dict(name='100ms-60hz', peers={'host': dict(pins=pins)}, launches_complete=True,
                      off_wire_pass=False, item9a_pass=True, measurement_complete=False)
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'one').mkdir()
            (root / 'one/manifest.json').write_text(json.dumps(dict(launches_complete=True)))
            (root / 'matrix-plan.json').write_text(json.dumps(dict(arms=['one'], matrix_group='all')))
            with patch.object(feel_measure, 'analyze', return_value=[result]), contextlib.redirect_stdout(io.StringIO()):
                code = feel_measure.main(['--out', folder, '--analyze-only', '--skip-gates',
                                          '--port', '49700', '--port-block', '49700-49709'])
            completion = json.loads((root / 'completion.json').read_text())
        self.assertEqual((code, completion['measurement_complete']), (1, False), completion)

    def test_each_product_conjunct_is_required(self):
        pins = {name: dict(status='PASS', value=0) for name in
                ('item9a_wall_tps', 'item9a_missing_frame_stalls', 'input_carried', 'input_response')}
        good = dict(name='measured', peers={'host': dict(pins=pins)}, launches_complete=True,
                    off_wire_pass=True, item9a_pass=True)
        self.assertTrue(feel_measure.product_predicate(good)['passed'])
        for field in ('launches_complete', 'off_wire_pass'):
            for value in (False, None):
                with self.subTest(field=field, value=value):
                    bad = dict(good, **{field: value})
                    self.assertFalse(feel_measure.product_predicate(bad)['passed'])
        for name in pins:
            for status in ('FAIL', 'MISS'):
                with self.subTest(pin=name, status=status):
                    bad = copy.deepcopy(good)
                    bad['peers']['host']['pins'][name]['status'] = status
                    self.assertFalse(feel_measure.product_predicate(bad)['passed'])
        absent = copy.deepcopy(good)
        del absent['peers']['host']['pins']['input_response']
        self.assertFalse(feel_measure.product_predicate(absent)['passed'])

    def test_timing_control_requires_its_paired_input_measurement(self):
        pins = {name: dict(status='PASS', value=0) for name in
                ('item9a_wall_tps', 'item9a_missing_frame_stalls', 'input_carried', 'input_response')}
        case = dict(name='paired', peers={'host': dict(pins=pins), 'host_off': dict(
            pins={name: value for name, value in pins.items() if name.startswith('item9a_')}, timing_control_for='host')},
            launches_complete=True, off_wire_pass=True, item9a_pass=True)
        self.assertTrue(feel_measure.product_predicate(case)['passed'])
        case['peers']['host']['pins']['input_response']['status'] = 'MISS'
        self.assertFalse(feel_measure.product_predicate(case)['passed'])


if __name__ == '__main__':
    unittest.main()
