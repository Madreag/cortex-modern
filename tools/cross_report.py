"""Build a self-contained, evidence-linked multi-box report and its run index."""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import html
import hashlib
import json
from pathlib import Path
import re

from feel import report
from feel.records import open_record, record_path, presentation_records
from compare_sim_traces import CORE

HERE = Path(__file__).resolve().parent
REQUIRED_SUBSYSTEMS = CORE | {'controller'}
CORE_CHECKS=('three_real_boxes','preflight_complete','full_history','zero_desync','zero_unscheduled_holds',
             'native_completion','adopted_peer_count','adopted_roster','native_desync_checks','record_integrity','binary_admission_limit')
CAPTURE_ROWS={
    1:'capture-rows lane row 1: unsupported userdata in object[...][AI][Behavior].slot[12] at Source/Managers/LuaMan.cpp:1613',
    2:'capture-rows lane row 2: Windows/Mac hex-float text at Source/System/FloatText.h:324-326,608-610; PieMenu.cpp:259; Arm.cpp:506; Scene.cpp:2056,2059,2142'}


def judge_attempt(manifest,checks,peers,matrix,recoveries):
    core=all(checks.get(name,False) for name in CORE_CHECKS)
    oracle=lambda passed,reason='':dict(status='PASS' if passed else 'FAIL',reason=reason)
    pending=manifest.get('capture_rows_pending',[1,2])
    memory=[value for peer in peers.values() for value in peer.get('memory_by_incarnation',{}).values()]
    memory_status='PASS' if memory and all(m['passed'] for m in memory) else \
        'FAIL' if any(m.get('sizes') or m.get('missing_samples') for m in memory) else 'NOT COVERED'
    coverage_status='FAIL' if any(row['status']=='FAIL' for row in matrix) else \
        'NOT COVERED' if any(row['status']=='NOT COVERED' for row in matrix) else 'PASS'
    oracles=dict(
        live_hashes=oracle(checks.get('full_history',False) and checks.get('zero_desync',False),'Every declared comparable key; UNKNOWN never equals.'),
        unscheduled_holds=oracle(checks.get('zero_unscheduled_holds',False)),
        native_completion=oracle(checks.get('native_completion',False)),
        full_state=oracle(checks.get('shared_fullstate',False) and not pending,
            'NOT COVERED by '+ '; '.join(CAPTURE_ROWS[row] for row in pending) if pending else 'All promised Shared/canonical/restored observations must compare.'),
        recovery=oracle(checks.get('bounded_recovery',False),'Every recovery id must reach its declared terminal outcome within its declared deadline.'),
        fault_effects=oracle(checks.get('faults_applied',False) and checks.get('native_fault_effects',False),'Arming H4 is separate from observing its native ack/commit effect.'),
        coverage=dict(status=coverage_status,reason='Each matrix row retains its own minimum, counts and reason.'),
        feel=dict(status=('PASS' if checks.get('quiet_feel',False) else 'FAIL') if any(p.get('feel_gated') for p in peers.values()) else
                         'UNDER LOAD' if any(p.get('feel_status')=='UNDER LOAD' for p in peers.values()) else 'REPORTED',reason='Gated only in a declared quiet window without measured load.'),
        memory=dict(status=memory_status,reason='Declared per-incarnation warm-up, slope and retention; raw sizes and measured instrumentation remain separate.'),
        record_integrity=oracle(checks.get('record_integrity',False)),
        engine_findings=oracle(checks.get('no_engine_findings',False),'All findings remain visible, including the named capture rows.'),
        exits=oracle(checks.get('all_incarnation_exits',False),'Each incarnation must exit normally or have its own scheduled, actually injected crash receipt.'))
    if not manifest.get('faults'): oracles['recovery']['status']='NOT APPLICABLE'
    if not manifest.get('faults'): oracles['fault_effects']['status']='NOT APPLICABLE'
    if manifest['scenario']!='match':
        oracles['forced_ends']=oracle(checks.get('forced_end_during_hold',False) and checks.get('forced_end_during_transfer',False),'Actual activity-over must overlap the named recovery phase; a stale hint is insufficient.')
        oracles['rematches']=oracle(checks.get('changed_settings_rematch',False) and checks.get('fog_on_match',False))
        oracles['autosaves']=oracle(checks.get('validated_autosave_archives',False),'Archive integrity alone does not prove restoration or sealed admission.')
    return dict(core_passed=core,gate_b_eligible=core and manifest['scenario']=='match' and manifest['ticks']==1201 and bool(manifest.get('fullstate_every')) and not manifest.get('faults'),oracles=oracles)


def judge_exit(record,peer,incarnation,faults,receipts):
    expected=next((f for f in faults if f['peer']==peer and f.get('incarnation',0)==incarnation and f['action']=='crash-restart'),None)
    injected=bool(expected and record.get('injected_termination')=='scheduled crash '+expected['id'] and
        any(r.get('id')==expected['id'] and r.get('peer')==peer and r.get('incarnation')==incarnation and
            r.get('phase')=='fault_applied' and r.get('native',{}).get('action')=='crash-restart' for r in receipts))
    normal=record.get('exit_code')==0 and not record.get('injected_termination')
    return dict(passed=bool(record.get('started') and not record.get('timed_out') and (injected or normal)),
                exit_code=record.get('exit_code'),timed_out=record.get('timed_out'),incarnation=incarnation,
                expected='scheduled crash '+expected['id'] if expected else 'normal exit',
                actual=record.get('injected_termination','normal exit'),injection_proved=injected)


def scheduled_hold(hold,receipts,recoveries):
    for recovery in recoveries:
        if not recovery['passed']: continue
        starts=[r.get('actual',r) for r in receipts if r.get('id')==recovery['id'] and r.get('source_peer')==recovery['peer'] and
                r.get('applied') and r.get('action')!='brain-eliminate']
        terminals=[r.get('native',{}) for r in recovery['phases'] if r.get('phase') in ('first_controllable_input','match_over_goodbye')]
        for start in starts:
            for end in terminals:
                if start.get('peer')==hold['peer'] and start.get('source_round')==end.get('source_round')==hold.get('source_round') and \
                        start.get('source_round') is not None and start.get('tick',float('inf'))<=hold['tick']<=end.get('tick',-1):
                    return recovery['id']
    return None


def attempt_label(result):
    if result.get('passed'): return 'PASS'
    return 'CORE PASS; FULL GATE RED' if result.get('core_passed') else 'CORE FAIL; FULL GATE RED'


def load(path, default=None):
    return json.loads(Path(path).read_text(encoding='utf-8-sig')) if Path(path).is_file() else default


