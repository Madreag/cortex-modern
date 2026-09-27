import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import cross_peers
import cross_report


class CrossDriverTests(unittest.TestCase):
    def plan(self):
        return cross_peers.make_plan(cross_peers.parse_args(['--dry-run']))

    def test_declared_peer_count_matches_private_instances_on_a_box(self):
        manifest=json.loads((cross_peers.HERE/'cross_peers/boxes.json').read_text())
        manifest['instances'].append(dict(name='mac2',box='Mac',port_block=[49905,49909],seat='spectator'))
        next(b for b in manifest['boxes'] if b['name']=='Mac')['peers_per_box']=2
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'boxes.json'; path.write_text(json.dumps(manifest))
            plan=cross_peers.make_plan(cross_peers.parse_args(['--boxes',str(path),'--dry-run']))
            peers=[s for s in plan['specs'] if s['box']=='Mac']
            self.assertEqual(len({s['own'] for s in peers}),2)
            self.assertEqual(len({s['participant_key_root'] for s in peers}),2)
            self.assertTrue(all(s['under_load_by_design'] for s in peers))
            next(b for b in manifest['boxes'] if b['name']=='Mac')['peers_per_box']=1
            path.write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError,'peers_per_box'): cross_peers.load_boxes(path)

    def test_capability_read_never_constructs_an_engine_before_s3_done(self):
        import run_sim_test
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary); box=copy.deepcopy(self.plan()['boxes'][0])
            box.update(guard_file=str(root/'S3-DONE'),scratch=str(root))
            with patch.object(run_sim_test,'make_run') as launch:
                with self.assertRaisesRegex(RuntimeError,'guard'): cross_peers.read_capabilities(box,root)
                launch.assert_not_called()

    def test_case_alias_cannot_share_an_instance_directory_on_windows(self):
        manifest=json.loads((cross_peers.HERE/'cross_peers/boxes.json').read_text())
        manifest['instances'].append(dict(name='MAC',box='Mac',port_block=[49905,49909],seat='spectator'))
        next(b for b in manifest['boxes'] if b['name']=='Mac')['peers_per_box']=2
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'boxes.json'; path.write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError,'unique'): cross_peers.load_boxes(path)

    def test_renewed_feel_reservation_blocks_an_already_released_s3_box(self):
        import run_sim_test
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); done=root/'S3-DONE'; marker=root/'FEEL-MATRIX-RUNNING'
            done.touch(); marker.touch()
            box=copy.deepcopy(self.plan()['boxes'][0]); box.update(guard_file=str(done),exclusive_marker=str(marker),scratch=str(root))
            with patch.object(run_sim_test,'make_run') as launch:
                with self.assertRaisesRegex(RuntimeError,'exclusive'): cross_peers.read_capabilities(box,root)
                launch.assert_not_called()

    def test_quns_release_selftest_starts_no_engine(self):
        with tempfile.TemporaryDirectory() as temporary:
            result=cross_peers.launch_guard_selftest(Path(temporary))
            self.assertTrue(result['passed'])
            self.assertEqual(result['engine_launches'],0)
            self.assertEqual(result['states'],['QUNS_BUSY','QUNS_BUSY',None])
            self.assertTrue(result['release_after_last_guard_clear'])

    def test_payload_release_requires_publication_and_respects_cancellation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            with self.assertRaises(TimeoutError): cross_peers.wait_for_payload_release(root,0)
            (root/'launch-go.json').write_text('{}')
            cross_peers.wait_for_payload_release(root,0)
            (root/'stop.json').write_text('{}')
            with self.assertRaises(RuntimeError): cross_peers.wait_for_payload_release(root,0)

    def test_exited_payload_cannot_release_its_stale_ready_file(self):
        from unittest.mock import Mock
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); (root/'launch-ready.json').write_text('{}')
            box=dict(name='local',kind='windows-local')
            with self.assertRaisesRegex(RuntimeError,'exited'):
                cross_peers.release_ready_payloads({'local':box},{'local':(None,None,str(root))},root,{'local':Mock(poll=lambda:1)},1)
            self.assertFalse((root/'launch-go.json').exists())

    def test_index_orders_launches_not_report_regeneration_times(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            for name,started,modified in [('older','2026-09-26 20:00:00 MST',20),('newer','2026-09-27 01:00:00 MST',10)]:
                own=root/name; own.mkdir(); result=own/'result.json'
                result.write_text(json.dumps(dict(passed=False,requirements=[],manifest=dict(started=started))))
                cross_peers.os.utime(result,(modified,modified))
            cross_report.write_index(root)
            page=(root/'index.html').read_text()
            self.assertLess(page.index('newer/report.html'),page.index('older/report.html'))

    def test_interrupted_json_publication_keeps_the_previous_complete_document(self):
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'session.json'; path.write_text('{"session":"old"}')
            def fail_document(*args,**kwargs): raise ValueError('serialization interrupted')
            with patch.object(cross_peers.json,'dumps',side_effect=fail_document):
                with self.assertRaises(ValueError): cross_peers.write_json(path,dict(session='new'))
            self.assertEqual(json.loads(path.read_text()),dict(session='old'))
            original=Path.write_text
            def interrupted(target,text,**kwargs):
                original(target,'partial',**kwargs)
                raise OSError('writer interrupted before close')
            with patch.object(Path,'write_text',interrupted):
                with self.assertRaises(OSError): cross_peers.write_json(path,dict(session='new'))
            self.assertEqual(json.loads(path.read_text()),dict(session='old'))

    def test_remote_publication_hides_the_name_until_scp_closes(self):
        for kind in ('windows-task','posix-ssh'):
            published='D:/scratch/session.json' if kind=='windows-task' else '/scratch/session.json'
            calls=[]
            def transfer(argv,**kwargs):
                calls.append(argv)
                if argv[0]=='scp': self.assertNotEqual(argv[-1],f'box:{published}')
                return ''
            with patch.object(cross_peers,'command',side_effect=transfer):
                cross_peers.stage_remote(dict(kind=kind,ssh='box'),'session.json',published)
            self.assertEqual([row[0] for row in calls],['scp','ssh'])
            self.assertIn(published,calls[-1][-1])

    def test_storage_sample_tolerates_a_file_sealed_after_enumeration(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            (root/'retained.gz').write_bytes(b'closed')
            # A payload may verify its gzip and remove the raw input after os.walk
            # lists it but before the coordinator samples that path.
            with patch.object(cross_peers.os,'walk',return_value=[(str(root),[],['gone.txt','retained.gz'])]):
                self.assertEqual(cross_peers.scratch_bytes(root),6)

    def test_live_capture_compression_waits_for_the_matching_writer_scope(self):
        from feel.records import CaptureSealer, open_record
        with tempfile.TemporaryDirectory() as temporary:
            own=Path(temporary); dump=own/'fullstate/process-1/round-2/capture-1/sample'
            (dump/'600').mkdir(parents=True); (own/'engine').mkdir()
            source=dump/'600/scene.txt'; source.write_bytes(b'complete scene\n')
            log=own/'engine/stdout.log'
            log.write_text(f'[fullstate-context] tick=600 round=2 label=sample path={dump}\n')
            sealer=CaptureSealer(own); sealer.poll()
            self.assertTrue(source.is_file())
            with log.open('a') as stream:
                stream.write('[fullstate-scope] tick=601 round=2 label=sample per_peer=camera\n')
                stream.write('[fullstate-scope] tick=600 round=2 label=sample per_peer=')
            sealer.poll(); self.assertTrue(source.is_file())
            with log.open('a') as stream: stream.write('camera\n')
            sealer.poll(); self.assertFalse(source.exists())
            with open_record(source,'rb') as stream: self.assertEqual(stream.read(),b'complete scene\n')
            sealer.poll()
            self.assertEqual(len((own/'compressed-records.jsonl').read_text().splitlines()),1)

    def test_capture_announcement_does_not_resolve_a_directory_still_being_created(self):
        from feel.records import CaptureSealer
        with tempfile.TemporaryDirectory() as directory:
            own=Path(directory); (own/'engine').mkdir()
            dump=own/'fullstate/process-1/round-2/capture-3/sample'
            log=own/'engine/stdout.log'
            log.write_text(f'[fullstate-context] tick=1200 round=2 label=sample path={dump}\n')
            sealer=CaptureSealer(own)
            original=Path.resolve
            def transient(path,*args,**kwargs):
                if path==dump: raise OSError('announced directory is still being created')
                return original(path,*args,**kwargs)
            with patch.object(Path,'resolve',transient): sealer.poll()
            (dump/'1200').mkdir(parents=True); source=dump/'1200/scene.txt'; source.write_text('writer completed')
            with log.open('a') as stream: stream.write('[fullstate-scope] tick=1200 round=2 label=sample per_peer=camera\n')
            sealer.poll(); self.assertTrue(source.with_name('scene.txt.gz').is_file())

    def test_capture_announcement_rejects_a_lexical_escape(self):
        from feel.records import CaptureSealer
        with tempfile.TemporaryDirectory() as directory:
            own=Path(directory)/'own'; (own/'engine').mkdir(parents=True)
            (own/'engine/stdout.log').write_text(f'[fullstate-context] tick=1 round=2 label=sample path={own}/fullstate/../../outside\n')
            with self.assertRaisesRegex(ValueError,'leaves'): CaptureSealer(own).poll()

    def test_live_capture_compression_defers_ambiguous_reexecutions(self):
        from feel.records import CaptureSealer
        with tempfile.TemporaryDirectory() as temporary:
            own=Path(temporary); (own/'engine').mkdir()
            contexts=[]
            for ordinal in (1,2):
                dump=own/f'fullstate/process-1/round-2/capture-{ordinal}/sample'
                (dump/'600').mkdir(parents=True); (dump/'600/scene.txt').write_text('retain until exit')
                contexts.append(f'[fullstate-context] tick=600 round=2 label=sample path={dump}\n')
            (own/'engine/stdout.log').write_text(''.join(contexts)+'[fullstate-scope] tick=600 round=2 label=sample per_peer=camera\n')
            CaptureSealer(own).poll()
            self.assertEqual(len(list((own/'fullstate').rglob('scene.txt'))),2)

    def test_closed_instance_sealing_preserves_nested_record_paths(self):
        from feel.records import open_record
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary); (root/'engine/feel').mkdir(parents=True)
            (root/'engine/feel/raw.jsonl').write_bytes(b'{"type":"frame"}\n')
            (root/'live.jsonl').write_bytes(b'{"tick":1}\n')
            cross_peers.seal_evidence(root)
            with open_record(root/'engine/feel/raw.jsonl','rb') as stream:
                self.assertEqual(stream.read(),b'{"type":"frame"}\n')
            index=[json.loads(line) for line in (root/'compressed-records.jsonl').read_text().splitlines()]
            self.assertIn('engine/feel/raw.jsonl',[r['original'].replace('\\','/') for r in index])

    def test_three_box_plan_has_private_instances_and_no_host_kill(self):
        plan = self.plan()
        self.assertEqual(len(plan['instances']), 3)
        self.assertEqual(len({s['own'] for s in plan['specs']}), 3)
        self.assertTrue(all('incarnation-0' in s['own'] for s in plan['specs']))
        self.assertTrue(all('-feel-measure' in s['flags'] for s in plan['specs']))
        self.assertFalse(any('-tick-hashes' in s['flags'] for s in plan['specs']))

    def test_manifest_accepts_n_boxes_without_a_peer_cap(self):
        original = json.loads((cross_peers.HERE / 'cross_peers/boxes.json').read_text())
        for n in range(3, 35):
            box = copy.deepcopy(original['boxes'][-1]); box['name'] = f'linux{n}'; box['ssh'] = f'linux{n}'
            original['boxes'].append(box)
            original['instances'].append(dict(name=f'peer{n}', box=box['name'], port_block=[49900,49904], seat='spectator'))
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)/'boxes.json'; path.write_text(json.dumps(original))
            self.assertEqual(len(cross_peers.load_boxes(path)['instances']), 35)

    def test_same_box_ports_cannot_overlap(self):
        original = json.loads((cross_peers.HERE / 'cross_peers/boxes.json').read_text())
        original['instances'].append(dict(name='extra', box='Mac', port_block=[49902,49906], seat='member'))
        next(b for b in original['boxes'] if b['name']=='Mac')['peers_per_box']=2
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)/'boxes.json'; path.write_text(json.dumps(original))
            with self.assertRaisesRegex(ValueError, 'share a port'): cross_peers.load_boxes(path)

    def test_chaos_seed_reproduces_choices(self):
        args = ['--scenario','chaos','--ticks','36000','--chaos-seed','71']
        first = cross_peers.make_plan(cross_peers.parse_args(args))
        second = cross_peers.make_plan(cross_peers.parse_args(args))
        self.assertEqual(first['faults'], second['faults'])
        self.assertIn('choices only',first['seed_scope'])

    def test_first_soak_cannot_accidentally_remove_its_host(self):
        for host in ('edith','mac'):
            with self.assertRaisesRegex(ValueError,'host removal'):
                cross_peers.make_plan(cross_peers.parse_args(['--scenario','soak','--host',host]))

    def test_new_round_hash_does_not_prove_changed_settings(self):
        rows=[dict(config_hash='first',difficulty=50,fog=False),dict(config_hash='next',difficulty=50,fog=False)]
        self.assertFalse(cross_report.changed_settings(rows))
        rows[1]['fog']=True
        self.assertTrue(cross_report.changed_settings(rows))

    def test_faults_follow_the_declared_restart_incarnation(self):
        plan=cross_peers.make_plan(cross_peers.parse_args(['--scenario','soak']))
        mac=[row for row in plan['faults'] if row['peer']=='mac']
        self.assertEqual([row['incarnation'] for row in mac],[0,1,1])
        spec=next(s for s in plan['specs'] if s['peer']=='mac')
        restarted=cross_peers.restart_spec(spec,dict(tick=400,budget_tick=14400,budget_base=14000,first_gameplay_tick=1))
        self.assertNotEqual(restarted['own'],spec['own'])
        old_ticket=spec['flags'][spec['flags'].index('-net-reconnect-ticket')+1]
        new_ticket=restarted['flags'][restarted['flags'].index('-net-reconnect-ticket')+1]
        self.assertEqual(old_ticket,new_ticket)
        self.assertEqual(restarted['incarnation'],1)

    def test_tail_keeps_partial_lines_and_rotated_parts(self):
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'events.jsonl'
            path.write_bytes(b'{"tick":1}\n{"tick":')
            tail=cross_peers.Tail(path)
            self.assertEqual(tail.read(),[dict(tick=1)])
            with path.open('ab') as stream: stream.write(b'2}\n')
            self.assertEqual(tail.read(),[dict(tick=2)])
            Path(str(path)+'.part1').write_text('{"tick":3}\n')
            self.assertEqual(tail.read(),[])
            self.assertEqual(tail.read(),[dict(tick=3)])

    def test_missing_every_peer_produces_failure_and_phone_report(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)/'run'; root.mkdir()
            plan=self.plan(); (root/'manifest.json').write_text(json.dumps(plan))
            result=cross_report.build_report(root)
            self.assertFalse(result['passed'])
            self.assertFalse(result['checks']['three_real_boxes'])
            page=(root/'report.html').read_text(encoding='utf-8')
            self.assertIn('name="viewport"',page)
            self.assertNotIn('<script src=',page)
            self.assertNotIn('<link ',page)
            self.assertEqual(len([r for r in result['requirements'] if isinstance(r['number'],int)]),71)
            self.assertTrue((root.parent/'index.html').is_file())
            own=cross_report.peer_root(root,plan,plan['specs'][0]); own.mkdir(parents=True,exist_ok=True)
            (own/'match-report.json').write_text(json.dumps(dict(lockstep=dict(missing_frame_stalls=0,next_frame=0,steady_missing_frame_stalls=None))))
            result=cross_report.build_report(root)
            self.assertIsNone(result['peers'][plan['specs'][0]['peer']]['timing']['steady_missing_frame_stalls'])
            own.with_name('incarnation-1').mkdir()
            result=cross_report.build_report(root)
            self.assertEqual(result['peers'][plan['specs'][0]['peer']]['incarnation'],1)
            self.assertEqual(len(result['peers'][plan['specs'][0]['peer']]['fragments']),2)

    def test_report_requires_all_peers_shared_capture_and_binary_limit(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)/'run'; root.mkdir()
            plan=self.plan(); plan['ticks']=4; plan['fullstate_every']=2; plan['capture_rows_pending']=[]
            plan['preflights']={b['name']:dict(machine_id=b['name']) for b in plan['boxes']}
            (root/'manifest.json').write_text(json.dumps(plan))
            for spec in plan['specs']:
                own=cross_report.peer_root(root,plan,spec); (own/'engine').mkdir(parents=True)
                box=next(b for b in plan['boxes'] if b['name']==spec['box'])
                box_root=root if box['kind']=='windows-local' else root/'boxes'/box['name']
                (box_root/'capabilities.json').write_text(json.dumps(dict(peer_limit=4)))
                (box_root/'samples.jsonl').write_text(json.dumps(dict(peer=spec['peer'],incarnation=0,engine_pid=99,
                    elapsed_s=0,working_set=1000,private=1000,load=[]))+'\n')
                (own/'record.json').write_text(json.dumps(dict(started=True,exit_code=0,elapsed_seconds=1,timed_out=False)))
                (own/'trace.json').write_text(json.dumps(dict(runs=[dict(strings=dict(completion='completed'))])))
                (own/'match-report.json').write_text(json.dumps(dict(exit_code=0,desync_check=dict(mismatches=0,compares=1,compare_margin=0))))
                live=[dict(session='s',match='m',history_branch='initial',source_round=1,round=1,tick=t,
                    instance=spec['peer'],execution='one',incarnation=0,phase='live',wall_ms=t*20,gameplay_tick=True,
                    effective_start_frame=1,sim_gated='a'*64,subsystems={key:'b'*64 for key in cross_report.REQUIRED_SUBSYSTEMS}) for t in range(1,5)]
                (own/'live.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in live))
                config=dict(type='adopted_config',peer_count=3,sim_tick_ms=1000/60,difficulty=50,
                    players=[dict(peer=p+1,team=p,human=True) for p in range(3)]+[dict(peer=0,team=3,human=False)],
                    config=dict(mode='pvpve',activity_preset='Multi Box Combat',scene_name='Grasslands',rules=dict(teams=[dict(ai_skill=50)]*4)))
                (own/'events.jsonl').write_text(json.dumps(config)+'\n'+
                    ''.join(json.dumps(dict(**row,type='tick_timing',compute_us=10,capture_us=0,partition_valid=True))+'\n' for row in live))
                (own/'engine/stdout.log').write_text(''.join(
                    f'[fullstate-context] tick={t} round=1 label=sample path=/instance/capture-{t}\n'
                    f'[fullstate] tick={t} hash=0123456789abcdef sections=header:0123456789abcdef,scene:0123456789abcdef round=1\n'
                    f'[fullstate-scope] tick={t} round=1 label=sample per_peer=camera\n' for t in (1,2,4)))
            self.assertTrue(cross_report.build_report(root)['passed'])
            own=cross_report.peer_root(root,plan,plan['specs'][-1])
            with (own/'live.jsonl').open('a') as stream: stream.write(json.dumps(live[-1])+'\n')
            failed=cross_report.build_report(root)
            self.assertFalse(failed['passed']); self.assertEqual(failed['comparison']['duplicates'],1)


if __name__ == '__main__': unittest.main()
