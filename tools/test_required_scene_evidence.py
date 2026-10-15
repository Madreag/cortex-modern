import json
from pathlib import Path
import tempfile
import unittest

import e2e_video as video


class RequiredSceneEvidence(unittest.TestCase):
    def test_native_ownership_links_seat_actor_ticket_and_fresh_input(self):
        from e2e.ownership import reclaim_evidence
        from test_soak_oracle_evidence import control
        for case in ('clean', 'seat', 'owner', 'actor', 'ticket', 'stale', 'wrong_process'):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                claim = dict(type='ownership_reclaim', round=7, peer=2, owner_peer=2, incarnation=0, stable_seat=1,
                             actor=9, ticket_incarnation=4, seat_incarnation=4, activation_tick=1100, tick=1100, committed=True)
                fresh = control(seat_incarnation=4)
                if case=='seat': claim['stable_seat']=0
                if case=='owner': claim['owner_peer']=1
                if case=='actor': fresh['actor']=8
                if case=='ticket': claim['ticket_incarnation']=3
                if case=='stale': fresh['input']['produced_tick']=1099
                if case=='wrong_process': fresh['incarnation']=1
                (root/'events.jsonl').write_text('\n'.join(map(json.dumps, [claim, fresh])))
                self.assertEqual(reclaim_evidence(root, dict(seat=1))['passed'], case=='clean')

    def test_backdrop_success_reaches_the_required_scene_item(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for peer, scale in (('host', '2,2'), ('client', '1,1')):
                (root/peer).mkdir()
                (root/peer/'stdout.log').write_text(f'[scene] backdrop Sky fit scale={scale}\n'+
                    ('[scene] backdrop Sky restored scale=2,2\n[scene] backdrop Sky fit scale=1,1\n' if peer=='client' else ''))
            scenario = video.load_scenario('mp-repair-cross-resolution')
            capture = dict(root=str(root), peers=[dict(peer='client', root=str(root/'client'))])
            video.feel_probes(scenario, capture, {})
            self.assertEqual(capture['peers'][0]['gates']['backdrop-refit']['status'], 'PASS')
            self.assertTrue(any(item.get('gate')=='backdrop-refit' for item in scenario['checklist']))

    def test_backdrop_failure_reaches_automatic_gate(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for peer in ('host', 'client'):
                (root / peer).mkdir()
                (root / peer / 'stdout.log').write_text('')
            capture = dict(root=str(root), peers=[dict(peer=peer, root=str(root/peer)) for peer in ('host', 'client')])
            video.feel_probes(dict(backdrop_refit_gate=dict(host='host', client='client')), capture, {})
            self.assertEqual(capture['peers'][0].get('gates', {}).get('backdrop-refit', {}).get('status'), 'FAIL')

    def test_moderation_drives_the_offered_row_and_real_accept_control(self):
        scenario = video.load_scenario('mp-moderation')
        run = next(row for row in scenario['runs'] if row['name']=='leave-apply')
        host = next(peer for peer in run['peers'] if peer['name']=='host')
        steps = json.loads(video.scenario_text(scenario, host['probe']))['steps']
        self.assertNotIn('-net-h4-substitute', host['args'])
        self.assertTrue(any(step.get('op')=='assert_control' and step.get('control')=='NetworkSeatApplicant0'
                            and step.get('text_contains')=='newcomer' for step in steps))
        self.assertTrue(any(step.get('op')=='mouse_up' and step.get('control')=='NetworkSeatSubstitute0' for step in steps))

    def test_running_alone_cannot_prove_returned_ownership(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root/'events.jsonl').write_text('')
            peer = dict(root=str(root), video_dir=str(root), index=[dict(frame=1, screen='game', service_state='Running', sim_tick=2700)])
            item = dict(screen='game', service_state='Running', ownership_reclaim=dict(seat=1))
            _, evidence = video.item_evidence(peer, item)
            self.assertEqual(evidence['probe'], 'fail')

    def test_a_marked_item_reads_past_a_step_whose_command_is_a_number(self):
        # The pie menu's step names its command by number: a mark's steps still run to the next mark.
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root/'video').mkdir(); (root/'probe').mkdir()
            (root/'video'/'events.jsonl').write_text(json.dumps(dict(message='video_mark playing-host', wall_ms=10)) + '\n')
            steps = [dict(op='menu', command='video_mark playing-host'), dict(op='pie_point', input_player=0, command=6),
                     dict(op='menu', command='video_mark pie-buy-host')]
            observed = [dict(index=index, observed={}) for index in range(3)]
            (root/'probe'/'net-ui-result.json').write_text(json.dumps(dict(script=dict(steps=steps), steps=observed)))
            record = dict(root=str(root), probe_dir=str(root/'probe'), video_dir=str(root/'video'), index=[])
            _, evidence = video.item_evidence(record, dict(id='playing-host', mark='playing-host', screen='game'))
            self.assertEqual([row['step'] for row in evidence['assertions']], [0, 1])
            self.assertEqual(evidence['probe'], 'pass')


if __name__ == '__main__':
    unittest.main()