def rows(path):
    index=Path(path).with_name('raw.index.json')
    if Path(path).name=='raw.jsonl' and index.is_file():
        try:
            for number,row in enumerate(presentation_records(index),1): yield dict(row,_line=number)
        except (OSError,ValueError,KeyError,EOFError) as error:
            yield dict(type='malformed_record',error=str(error),_line=0)
        return
    if record_path(path).is_file():
        with open_record(path,encoding='utf-8-sig') as stream:
            for number,line in enumerate(stream,1):
                try:
                    row=json.loads(line)
                    if not isinstance(row,dict): raise ValueError('record is not an object')
                    row['_line']=number
                    yield row
                except ValueError as error:
                    yield dict(type='malformed_record',error=str(error),_line=number)


def event_paths(own):
    bases={str(p)[:-3] if str(p).endswith('.gz') else str(p) for p in own.glob('events.jsonl.part*') if not str(p).endswith('.partial')}
    return [own/'events.jsonl', *[Path(p) for p in sorted(bases,key=lambda p:int(p.rsplit('.part',1)[1]))]]


def source_rows(path, root):
    for row in rows(path):
        row['_path']=str(record_path(path).relative_to(root))
        yield row


def peer_root(root, manifest, spec):
    box = next(b for b in manifest['boxes'] if b['name'] == spec['box'])
    relative = Path(spec['own'].replace('\\', '/')).relative_to(Path(spec['root'].replace('\\', '/')))
    return (root if box['kind'] == 'windows-local' else root / 'boxes' / box['name']) / relative


def read_log(path):
    if Path(path).is_file():
        with Path(path).open(encoding='utf-8-sig', errors='replace') as stream:
            yield from enumerate(stream, 1)


def roster_matches(manifest, configs):
    if not configs: return False
    humans=sum(p.get('seat','player')=='player' for p in manifest['instances'])
    roster=manifest['roster']
    human_teams=[0]*humans if roster=='ai-heavy' else [0,0,1] if roster in ('mixed','allies') else list(range(humans))
    cpu_teams=[1,2] if roster=='ai-heavy' else [2] if roster in ('mixed','allies') else [humans]
    mode='coop-pve' if roster=='ai-heavy' else 'pvpve'
    for config in configs:
        players=config.get('players',[]); native=config.get('config',{})
        if sorted(p.get('team',-1) for p in players if p.get('human'))!=human_teams or \
           sorted(p.get('team',-1) for p in players if not p.get('human'))!=cpu_teams or \
           any(p.get('peer')!=0 for p in players if not p.get('human')) or \
           native.get('mode')!=mode or native.get('activity_preset')!='Multi Box Combat': return False
    skill=manifest['specs'][0]['initial_skill']; initial=configs[0]
    teams=initial.get('config',{}).get('rules',{}).get('teams',[])
    return initial.get('difficulty')==skill and len(teams)==4 and all(t.get('ai_skill')==skill for t in teams) and \
           initial.get('config',{}).get('scene_name')==manifest['scene']


def changed_settings(configs):
    return len({(c.get('difficulty'),c.get('fog'),c.get('config',{}).get('scene_name'),
        tuple(t.get('ai_skill') for t in c.get('config',{}).get('rules',{}).get('teams',[]))) for c in configs})>1


def coverage(events, peers, manifest):
    definitions = [
        ('movement', 'Movement, jetpack, climb and impact', [], 5, 'each kind per originating seat per ten minutes', 'Movement, climb and impact success hooks and obstacle fixtures are incomplete.'),
        ('weapons', 'Weapon classes and actions', ['round_fired', 'reload_completed', 'thrown_release'], 1, 'each class and action per seat per match', 'Melee, shield, explosion, sharp-aim and brain-weapon oracles remain absent.'),
        ('pie', 'Every pie command', [], 1, 'each command per seat per soak', 'No complete enumerated command-effect evidence on every seat.'),
        ('buy', 'Buy menu and delivery lifecycle', ['cargo_ejected', 'craft_departure', 'craft_refund'], 2, 'deliveries per seat, plus one refusal', 'Queue correlation, landing, funds and blocked-LZ refusal chains are incomplete.'),
        ('gold', 'Gold collection and funds', ['gold_deposited'], 1, 'collection per originating seat', 'No verified placed-vein mining chain and purchase/refund-adjusted positive funds assertion.'),
        ('objects', 'Doors, turrets, wounds, gibs and corpses', ['door_open_completed', 'door_close_completed', 'wound_added', 'wound_damage', 'gibbed', 'dying', 'dead'], 1, 'each successful kind per observing peer per soak', 'Emitter-output, turret attribution and corpse-settling records are not complete.'),
        ('ai', 'AI modes, paths, buying and difficulty', [], 1, 'each mode and one AI purchase per soak', 'No complete mode/path-before-and-after-terrain or CPU purchase oracle.'),
        ('rules', 'Rosters, scenes, natural ends and changed rematches', ['match_boundary'], 2, 'natural ends and one changed-settings rematch', 'Real result/lobby surfaces and all roster/scene arms are not demonstrated.'),
        ('scripts', 'Callbacks, timers, coroutines and errors', [], 1, 'both preselected error arms per soak', 'Before/after-write error outcomes and callback/timer/coroutine success records are incomplete.'),
        ('presentation', 'Window, menus, audio and music', [], 1, 'each change and authority scope per soak', 'Audio authority/music transitions require records outside this lane; actual OS presentation is not demonstrated.'),
        ('session', 'Chat, pause, delay, leave, saves, resume and replay', [], 1, 'each defined recovery per soak', 'Recipient, authorization, archive, full playback and first-controllable-input oracles are incomplete.'),
        ('migration', 'Migration, late join, spectate and removal', [], 1, 'each coverable recovery per soak', 'WAN migration endpoint fix is absent; fourth real box and removal retry arms are not demonstrated.'),
        ('members', 'Team members and brains (row 14)', [], 1, 'each member-seat lifecycle and brain rule', 'NOT COVERED until netcode row 14 lands.'),
        ('counts', 'Peer counts (row 13)', [], 1, 'runs at 5, 9, 17, 32 and refused 33', 'NOT COVERED until netcode row 13 lands; current admission limit is reported from the binary.'),
        ('terrain', 'Terrain destruction and object load', ['terrain_removed'], 10000, 'gross removed non-air pixels per peer per soak', ''),
        ('fog', 'Fog reveal, capture and rejoin', [], 1, 'one fog arm per soak', 'No eligible fog-on unseen-layer and visible-reveal proof.'),
        ('mod', 'Void Wanderers mission, economy and transitions', [], 1, 'one real co-op arm', 'This run uses the combat fixture, not the unchanged-reference Void Wanderers arm.')]
    result = []
    for key, label, kinds, minimum, unit, reason in definitions:
        counts = {}
        for peer in peers:
            selected = [event for event in events[peer] if event.get('type') == 'coverage' and event.get('phase') == 'live' and event.get('gameplay_tick')]
            successes = {kind: sum(event.get('amount', 1) for event in selected if event.get('event') == kind
                                   and event.get('result', 'success') in ('success', 'orphan', 'penetrate', 'penetrate_air', 'dislodge', 'silhouette', 'unattributed', 'health_exhausted_unattributed', 'death_timer', 'committed')) for kind in kinds}
            counts[peer] = dict(successes=successes, attempts=sum(event.get('result') == 'attempt' for event in selected if event.get('event') in kinds))
        status = 'NOT COVERED' if reason else 'PASS' if all(c['successes'] and all(v >= minimum for v in c['successes'].values()) for c in counts.values()) else 'FAIL'
        if key=='terrain' and manifest['scenario']=='match':
            status,reason='NOT APPLICABLE','The destruction minimum applies to soaks; smoke event totals remain visible.'
        result.append(dict(id=key, item=label, status=status, reason=reason, unit=unit, minimum=minimum, peers=counts))
    return result


