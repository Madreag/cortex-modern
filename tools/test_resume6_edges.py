"""Reservation and receipt edge cases introduced by the additional acceptance rows."""
import contextlib
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import acceptance_collection as collection
import cross_peers
from test_acceptance_resume import fixture,write,reader
from test_inventory_oracle_evidence import INVENTORY,run_stream


class Resume6Edges(unittest.TestCase):
    def test_four_k_cross_borrows_the_parent_window_reservation(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);marker=root/'marker'
            write(marker,dict(token='parent-owned',pid=os.getpid(),stream_root=str(root)))
            before=marker.read_bytes();box=dict(exclusive_marker=str(marker))
            with patch.dict(os.environ,CCCP_FEEL_MATRIX_RUN='parent-owned'),patch.object(cross_peers,'box_load',return_value=[]), \
                 patch.object(cross_peers.time,'sleep'),patch.object(cross_peers.time,'monotonic',side_effect=[0,0,2]):
                try:claim=cross_peers.acquire_reservation(box,root,1)
                except TimeoutError:self.fail('the restored 4K row deadlocks on its own parent reservation')
                self.assertTrue(claim['borrowed'])
                self.assertFalse(cross_peers.release_reservation(claim))
                self.assertEqual(marker.read_bytes(),before)

    def test_expired_reservation_wait_is_pending_without_restarting_section(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);plan,required=fixture(root,item='20')
            declaration=collection.read(plan);spec=declaration['rows'][0]
            spec.update(section=3,window_required=True,engine_boxes={'EROL-PC':1},minutes={'cap':1})
            declaration['sections']=[dict(id=3,rows=[['S1','case']])]
            write(root/'acceptance-plan.json',declaration)
            for path in (root/'S1/case/command.json',root/'S1/case/result.json',root/'S1/DEFECTS.json'):
                if path.exists():path.unlink()
            marker=root/'marker';write(marker,dict(token='live-lane',pid=os.getpid(),stream_root=str(root/'other')))
            choice=collection.start_section(root,3,marker=marker,inventory_root=INVENTORY,optional_boxes={})
            before=(root/'sections/3.json').read_bytes()
            self.assertEqual(collection.wait_for_window(root,3,marker=marker,timeout=0)['state'],'AWAITING')
            result=reader.build_manifest(root/'acceptance-plan.json',required)
            self.assertEqual(result['rows'][0]['status'],'AWAITING',result['rows'][0])
            self.assertIn('reservation',result['rows'][0]['reason'])
            self.assertEqual((root/'sections/3.json').read_bytes(),before)

    def test_legacy_mac_launch_and_cross_refresh_check_reservation(self):
        from types import SimpleNamespace
        with tempfile.TemporaryDirectory() as folder:
            template=Path(folder)/'job.zsh';template.write_text('#!/bin/zsh\nSHA='+('a'*40)+'\nBRANCH=unit\necho RUN\n')
            with patch.object(run_stream,'MAC_TEMPLATE',template):
                script=run_stream.mac_script(SimpleNamespace(head='b'*40,branch='unit'),run_stream.Command('S5','!mac',[],mac={'template':'gates'}),'unit')
            self.assertIn('ACCEPTANCE-STREAM-RUNNING',script)
            self.assertIn('held by',script)
        for script in (Path(__file__).parent/'linux/cross_refresh_boxes.sh',INVENTORY/'cross_refresh_boxes.sh'):
            self.assertIn('held by',script.read_text())


if __name__=='__main__':unittest.main()
