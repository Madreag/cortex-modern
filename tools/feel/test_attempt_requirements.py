"""Required acceptance evidence must survive the cross report's final predicate."""
import copy
import unittest

import cross_report


def attempt():
    names = dict(erol='EROL-PC', edith='EDITH', mac='Mac', linux='Linux')
    manifest = dict(scenario='soak', acceptance_row=18, source_sha='a'*40, host='erol', ticks=72000, fullstate_every=600,
                    boxes=[dict(name=box) for box in names.values()], instances=[dict(name=name, box=box) for name, box in names.items()],
                    specs=[dict(peer=name, box=box, role='host' if name == 'erol' else 'player') for name, box in names.items()],
                    preflights={box: dict(head='a'*40, machine_id=box, executable_sha256='b'*64,
                        build=dict(commit='a'*40, executable_sha256='b'*64)) for box in names.values()},
                    capture_rows_pending=[], faults=[dict(id='stall', action='live-stall', peer='mac')])
    checks = dict.fromkeys((*cross_report.CORE_CHECKS, 'shared_fullstate', 'all_incarnation_exits',
                           'no_engine_findings', 'box_pace', 'bounded_recovery', 'faults_applied',
                           'native_fault_effects', 'quiet_feel', 'unique_gameplay_budget',
                           'coverage_minima', 'memory_bounds', 'acceptance_roster', 'build_receipts'), True)
    peers = {name: dict(feel_gated=True, feel_pass=True, record=dict(exe_sha256='b'*64),
                       memory_by_incarnation={'0': dict(passed=True, sizes={'private': {}}, missing_samples=0)},
                       memory_census={'0': dict(status='PASS', warm_slope_bound=10)})
             for name in ('erol', 'edith', 'mac', 'linux')}
    return manifest, checks, peers, [dict(status='PASS', category='combat')], []


class AttemptRequirements(unittest.TestCase):
    def test_p01_failed_required_evidence_cannot_pass_v1(self):
        args = attempt()
        args[0]['faults'] += [dict(id='end', action='brain-eliminate', phase='catch_up'),
                             dict(id='archive', action='crash-restart', restore='archive')]
        args[1].update(quiet_feel=False, unique_gameplay_budget=False, coverage_minima=False,
                       forced_end_during_transfer=False, changed_settings_rematch=False,
                       fog_on_match=False, round_ended=True, validated_autosave_archives=False)
        result = cross_report.judge_attempt(*args)
        self.assertFalse(result['v1_passed'], result)

    def test_each_required_conjunct_fails_independently(self):
        self.assertTrue(cross_report.judge_attempt(*attempt())['v1_passed'])
        for key in ('quiet_feel', 'unique_gameplay_budget', 'coverage_minima', 'memory_bounds'):
            with self.subTest(key=key):
                args = attempt()
                args[1][key] = False
                self.assertFalse(cross_report.judge_attempt(*args)['v1_passed'])

    def test_each_scheduled_oracle_is_required_even_if_no_round_ended(self):
        for extra in ([dict(id='end', action='brain-eliminate', phase='hold')],
                      [dict(id='end', action='brain-eliminate', phase='catch_up')],
                      [dict(id='archive', action='crash-restart', restore='archive')]):
            with self.subTest(schedule=extra):
                args = attempt()
                args[0]['faults'] += extra
                args[1].update(round_ended=False, forced_end_during_hold=True, forced_end_during_transfer=True,
                               changed_settings_rematch=True, fog_on_match=True, validated_autosave_archives=True)
                self.assertTrue(cross_report.judge_attempt(*args)['v1_passed'])
                for key in ('forced_end_during_hold' if extra[0].get('phase') == 'hold' else
                            'forced_end_during_transfer' if extra[0].get('phase') == 'catch_up' else 'validated_autosave_archives',):
                    args[1][key] = False
                    self.assertFalse(cross_report.judge_attempt(*args)['v1_passed'])
        args = attempt()
        args[0]['specs'][0]['flags'] = ['-net-cross-rematches', '4096']
        args[1]['round_ended'] = False
        self.assertEqual(cross_report.judge_attempt(*args)['oracles']['rematches']['status'], 'FAIL')

    def test_unasked_paths_are_inapplicable_and_required_unknown_coverage_is_not_pass(self):
        args = attempt()
        result = cross_report.judge_attempt(*args)
        self.assertEqual([result['oracles'][name]['status'] for name in ('forced_ends', 'rematches', 'autosaves')], ['NOT APPLICABLE'] * 3)
        args[3][0]['status'] = 'NOT COVERED'
        self.assertFalse(cross_report.judge_attempt(*args)['v1_passed'])

    def test_ungated_peer_timing_cannot_supply_quiet_feel(self):
        args = attempt()
        args[2]['mac']['feel_gated'] = False
        self.assertFalse(cross_report.judge_attempt(*args)['v1_passed'])

    def test_every_instance_needs_its_own_completed_progress(self):
        instances = [dict(name=name) for name in ('a', 'b')]
        events = {'a': [dict(type='progress', budget_tick=72000)],
                  'b': [dict(type='progress', budget_tick=71999)]}
        self.assertFalse(cross_report.completed_workload(instances, events, 72000)['passed'])
        events['b'].append(dict(type='progress', budget_tick=72000))
        self.assertTrue(cross_report.completed_workload(instances, events, 72000)['passed'])
        events['b'] = [dict(type='configured', budget_tick=72000)]
        self.assertFalse(cross_report.completed_workload(instances, events, 72000)['passed'])


if __name__ == '__main__':
    unittest.main()