def requirements(manifest, comparison, metrics):
    items = load(HERE / 'cross_peers/requirements.json', [])
    reasons = {
        2: 'WAN migration requires NetMatchService.cpp:8284 endpoint publication and NetLockstep.cpp:3304,3334 dialing changes owned by the catch-up lane.',
        3: 'MIXED uses human teams 0,0,1 plus peerless CPU team 2 through real host options; ordinary AI actors supply allied units. Adopted configurations and all roster arms still need run evidence.',
        20: 'SoundContainer.cpp:405 authority and music-transition emissions are outside this lane; muted output proves no audible result.',
        22: 'A fourth real box and its persistent-world arrival arm are absent.',
        34: 'Hold and catch-up ends are wired into the default schedule with peer/round/incarnation-bound phase signals and native end witnesses. Both executed overlaps and the post-migration end are not yet demonstrated; migration awaits the endpoint seam.',
        35: 'Pre-auth/proof/image/tail/activation overlaps await the WAN endpoint fix; queued/cancelled phases never complete recovery.',
        36: 'Capture-announced and writer-pending barriers have RED/GREEN bounded-release tests; migration overlap, archive validity and restarted writer cadence still await a real endpoint-capable arm.',
        40: 'Initial history uses the configured start frame; settled live authority is read from the public runner report. Private catch-up branch/checkpoint lineage remains unexposed at NetMatchService.h:1305,1599 and NetMatchService.cpp:4534, so restored keys stay UNKNOWN.',
        45: 'Checkpoint boot/handover anchors and survivor segment indexing need ScenarioRunner.cpp:2653 and NetMatchService.cpp:3455 outside this lane.',
        50: 'Movement/aim submitted-render measurements are reported; other action/input-sequence stamps require FrameMan.cpp:277,344 outside this lane.',
        51: 'Remote-unit render discontinuities require FrameMan/LocalPrediction records outside this lane; local corrections retain their own labels.',
        53: 'Fresh produced/applied controller input and authenticated goodbye now have native terminal records, bound to a continuous payload clock. Full phase-specific queued/cancelled admission and survivor end-to-end runs remain incomplete at NetMatchService.cpp:939,5319 and NetWorldJoin.cpp:1139.',
        54: 'FrameMan.cpp:277,344 lacks round/execution ids; multi-round raw presentation reduction cannot safely reuse single-round identities.',
        69: 'The real Void Wanderers mission/economy/scene-transition and unchanged-reference arm has not run.',
        71: 'Fog-on reveal/capture/rejoin is not yet demonstrated; a fog-off fight does not cover it.'}
    for item in items:
        item['status'] = 'NOT COVERED'
        item['reason'] = reasons.get(item['number'], item['reason'])
        item['evidence'] = []
    # Only complete requirements are credited; partial event totals remain visible in the matrix.
    if manifest.get('preflights') and len(manifest['preflights']) == len(manifest['boxes']) and metrics and all(p['record'].get('started') and any(s.get('engine_pid') and (s.get('resident') or s.get('working_set')) for s in p['samples']) for p in metrics.values()):
        for item in items:
            if item['number'] == 66:
                item.update(status='PASS', reason='Each box has one owning runner payload, local scripts and far-side engine PID samples.', evidence=['manifest.json'])
    for item in items:
        if item['number'] in (41,'reread-4') and comparison.get('passed'):
            item.update(status='PASS',reason='Every declared key contains the required hashed subsystems and applied-controller hash; only documented controller_route is excluded. This proves hashed tick-end equality, not all state, input order or AI provenance.',evidence=['result.json#comparison'])
        if item['number']==52 and comparison.get('passed') and metrics and all(p.get('tick_timing_valid') and p.get('frames',0)>0 and p['tick_compute_ms']['count']==p['frames'] for p in metrics.values()):
            item.update(status='PASS',reason='Every compared live tick has a valid exclusive compute/wait/capture partition. Distributions use native per-tick samples; configured sim_tick_ms is not substituted for compute time.',evidence=['result.json#peers'])
        if item['number']=='reread-5' and metrics and all(p['timing'].get('complete') and all(p['timing'].get(k) is not None for k in ('steady_waits_over_50','steady_missing_frame_stalls','net_wait_ms','waiting_percent','longest_stall_ms')) for p in metrics.values()):
            item.update(status='PASS',reason='Matched declared windows report the eligible >50 ms wait count, separate native missing-frame stalls, wait sum, maximum and 100*wait/wall percentage.',evidence=['result.json#peers'])
    return items


