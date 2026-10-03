"""Reservation identity and preflight ordering without starting a native process."""
import hashlib
import json
import os
from pathlib import Path
import unittest
from unittest.mock import patch

import acceptance_box_lease as lease
import acceptance_remote_tasks as remote
from test_acceptance_remote_tasks import profiles


class NativeReservation(unittest.TestCase):
    def test_physical_pc_cannot_hold_a_game_box(self):
        payload=dict(box=profiles()[0])
        with patch.object(Path,'read_text',return_value=json.dumps(payload)), \
                patch.object(lease.platform,'node',return_value='EROL-PC'), \
                patch.object(lease.cross,'acquire_reservation') as acquire:
            with self.assertRaisesRegex(ValueError,'declared remote machine'):
                lease.hold('/virtual/payload.json')
        acquire.assert_not_called()

    def test_borrowed_environment_requires_a_live_unchanged_holder_and_is_restored(self):
        raw=json.dumps(dict(CCCP_FEEL_MATRIX_RUN='fixture-token')).encode()
        good=dict(box='Z13',runner_pid=123,deadline_monotonic_s=10,environment_sha256=hashlib.sha256(raw).hexdigest())
        for defect in (None,'dead','digest','ended','wrong-box'):
            ready=dict(good)
            if defect=='digest': ready['environment_sha256']='0'*64
            if defect=='wrong-box': ready['box']='EDITH'
            before=dict(os.environ)
            with self.subTest(defect=defect), patch.object(Path,'is_file',return_value=True), \
                    patch.object(Path,'read_bytes',return_value=raw), patch.object(Path,'read_text',return_value=json.dumps(ready)), \
                    patch.object(Path,'exists',return_value=defect=='ended'), patch.object(lease,'alive',return_value=defect!='dead'), \
                    patch.object(lease.time,'monotonic',return_value=1), \
                    patch.object(lease.cross,'owns_reservation',return_value=True), patch.object(lease.cross,'assert_box_guard'):
                if defect is None:
                    with lease.borrow(dict(name='Z13'),'/virtual') as borrowed:
                        self.assertTrue(borrowed)
                        self.assertEqual(os.environ['CCCP_FEEL_MATRIX_RUN'],'fixture-token')
                else:
                    with self.assertRaises(ValueError):
                        with lease.borrow(dict(name='Z13'),'/virtual'):
                            self.fail('changed reservation was borrowed')
            self.assertEqual(dict(os.environ),before)

    def test_reservations_precede_native_preflight(self):
        box=profiles()[0]
        plan=dict(run='fixture',boxes=[box],specs=[dict(box='Z13',peer='erol')])
        events=[]
        def start(plan,root,holders):
            events.append('reserved'); holders['Z13']='native-holder'
        def command(args,**kwargs):
            if isinstance(args[-1],str) and '--preflight' in args[-1]:
                self.assertIn('reserved',events)
                events.append('preflight')
            return ''
        with patch.object(Path,'mkdir'), patch.object(Path,'read_text',return_value='{}'), \
                patch.object(remote,'helper_archive',return_value=Path('/virtual/helpers.tar')), \
                patch.object(remote,'remote_python'), patch.object(remote,'publish_new'), patch.object(remote,'write_json'), \
                patch.object(remote,'start_leases',side_effect=start,create=True), \
                patch.object(remote,'stop_leases',return_value=[],create=True) as release, \
                patch.object(remote.cross,'command',side_effect=command), \
                patch.object(remote.world,'fetch_preserved'), patch.object(remote,'validate_preflights'):
            holders=remote.stage(plan,Path('/virtual/run'))
        self.assertEqual(events,['reserved','preflight'])
        self.assertEqual(holders,{'Z13':'native-holder'})
        release.assert_not_called()

    def test_native_preflight_failure_releases_the_held_boxes(self):
        box=profiles()[0]
        plan=dict(run='fixture',boxes=[box],specs=[dict(box='Z13',peer='erol')])
        def start(plan,root,holders): holders['Z13']='native-holder'
        with patch.object(Path,'mkdir'), patch.object(Path,'read_text',return_value='{}'), \
                patch.object(remote,'helper_archive',return_value=Path('/virtual/helpers.tar')), \
                patch.object(remote,'remote_python'), patch.object(remote,'publish_new'), patch.object(remote,'write_json'), \
                patch.object(remote,'start_leases',side_effect=start), patch.object(remote,'stop_leases',return_value=[]) as release, \
                patch.object(remote.cross,'command'), patch.object(remote.world,'fetch_preserved'), \
                patch.object(remote,'validate_preflights',side_effect=ValueError('foreign workload')):
            with self.assertRaisesRegex(ValueError,'foreign workload'):
                remote.stage(plan,Path('/virtual/run'))
        self.assertEqual(release.call_args.args[2],{'Z13':'native-holder'})

    def test_spectator_preflight_uses_the_same_live_reservation_layer(self):
        import acceptance_spectator_tasks as spectator
        box=profiles('world-join')[0]
        plan=dict(run='fixture',boxes=[box])
        events=[]
        def start(plan,root,holders):
            events.append('reserved'); holders['Z13']='native-holder'
        def command(args,**kwargs):
            if isinstance(args[-1],str) and '--preflight' in args[-1]:
                self.assertIn('reserved',events); events.append('preflight')
            return ''
        with patch.object(Path,'mkdir'), patch.object(Path,'read_text',return_value='{}'), \
                patch.object(spectator,'helper_archive',return_value=Path('/virtual/helpers.tar')), \
                patch.object(remote,'remote_python'), patch.object(remote,'publish_new'), patch.object(spectator,'write_json'), \
                patch.object(remote,'start_leases',side_effect=start), patch.object(remote,'stop_leases',return_value=[]) as release, \
                patch.object(remote.cross,'command',side_effect=command), \
                patch.object(remote.world,'fetch_preserved'), patch.object(spectator,'check_preflights'):
            holders=spectator.stage(plan,Path('/virtual/run'))
        self.assertEqual(events,['reserved','preflight'])
        self.assertEqual(holders,{'Z13':'native-holder'})
        release.assert_not_called()


if __name__=='__main__':
    unittest.main()
