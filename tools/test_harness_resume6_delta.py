"""The additional window, timing and optional handheld rulings; synthetic inputs only."""
import copy
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import acceptance_collection as collection
from test_harness_resume5 import built,REPO
from test_inventory_oracle_evidence import run_split,INVENTORY
from test_acceptance_resume3 import build_plan
import check_plan


class AddedRulings(unittest.TestCase):
    def test_r05_omitted_four_box_4k_window_row_is_a_schema_error(self):
        result=built();plan=result['plan'];schedule=result['split_plan']
        rid='cross.match-host-erol-3840x2160-uncapped'
        plan['rows']=[row for row in plan['rows'] if row['id']!=rid]
        for section in plan['sections']: section['rows']=[row for row in section['rows'] if row[1]!=rid]
        for share in schedule['shares']:share['ids']=[name for name in share['ids'] if name!=rid]
        problems=check_plan.static_schema(plan,schedule,plan['requirements']['ids'],REPO)
        self.assertTrue(any('4K-host four-box match' in text for text in problems),problems)

    def test_r06_live_lane_reservation_waits_and_retries_the_same_section(self):
        wait=getattr(collection,'wait_for_window',None)
        self.assertTrue(callable(wait),'window rows need a bounded wait before an engine dispatch')
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);marker=root/'marker'
            marker.write_text(json.dumps(dict(token='lane-owned',pid=os.getpid(),stream_root=str(root/'other'))))
            result=built(marker=marker)
            collection.write(root/'acceptance-plan.json',result['plan'])
            schedule=result['split_plan'];schedule.update(collection_id='unit',source_sha='a'*40,exe_sha256='b'*64,sequence={'index':1})
            collection.write(root/'split-plan.json',schedule)
            section=collection.start_section(root,3,marker=marker,inventory_root=INVENTORY,optional_boxes={})
            self.assertEqual(section['window']['variant'],'WITH')
            before=(root/'sections/3.json').read_bytes();sleeps=[]
            def release(seconds):sleeps.append(seconds);marker.unlink()
            outcome=wait(root,3,marker=marker,timeout=90,sleep_fn=release)
            self.assertEqual(outcome['state'],'READY');self.assertEqual(len(sleeps),1)
            self.assertGreater(sleeps[0],0);self.assertLessEqual(sleeps[0],45*60)
            self.assertEqual((root/'sections/3.json').read_bytes(),before)
            marker.write_text(json.dumps(dict(token='USER-AT-DESK',pid=os.getpid())))
            self.assertEqual(run_split.window_variant(marker)['variant'],'WITHOUT')

    def test_r07_timing_proof_older_than_engine_receipt_stays_on_pc(self):
        boxes,_=run_split.load_manifest(build_plan.BOXES_JSON)
        box=next(box for box in boxes if box.name=='Z13')
        box.timing_proof=dict(date='2026-10-01',series='old-series.json',pass_=True)
        box.timing_proof['pass']=True
        box.build_receipt=dict(date='2026-10-03',commit='a'*40,executable_sha256='b'*64)
        self.assertFalse(run_split.timing_proven(box),'a proof predating the current engine is stale')
        status=run_split.timing_proof_status(box)
        self.assertIn('older than',status['reason'])
        box.timing_proof['date']='2026-10-03'
        self.assertTrue(run_split.timing_proven(box))

    def test_r08_away_ally_is_optional_and_its_own_shares_are_deferred(self):
        boxes,_=run_split.load_manifest(build_plan.BOXES_JSON)
        ally=next(box for box in boxes if box.name=='ALLY')
        self.assertTrue(ally.optional,'the handheld leaves the LAN for hotspot sessions')
        result=built()
        self.assertTrue(any(share.box.name=='ALLY' for share in result['shares']))
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            collection.write(root/'acceptance-plan.json',result['plan'])
            schedule=result['split_plan'];schedule.update(collection_id='unit',source_sha='a'*40,exe_sha256='b'*64,sequence={'index':1})
            collection.write(root/'split-plan.json',schedule)
            choice=collection.start_section(root,1,marker=root/'absent-marker',inventory_root=INVENTORY,optional_boxes={'ALLY':False})
            expected={(row['share'],row['id']) for row in result['plan']['rows'] if row['section']==1 and
                      (row['engine_boxes'].get('ALLY') or row['box']=='ALLY')}
            deferred={(row['share'],row['id']) for row in choice['rows'] if row['state']=='AWAITING'}
            self.assertEqual(deferred,expected)
            self.assertEqual({row['reason'] for row in choice['rows'] if row['state']=='AWAITING'},{'ALLY absent: required game peer deferred'})


if __name__=='__main__':unittest.main()