def build_report(root):
    root = Path(root).resolve(); manifest = load(root / 'manifest.json', {})
    live, events, peers, findings, paths = {}, {}, {}, list(manifest.get('driver_findings', [])), {}
    capabilities = {}
    for box in manifest['boxes']:
        box_root = root if box['kind'] == 'windows-local' else root / 'boxes' / box['name']
        capabilities[box['name']] = load(box_root / 'capabilities.json', {})
        for leaf in ('payload-error.json', 'seal-error.json'):
            if error := load(box_root / leaf):
                findings.append(dict(kind='payload', box=box['name'], reason=error.get('error', str(error))))
    for spec in manifest['specs']:
        name = spec['peer']; first_own = peer_root(root, manifest, spec)
        fragments=sorted(first_own.parent.glob('incarnation-*'),key=lambda p:int(p.name.split('-')[-1])) or [first_own]
        own=fragments[-1]; paths[name] = own
        live[name] = [row for fragment in fragments for row in source_rows(fragment/'live.jsonl',root)]
        events[name] = [row for fragment in fragments for path in event_paths(fragment) for row in source_rows(path,root)]
        for observed in [*live[name],*events[name]]:
            if observed.get('type')=='malformed_record':
                findings.append(dict(peer=name,path=observed['_path'],line=observed['_line'],text='Malformed or truncated record: '+observed['error']))
        log = [(fragment/'engine'/leaf,number,line) for fragment in fragments for leaf in ('stdout.log','stderr.log')
               for number,line in read_log(fragment/'engine'/leaf)]
        record, native = load(own / 'record.json', {}), load(own / 'match-report.json', {})
        waits = []; holds = []; observed_round=None; observed_log=None
        for log_path, number, line in log:
            if log_path!=observed_log: observed_log=log_path; observed_round=None
            if match := re.search(r'\[cross-context\] round=\d+ source_round=(\d+)',line): observed_round=int(match[1])
            if match := re.search(r'\[net-frame-wait\] frame=(\d+) wait_ms=(\d+)', line):
                waits.append(dict(tick=int(match[1]), wait_ms=int(match[2]), line=number))
            if match := re.search(r'\[net-match\] hold peer=(\d+) frame=(\d+)', line):
                holds.append(dict(peer=int(match[1]), tick=int(match[2]), line=number,source_round=observed_round,path=str(log_path.relative_to(root))))
            if re.search(r'RTE Assert|FATAL:|EXCEPTION_ACCESS_VIOLATION|Runtime Error due to unhandled exception|Rejected .*command|\[cross-record\] FAIL|\[net-ui-probe\] FAIL|\[net-match-service-e2e\].*(?:FAIL|setup failed)|\[net-plane\].*ASSERT|\[fullstate(?:-refusal)?\].*(?:failed:|refused:|problem=)|Desync:|desync at|admission refused|\[Lua error\]|Segmentation fault', line, re.I):
                findings.append(dict(peer=name, path=str(log_path.relative_to(root)), line=number, text=line.strip()))
        raw_path = own / 'engine/feel/raw.jsonl'; raw = list(rows(raw_path))
        presentation_window=load(raw_path.with_name('raw.index.json'),{})
        for row in raw:
            if row.get('type')=='malformed_record': findings.append(dict(peer=name,path=str(raw_path.relative_to(root)),line=row['_line'],text=row['error']))
        frames = [r for r in raw if r.get('type') == 'frame' and r.get('active')]
        inputs = [r for r in raw if r.get('type') == 'input']
        latency = report.input_latencies(inputs, frames) if inputs and frames else []
        configs = [r for r in events[name] if r.get('type') == 'adopted_config']
        initial = configs[0] if configs else {}
        native_steps = []
        def visit(value):
            if isinstance(value, dict):
                if 'missing_frame_stalls' in value and 'next_frame' in value: native_steps.append(value)
                for nested in value.values(): visit(nested)
            elif isinstance(value, list):
                for nested in value: visit(nested)
        visit(native)
        stall_values=[r.get('steady_missing_frame_stalls') for r in native_steps]
        stalls=sum(stall_values) if stall_values and all(isinstance(v,(int,float)) for v in stall_values) else None
        clock = [r for r in live[name] if r.get('phase') == 'live']
        timing = report.reduce_net_window(clock, waits, 300, manifest['ticks'], initial.get('sim_tick_ms'), stalls) if len(configs) <= 1 else {'complete': False, 'reason': 'multiple rounds require explicit segmented windows'}
        timing['sim_ms_per_tick'] = native.get('pace', {}).get('sim_ms_per_tick')
        tick_cost = [r for r in events[name] if r.get('type') == 'tick_timing' and r.get('phase') == 'live']
        sample_root = root if next(b for b in manifest['boxes'] if b['name'] == spec['box'])['kind'] == 'windows-local' else root / 'boxes' / spec['box']
        samples = [r for r in rows(sample_root / 'samples.jsonl') if r['peer'] == name]
        recovery_observations=[r for r in source_rows(sample_root/'recovery-observed.jsonl',root) if r.get('peer')==name]
        payload_done=load(sample_root/'done.json',{})
        exits=[judge_exit(load(fragment/'record.json',{}),name,int(fragment.name.split('-')[-1]),manifest['faults'],recovery_observations)
               for fragment in fragments]
        under_load = any(r.get('load') or r.get('same_box_instances', 1) > 1 for r in samples)
        preflight = manifest.get('preflights', {}).get(spec['box'], {})
        under_load |= bool(preflight.get('load'))
        quiet = manifest.get('quiet_window', False) and not under_load and bool(samples)
        feel_pins = [timing.get('steady_wall_tps') is not None and timing['steady_wall_tps'] >= 59.5,
                     timing.get('waiting_percent') is not None and timing['waiting_percent'] < 1,
                     timing.get('longest_stall_ms') is not None and timing['longest_stall_ms'] <= 50,
                     timing.get('confirmed_horizon_lag_ms') is not None and timing['confirmed_horizon_lag_ms'] <= 50]
        memory_by_incarnation={}
        for fragment in fragments:
            incarnation=int(fragment.name.split('-')[-1])
            fragment_record=load(fragment/'record.json',{})
            memory_by_incarnation[str(incarnation)]=report.reduce_memory([r for r in samples if r.get('incarnation',0)==incarnation],
                **manifest['memory'],elapsed_s=fragment_record.get('elapsed_seconds',0))
        memory=memory_by_incarnation[str(int(own.name.split('-')[-1]))]
        archives=[r for fragment in fragments for r in source_rows(fragment/'archives.jsonl',root)]
        payload_sizes = [r['trace_vector_payload_bytes'] for r in events[name]
                         if r.get('type') == 'tick_timing' and 'trace_vector_payload_bytes' in r]
        instrumentation = dict(first_bytes=payload_sizes[0] if payload_sizes else None,
            last_bytes=payload_sizes[-1] if payload_sizes else None, peak_bytes=max(payload_sizes) if payload_sizes else None,
            growth_bytes=payload_sizes[-1]-payload_sizes[0] if payload_sizes else None,
            scope='Measured trace vector and string capacities only; allocator, map nodes and other buffers excluded. No subtraction from resident/private totals.')
        trace=load(own/'trace.json',{})
        completion=trace.get('runs',[{}])[-1].get('strings',{}) if trace.get('runs') else {}
        final_tick=trace.get('runs',[{}])[-1].get('numeric',{}).get('final_tick') if trace.get('runs') else None
        peers[name] = dict(box=spec['box'], role=spec['role'], instance=name, incarnation=int(own.name.split('-')[-1]), frames=len(live[name]),
            observed_waits_over_50=sum(r['wait_ms']>50 for r in waits) if log else None, observed_wait_records=len(waits),
            native=native, record=record, timing=timing, presentation_window=presentation_window, tick_compute_ms=report.distribution([r['compute_us']/1000 for r in tick_cost]),
            tick_timing_valid=bool(tick_cost) and all(r.get('partition_valid') for r in tick_cost),
            capture_ms=report.distribution([r['capture_us']/1000 for r in tick_cost]),
            latency_ms=report.distribution([r['ms'] for r in latency if r['ms'] is not None]),
            latency_lower_bounds_ms=report.distribution([r['latency_lower_bound_ms'] for r in latency]),
            latency_unreflected=sum(r['ms'] is None for r in latency),
            draw_ms=report.distribution([r['draw_ms'] for r in frames]), present_ms=report.distribution([r['present_ms'] for r in frames]),
            frame_interval_ms=report.distribution([r['interval_ms'] for r in frames if r.get('interval_ms') is not None]),
            frame_count=len(frames), frames_over_50_ms=[r['frame'] for r in frames if max(r['draw_ms'], r['present_ms'], r.get('interval_ms') or 0) > 50],
            effective_hz=(len(frames)-1)*1000/(frames[-1]['present_end_ms']-frames[0]['present_end_ms']) if len(frames)>1 and frames[-1]['present_end_ms']>frames[0]['present_end_ms'] else None,
            memory=memory, memory_by_incarnation=memory_by_incarnation, instrumentation=instrumentation, archives=archives,
            recovery_observations=recovery_observations,payload_clock_last_ms=max([payload_done.get('payload_monotonic_ms',0),*[r.get('payload_monotonic_ms',0) for r in samples],*[r.get('upper_wall_ms',0) for r in recovery_observations]]),
            native_completion=completion, native_final_tick=final_tick, exits=exits,
            fragments=[str(fragment.relative_to(root)) for fragment in fragments], samples=samples, holds=holds, feel_status='PASS' if quiet and all(feel_pins) else 'FAIL' if quiet else 'UNDER LOAD' if under_load else 'REPORTED; quiet window not scheduled',
            feel_gated=quiet, feel_pass=all(feel_pins), wire_egress=None,
            wire_reason='Transport wire counters are not exposed at an owned seam; GnsTransport.cpp:904 detailed-status text is not a per-tick counter API.',
            configs=configs, paths={kind: str(record_path(own / leaf).relative_to(root)) for kind,leaf in [('live','live.jsonl'),('events','events.jsonl'),('log','engine/stdout.log'),('feel','engine/feel/raw.jsonl'),('native','match-report.json')]})
    host_rows = live.get(manifest['host'], [])
    ranges, missing_boundaries = report.declared_history_ranges(host_rows,
        [r for r in events.get(manifest['host'],[]) if r.get('type')=='match_boundary'], peers,
        smoke_ticks=manifest['ticks'] if manifest['scenario']=='match' else None,
        final_tick=peers[manifest['host']]['native_final_tick'])
    comparison = report.compare_histories(live, ranges, REQUIRED_SUBSYSTEMS)
    fullstate_documents={name:report.parse_fullstate([root/fragment/'engine/stdout.log' for fragment in peer['fragments']]) for name,peer in peers.items()}
    fullstate_expected=set()
    cadence=manifest.get('fullstate_every',0)
    if cadence:
        first_eligible={}
        for observed in host_rows:
            if observed.get('phase')!='live' or observed.get('paused') or not observed.get('gameplay_tick') or not observed.get('effective_start_frame'):
                continue
            if observed['tick'] < observed['effective_start_frame']: continue
            round_id=observed['round']; first_eligible.setdefault(round_id,observed['tick'])
            if observed['tick'] % cadence == 0: fullstate_expected.add((round_id,observed['tick'],'sample'))
        fullstate_expected.update((r,t,'sample') for r,t in first_eligible.items())
    fullstate=report.compare_fullstate_histories(fullstate_documents,sorted(fullstate_expected)) if cadence else dict(passed=False,status='NOT COVERED',reason='full-state instrumentation disabled')
    matrix = coverage(events, peers, manifest)
    fault_receipts = [dict(r, source_peer=name) for name,values in events.items() for r in values if r.get('type') == 'fault']
    fault_receipts += [dict(r['native'],source_peer=name,id=r['id'],type='fault',applied=True,source='owning payload termination')
        for name,p in peers.items() for r in p['recovery_observations'] if r.get('phase')=='fault_applied' and r.get('native',{}).get('action')=='crash-restart']
    faults_applied = all(any(r.get('id') == f['id'] and r.get('applied') for r in fault_receipts) for f in manifest['faults'])
    effects=[dict(r,source_peer=name) for name,values in events.items() for r in values if r.get('type')=='fault_effect']
    h4_effects=all(any(r.get('id')==f['id'] and r.get('source_peer')==f['peer'] and r.get('effect_observed') for r in effects)
                  for f in manifest['faults'] if f['action'] in ('ack-drop','ack-duplicate','commit-drop'))
    recovery_events=[r for peer in peers.values() for r in peer['recovery_observations']]
    native_recovery_records=[dict(r,source_peer=name) for name,values in events.items() for r in values if r.get('type')=='recovery']
    recoveries = []
    for fault in manifest['faults']:
        if fault['action']=='brain-eliminate': continue
        last_clock=peers[fault['peer']]['payload_clock_last_ms']
        recoveries += report.reduce_recoveries([fault], recovery_events, now_ms=last_clock)
    holds = sum(len(p['holds']) for p in peers.values())
    for p in peers.values():
        for hold in p['holds']: hold['scheduled_recovery_id']=scheduled_hold(hold,fault_receipts,recoveries)
    unscheduled_holds=sum(not h['scheduled_recovery_id'] for p in peers.values() for h in p['holds'])
    checks = dict(three_real_boxes=len({manifest.get('preflights',{}).get(p['box'],{}).get('machine_id',p['box']) for p in peers.values() if p['record'].get('started')}) >= 3,
                  preflight_complete=len(manifest.get('preflights', {})) == len(manifest['boxes']) and not manifest.get('driver_findings'),
                  full_history=comparison['passed'] and not missing_boundaries, zero_desync=comparison['unequal_keys'] == 0 and bool(ranges),
                  zero_unscheduled_holds=unscheduled_holds == 0, native_completion=all(p['record'].get('exit_code') == 0 and
                      not p['record'].get('timed_out') and p['native'].get('exit_code') == 0 and p['native_completion'].get('completion') == 'completed' for p in peers.values()),
                  adopted_peer_count=all(p['configs'] and all(c['peer_count']==len(manifest['instances']) for c in p['configs']) for p in peers.values()),
                  adopted_roster=all(roster_matches(manifest,p['configs']) for p in peers.values()),
                  faults_applied=faults_applied, bounded_recovery=all(r['passed'] for r in recoveries),
                  native_fault_effects=h4_effects,
                  native_desync_checks=all(p['native'].get('desync_check', {}).get('mismatches') == 0 and
                      p['native'].get('desync_check', {}).get('compares', 0) > 0 and
                      p['native']['desync_check'].get('compare_margin', -1) >= 0 for p in peers.values()),
                  quiet_feel=all(not p['feel_gated'] or p['feel_pass'] for p in peers.values()),
                  binary_admission_limit=all(c.get('peer_limit', 0) >= len(manifest['instances']) for c in capabilities.values()),
                  record_integrity=all(any(r.get('type') == 'tick_timing' for r in values) and
                      not any(r.get('type') == 'record_loss' or (r.get('type') == 'tick_timing' and not r.get('partition_valid')) for r in values)
                      for values in events.values()),
                  no_engine_findings=not findings)
    checks['all_incarnation_exits']=all(p['exits'] and all(e['passed'] for e in p['exits']) for p in peers.values())
    checks['shared_fullstate']=bool(cadence) and fullstate['passed']
    checks['capture_rows_resolved']=not manifest.get('capture_rows_pending',[1,2])
    barrier_receipts=[dict(r,source_peer=name) for name,values in events.items() for r in values if r.get('type')=='capture_barrier']
    checks['capture_barrier_outcomes']=all(any(r.get('source_peer')==b['peer'] and r.get('id')==b['id'] and
        r.get('capture_phase')==b['phase'] and r.get('capture_tick')==b['tick'] and r.get('outcome')=='released' and
        r.get('wait_ms',float('inf'))<=b['timeout_ms'] for r in barrier_receipts) for b in manifest.get('capture_barriers',[]))
    if manifest['scenario'] != 'match':
        checks['coverage_minima'] = all(r['status'] in ('PASS', 'NOT COVERED', 'NOT APPLICABLE') for r in matrix)
        checks['unique_gameplay_budget'] = all(max((r.get('budget_tick',0) for r in values if r.get('type')=='progress'),default=0)>=manifest['ticks'] for values in events.values())
        endings = [r for values in events.values() for r in values if r.get('type')=='elimination_outcome' and r.get('result')=='activity_over']
        checks['forced_end_during_hold'] = any(r.get('overlap_hold_at_end') for r in endings)
        witnesses=[dict(r,source_peer=name) for name,values in events.items() for r in values if r.get('type')=='recovery_match_end']
        checks['forced_end_during_transfer'] = any(r.get('trigger_phase')=='catch_up' and any(
            w.get('id')==r.get('id') and w.get('source_peer')==r.get('target_peer') and w.get('source_round')==r.get('source_round') and
            w.get('observed_tick',0)>=r.get('final_tick',1) and w.get('catchup_at_end') for w in witnesses) for r in endings)
        checks['changed_settings_rematch'] = all(changed_settings(p['configs']) for p in peers.values())
        checks['fog_on_match'] = all(any(c.get('fog') for c in p['configs']) for p in peers.values())
        checks['validated_autosave_archives'] = False
        checks['memory_bounds']=all(p['memory_by_incarnation'] and all(m['passed'] for m in p['memory_by_incarnation'].values()) for p in peers.values())
    for name,peer in peers.items():
        peer['observations']=len(live[name])
        peer['frames']=comparison['peers'][name]['present']
    judgment=judge_attempt(manifest,checks,peers,matrix,recoveries)
    rerun=f'python tools/cross_peers.py --host {manifest["host"]} --scenario {manifest["scenario"]} --ticks {manifest["ticks"]} --roster {manifest["roster"]} --scene "{manifest["scene"]}" --seed {manifest["seed"]} --fullstate-every {cadence} --out "{root.as_posix()}-capture-fixed"'
    result = dict(version=2, run=manifest['run'], passed=all(checks.values()), checks=checks, manifest=manifest,**judgment,
                  assigned_capture_rows={str(row):CAPTURE_ROWS[row] for row in manifest.get('capture_rows_pending',[1,2])},rerun_after_capture_fix=rerun,
                  peers=peers, comparison=comparison, declared_ranges=ranges, missing_boundaries=missing_boundaries,
                  fullstate=fullstate, fullstate_records=fullstate_documents,
                  coverage=matrix, recoveries=recoveries, fault_receipts=fault_receipts, capabilities=capabilities, barrier_receipts=barrier_receipts,
                  native_recovery_records=native_recovery_records,native_fault_effects=effects,unscheduled_holds=unscheduled_holds,
                  findings=findings, requirements=requirements(manifest, comparison, peers))
    (root / 'result.json').write_text(json.dumps(result, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    write_page(root, result, events)
    write_index(root.parent)
    verdict = f'{attempt_label(result)} {result["run"]}: ' + ', '.join(f'{n}={p["frames"]} frames' for n,p in peers.items()) + f'; unequal={comparison["unequal_keys"]} unknown={comparison["unknown_keys"]} holds={holds} unscheduled={unscheduled_holds}; ' + '; '.join(name+'='+oracle['status'] for name,oracle in result['oracles'].items())
    if result['assigned_capture_rows']: verdict+='; full-state NOT COVERED by capture-rows lane rows '+','.join(result['assigned_capture_rows'])
    (root / 'verdict.txt').write_text(verdict + '\n', encoding='utf-8'); print(verdict)
    return result


def value(value):
    if value is None: return 'N/A'
    if isinstance(value, float): return f'{value:.3f}'
    return str(value)


def escape(value): return html.escape(str(value), quote=True)


def chart(result, events):
    width, height, budget = 760, 180, result['manifest']['ticks']
    colors = ['#61cbbf', '#eba873', '#a7a5ff', '#db89b4', '#b9d782']
    parts = [f'<svg viewBox="0 0 {width} {height}" role="img" aria-label="Compute milliseconds by committed tick">']
    maxima = [r['compute_us']/1000 for rows_ in events.values() for r in rows_ if r.get('type') == 'tick_timing']
    ceiling = max(1,max(maxima, default=1))
    for index, (peer, rows_) in enumerate(events.items()):
        segments = defaultdict(list)
        for row in rows_:
            if row.get('type') == 'tick_timing' and row.get('phase') == 'live' and not row.get('paused'):
                segments[(row.get('source_round'),row.get('execution'),row.get('incarnation'))].append(row)
        for identity,timed in segments.items():
            selected = timed[::max(1,len(timed)//400)]
            points = ' '.join(f'{20+min(r.get("budget_tick",r["tick"]),budget)/budget*720:.1f},{150-min(r["compute_us"]/1000/ceiling,1)*125:.1f}' for r in selected)
            parts.append(f'<polyline fill="none" stroke="{colors[index%len(colors)]}" stroke-width="1.5" points="{points}"><title>{escape(peer)} {escape(identity)}</title></polyline>')
    parts.append(f'<text x="20" y="175" fill="#b9c5d3" font-size="12">0 · unique gameplay budget (separate round segments) · {budget:,} ticks</text></svg>')
    return ''.join(parts)


def write_page(root, result, events):
    manifest = result['manifest']; parts = [f'<h1>{escape(result["run"])}</h1><p class="verdict {"pass" if result["passed"] else "fail"}">{escape(attempt_label(result))}</p>',
        f'<p>{len(result["peers"])} peers on {len(manifest["boxes"])} boxes · host {escape(manifest["host"])} · {manifest["ticks"]:,} committed ticks requested.</p>',
        '<p>Correctness gates apply throughout. Feel gates apply only in a declared quiet window with measured load absent. UNKNOWN history is never counted as equal.</p>',
        '<p>Quiet feel pins: at least 59.5 TPS, waiting below 1%, longest measured wait at most 50 ms, and nominal-dt horizon drift at most 50 ms. The steady interval is anchored at tick 300; only waits after that tick and through the declared last tick enter its denominator. Other observed waits remain visible.</p>',
        f'<p>Recovery deadline {manifest["deadlines"]["recovery_ms"]:,} ms · capture budget {manifest["deadlines"]["capture_ms"]} ms. Memory warm-up {manifest["memory"]["warmup_s"]} s, slope bound {manifest["memory"]["slope_bytes_per_minute"]:,} B/min, retention bound {manifest["memory"]["retained_bytes"]:,} B. Raw sizes are never reduced by unmeasured instrumentation.</p>',
        '<div class="cards">']
    for name, peer in result['peers'].items():
        pairs = [('Frames',peer['frames']),('Feel',peer['feel_status']),('Steady TPS',peer['timing'].get('steady_wall_tps')),
                 ('All observed waits over 50 ms',peer['observed_waits_over_50']),('Steady waits over 50 ms',peer['timing'].get('steady_waits_over_50')),('Missing-frame stalls',peer['timing'].get('steady_missing_frame_stalls')),
                 ('Longest wait ms',peer['timing'].get('longest_stall_ms')),('Waiting %',peer['timing'].get('waiting_percent')),
                 ('Horizon drift ms',peer['timing'].get('confirmed_horizon_lag_ms')),('Compute p50 / p99 / max ms',' / '.join(value(peer['tick_compute_ms'][k]) for k in ('p50','p99','max'))),
                 ('Submitted Hz',peer['effective_hz']),('Submitted frames',peer['frame_count']),('Intervals over 50 ms',len(peer['frames_over_50_ms'])),
                 ('Own movement/aim latency p99 ms',peer['latency_ms']['p99']),('Unreflected input edges',peer['latency_unreflected'])]
        parts.append(f'<article><h2>{escape(name)} · {escape(peer["box"])}</h2><dl>' + ''.join(f'<dt>{escape(k)}</dt><dd>{escape(value(v))}</dd>' for k,v in pairs) +
                     f'</dl><p><a href="{escape(peer["paths"]["live"])}">Live hashes</a> · <a href="{escape(peer["paths"]["events"])}">Events</a> · <a href="{escape(peer["paths"]["log"])}">Log</a> · <a href="{escape(peer["paths"]["feel"])}">Feel samples</a></p></article>')
    parts.append('</div><h2>Tick compute time</h2>' + chart(result, events) + '<p>Samples measure non-overlapping compute, wait and capture durations. Cross-box wall-clock calibration is NOT COVERED; ticks align canonical events, and local clocks remain separate.</p>')
    parts.append('<h2>Gates</h2><ul>' + ''.join(f'<li class="{"pass" if ok else "fail"}">{escape(name)}: {"PASS" if ok else "FAIL"}</li>' for name,ok in result['checks'].items()) + '</ul>')
    parts.append('<h2>Independent oracle verdicts</h2><p>CORE PASS requires complete comparable live frames, zero live mismatches, zero unscheduled holds and native completion, with the box/configuration/integrity prerequisites. It does not turn any other oracle green.</p>'+
        ''.join(f'<details><summary>{escape(name)} — {escape(row["status"])}</summary><p>{escape(row["reason"])}</p></details>' for name,row in result['oracles'].items()))
    parts.append('<h2>Coverage</h2>')
    parts.append('<div style="overflow-x:auto"><table><thead><tr><th>Coverage / minimum</th>' +
        ''.join(f'<th>{escape(b["name"])}</th>' for b in manifest['boxes']) + '</tr></thead><tbody>')
    for row in result['coverage']:
        parts.append(f'<tr><th>{escape(row["item"])}<br>{row["status"]}<br>{row["minimum"]} · {escape(row["unit"])}</th>')
        for box in manifest['boxes']:
            values = {name: row['peers'][name] for name,p in result['peers'].items() if p['box'] == box['name']}
            parts.append('<td><pre>' + escape(json.dumps(values,indent=2)) + '</pre></td>')
        parts.append('</tr>')
    parts.append('</tbody></table></div>')
    for row in result['coverage']:
        parts.append(f'<details><summary>{escape(row["item"])} — {row["status"]}</summary><p>{escape(row["unit"])}; minimum {row["minimum"]}. {escape(row["reason"])}</p><pre>{escape(json.dumps(row["peers"],indent=2))}</pre></details>')
    parts.append('<h2>Faults and recovery</h2><p>Queued admission and cancelled reclaim are phase evidence, never completed recovery. Durations use the owning payload clock across engine incarnations; the deadline judges the conservative upper duration, not subtraction between engine clocks. Native phase/input evidence stays separate. The chaos seed fixes choices only.</p><pre>' + escape(json.dumps(dict(seed=manifest['chaos_seed'],schedule=manifest['faults'],applied=result['fault_receipts'],recoveries=result['recoveries'],native_evidence=result['native_recovery_records']),indent=2)) + '</pre>')
    parts.append('<p>H4 effects require the native substitution log and a reset receipt; arming alone is insufficient. If no substitution ack is sent, NetReconnectSession.cpp:2513-2515 remains an unexercised engine seam.</p><pre>'+escape(json.dumps(result['native_fault_effects'],indent=2))+'</pre>')
    parts.append('<h2>Wire egress</h2><p>NOT COVERED. Transport wire counters are not exposed at an owned seam. Application bytes and host relayed bytes are not wire egress; no upstream curve is fabricated.</p>')
    parts.append('<h2>Capture and writer barriers</h2><p>Each arm has its own declared timeout; timeout is a failed outcome.</p><pre>' + escape(json.dumps(dict(schedule=manifest.get('capture_barriers',[]),receipts=result['barrier_receipts']),indent=2)) + '</pre>')
    parts.append('<h2>Retained autosaves</h2><p>CRCs, required entries and descriptor/world/manifest identities are checked. Archive restoration and sealed admission remain separate assertions; file integrity alone does not satisfy them.</p><pre>' + escape(json.dumps({name:p['archives'] for name,p in result['peers'].items()},indent=2)) + '</pre>')
    parts.append('<h2>Admission limits reported by each binary</h2><pre>' + escape(json.dumps(result['capabilities'],indent=2)) + '</pre>')
    parts.append('<h2>Memory and measured instrumentation</h2><p>Finite samples judge the declared bounds; they do not prove the absence of leaks. Resident/working-set and virtual/private bytes remain separate.</p>')
    for name,peer in result['peers'].items():
        parts.append(f'<details><summary>{escape(name)} — raw samples and per-incarnation judgment</summary><pre>' + escape(json.dumps(dict(
            samples=peer['samples'],bounds=peer['memory_by_incarnation'],instrumentation=peer['instrumentation']),indent=2)) + '</pre></details>')
    parts.append('<h2>Full-state scope and cadence</h2><p>Every listed Shared section is compared. Native PerPeer exclusions are retained below; their behavioral restoration remains a separate requirement.</p><pre>'+escape(json.dumps(dict(verdict=result['fullstate'],records=result['fullstate_records']),indent=2))+'</pre>')
    if result['assigned_capture_rows']:
        parts.append('<p class="fail">Mandatory full-state evidence remains RED / NOT COVERED by these assigned engine rows. No sample, refusal or difference is masked.</p><ul>'+''.join('<li>'+escape(reason)+'</li>' for reason in result['assigned_capture_rows'].values())+'</ul>')
    parts.append('<p>After the capture lane fixes are included in this build, rerun:</p><pre>'+escape(result['rerun_after_capture_fix'])+'</pre>')
    parts.append('<h2>Engine and driver findings</h2>')
    for finding in result['findings']:
        parts.append('<p class="fail">' + (f'<a href="{escape(finding["path"])}">{escape(finding["path"])}:{finding["line"]}</a> ' if 'path' in finding else '') + escape(finding.get('text',finding.get('reason',''))) + '</p>')
    parts.append('<h2>Numbered requirements</h2><p>Each verdict number retains its complete original assertion set. Partial observations do not satisfy the whole requirement.</p>')
    for item in result['requirements']:
        parts.append(f'<details id="requirement-{escape(item["number"])}"><summary>{escape(item["number"])} — {item["status"]}</summary><p>{escape(item["reason"])}</p><p>{escape(item["requirement"])}</p><p>{escape(item["reread"])}</p></details>')
    shown_manifest={**manifest,'preflights':{name:{**{k:v for k,v in preflight.items() if k!='content'},
        'content_files':len(preflight.get('content',{})),
        'content_manifest_sha256':hashlib.sha256(json.dumps(preflight.get('content',{}),sort_keys=True).encode()).hexdigest()}
        for name,preflight in manifest.get('preflights',{}).items()}}
    parts.append('<details><summary>Manifest, builds, config, load and raw metrics</summary><pre>' + escape(json.dumps(dict(manifest=shown_manifest, peers=result['peers'], comparison=result['comparison']),indent=2)) + '</pre></details><p><a href="manifest.json">Complete manifest and per-file content hashes</a> · <a href="result.json">All reduced evidence</a> · <a href="../index.html">Run index</a></p>')
    document = '<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>' + escape(result['run']) + '</title><style>body{margin:0;background:#101924;color:#e1e9f0;font:16px/1.5 system-ui,sans-serif}main{max-width:1160px;margin:auto;padding:24px 16px}h1{font-size:clamp(24px,5vw,42px);overflow-wrap:anywhere}h2{font-size:21px;margin-top:28px}a{color:#83d4ff}.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,290px),1fr));gap:16px}article,details{background:#1c2938;border:1px solid #35465b;border-radius:10px;padding:16px;margin:12px 0}dl{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1fr);gap:6px}dt,dd{margin:0;overflow-wrap:anywhere}dd{text-align:right}pre{white-space:pre-wrap;overflow-wrap:anywhere;font:12px/1.5 ui-monospace,monospace}summary{cursor:pointer;font-weight:650}.pass{color:#80dcc0}.fail{color:#ff9b93}.verdict{font-size:24px;font-weight:750}svg{width:100%;background:#192635;border-radius:10px}li{overflow-wrap:anywhere}*{box-sizing:border-box}</style><main>' + ''.join(parts) + '</main></html>'
    (root / 'report.html').write_text(document,encoding='utf-8')


def write_index(root):
    items = []
    for path in root.glob('*/result.json'):
        try:
            result = load(path)
            if 'requirements' not in result: continue
            items.append((result.get('manifest',{}).get('started',''), path.parent.name, result))
        except (ValueError,OSError): continue
    text = '<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Multi-box runs</title><style>body{font:16px system-ui;background:#101924;color:#e1e9f0;padding:16px;max-width:900px;margin:auto}a{color:#83d4ff}li{margin:24px 0;overflow-wrap:anywhere}</style><h1>Multi-box runs</h1><ul>'
    for _, name, result in sorted(items, reverse=True):
        missing = ', '.join(str(r['number']) for r in result['requirements'] if r['status'] != 'PASS')
        text += f'<li><a href="{escape(name)}/report.html">{escape(name)}</a> — {escape(attempt_label(result))}<br>NOT COVERED verdict numbers: {escape(missing)}</li>'
    (root / 'index.html').write_text(text+'</ul>',encoding='utf-8')


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__); parser.add_argument('root',type=Path)
    result=build_report(parser.parse_args().root)
    raise SystemExit(0 if (result.get('gate_b_eligible') if result['manifest']['scenario']=='match' else result['passed']) else 1)
