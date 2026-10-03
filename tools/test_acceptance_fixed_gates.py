from copy import deepcopy
import unittest
from unittest.mock import patch

from acceptance_fixed_gates import evaluate, identity
from feel.test_attempt_requirements import attempt


def fixture():
    manifest, _, peers, _, _ = attempt()
    manifest.update(acceptance_row='mod-match', scenario='match', ticks=1201, faults=[], fullstate_every=60)
    for name, peer in peers.items():
        peer.update(native_final_tick=1201, native_completion=dict(completion='completed'),
                    instrument_valid=True, presentation_valid=True, tick_timing_valid=True)
        peer['record']['started'] = True
    events = {name:[dict(type='progress', budget_tick=1201), dict(type='tick_timing', partition_valid=True)] for name in peers}
    return dict(manifest=manifest, peers=peers, events=events, paths={}, live={}, findings=[], mixed_builds=[],
                capabilities={box['name']:dict(peer_limit=4) for box in manifest['boxes']})


class FixedAcceptanceEvidence(unittest.TestCase):
    def test_checkout_head_is_diagnostic_when_the_measured_build_matches(self):
        value = fixture()
        value['manifest']['preflights']['EDITH']['head'] = 'f'*40
        self.assertTrue(identity(value['manifest'], value['peers'])['passed'])

    def test_world_soak_binds_the_authorized_remote_host_box(self):
        value = fixture()
        manifest = value['manifest']
        manifest.update(acceptance_row='world-soak', world_host_box='Z13')
        manifest['instances'] = [dict(name='erol', box='Z13'), dict(name='edith-first', box='EDITH'), dict(name='edith', box='EDITH')]
        manifest['specs'] = [dict(peer='erol', box='Z13', role='host'), dict(peer='edith-first', box='EDITH', role='player'), dict(peer='edith', box='EDITH', role='player')]
        manifest['boxes'] = [dict(name='Z13'), dict(name='EDITH')]
        manifest['preflights']['Z13'] = manifest['preflights'].pop('EROL-PC')
        value['peers'] = {name:peer for name,peer in value['peers'].items() if name in ('erol','edith')}
        value['peers']['edith-first'] = deepcopy(value['peers']['edith'])
        self.assertTrue(identity(manifest, value['peers'])['passed'])
        manifest['specs'][0]['box'] = 'EROL-PC'
        self.assertFalse(identity(manifest, value['peers'])['passed'])

    def judge(self, value):
        with patch('acceptance_fixed_gates.capture_evidence', return_value=dict(passed=True)):
            return evaluate(value, {})

    def test_every_fixed_gate_remains_required_for_the_mod_match(self):
        self.assertTrue(self.judge(fixture())['passed'])
        for name, field in (('instrumentation', 'instrument_valid'), ('quiet_feel', 'feel_gated'), ('record_integrity', 'presentation_valid')):
            with self.subTest(gate=name):
                value = fixture()
                value['peers']['mac'][field] = False
                result = self.judge(value)
                self.assertFalse(result['passed'])
                self.assertFalse(result['checks'][name])

    def test_configured_workload_is_not_measured_workload(self):
        value = fixture()
        value['events']['linux'][0]['type'] = 'configured'
        self.assertFalse(self.judge(value)['checks']['measured_workload'])

    def test_missing_census_or_raw_bounds_cannot_pass(self):
        for field in ('memory_census', 'memory_by_incarnation'):
            with self.subTest(field=field):
                value = fixture()
                value['peers']['edith'][field] = {}
                self.assertFalse(self.judge(value)['checks']['memory_bounds'])

    def test_box_roles_and_build_receipts_are_bound(self):
        for defect in ('role', 'box', 'build', 'runner'):
            with self.subTest(defect=defect):
                value = fixture()
                if defect == 'role': value['manifest']['specs'][1]['role'] = 'host'
                if defect == 'box': value['manifest']['instances'][1]['box'] = 'EROL-PC'
                if defect == 'build': value['manifest']['preflights']['Mac']['build']['commit'] = 'c'*40
                if defect == 'runner': value['peers']['mac']['record']['exe_sha256'] = 'c'*64
                self.assertFalse(identity(value['manifest'], value['peers'])['passed'])

    def test_soak_uses_its_explicit_two_box_three_process_contract(self):
        value = fixture()
        manifest = value['manifest']
        manifest['acceptance_row'] = 'world-soak'
        for key in ('instances', 'specs'):
            manifest[key] = [entry for entry in manifest[key] if entry.get('name', entry.get('peer')) in ('erol','edith')]
        manifest['instances'].append(dict(name='edith-first', box='EDITH'))
        manifest['specs'].append(dict(peer='edith-first', box='EDITH', role='player'))
        manifest['boxes'] = [entry for entry in manifest['boxes'] if entry['name'] in ('EROL-PC','EDITH')]
        value['peers'] = {name:peer for name,peer in value['peers'].items() if name in ('erol','edith')}
        value['peers']['edith-first'] = deepcopy(value['peers']['edith'])
        self.assertTrue(identity(manifest, value['peers'])['passed'])
        value['peers'].pop('edith-first')
        self.assertFalse(identity(manifest, value['peers'])['passed'])

    def test_capture_failure_cannot_hide_behind_passing_periodic_hashes(self):
        with patch('acceptance_fixed_gates.capture_evidence', return_value=dict(passed=False)):
            result = evaluate(fixture(), {})
        self.assertFalse(result['passed'])
        self.assertFalse(result['checks']['complete_captures'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
