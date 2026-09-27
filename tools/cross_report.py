"""Build a self-contained, evidence-linked multi-box report and its run index."""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import html
import json
from pathlib import Path
import re

from feel import report
from feel.records import open_record, record_path
from compare_sim_traces import CORE

HERE = Path(__file__).resolve().parent
REQUIRED_SUBSYSTEMS = CORE | {'controller'}


def load(path, default=None):
    return json.loads(Path(path).read_text(encoding='utf-8-sig')) if Path(path).is_file() else default


def rows(path):
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
            selected = [event for event in events[peer] if event.get('type') == 'coverage' and event.get('phase') == 'live']
            successes = {kind: sum(event.get('amount', 1) for event in selected if event.get('event') == kind
                                   and event.get('result', 'success') in ('success', 'orphan', 'penetrate', 'penetrate_air', 'dislodge', 'silhouette', 'unattributed', 'health_exhausted_unattributed', 'death_timer', 'committed')) for kind in kinds}
            counts[peer] = dict(successes=successes, attempts=sum(event.get('result') == 'attempt' for event in selected if event.get('event') in kinds))
        status = 'NOT COVERED' if reason else 'PASS' if all(c['successes'] and all(v >= minimum for v in c['successes'].values()) for c in counts.values()) else 'FAIL'
        result.append(dict(id=key, item=label, status=status, reason=reason, unit=unit, minimum=minimum, peers=counts))
    return result


def requirements(manifest, comparison, metrics):
    items = load(HERE / 'cross_peers/requirements.json', [])
    reasons = {
        2: 'WAN migration requires NetMatchService.cpp:8244 endpoint publication and NetLockstep.cpp:3304,3334 dialing changes owned by the catch-up lane.',
        3: 'MIXED uses human teams 0,0,1 plus peerless CPU team 2 through real host options; ordinary AI actors supply allied units. Adopted configurations and all roster arms still need run evidence.',
        20: 'SoundContainer.cpp:405 authority and music-transition emissions are outside this lane; muted output proves no audible result.',
        22: 'A fourth real box and its persistent-world arrival arm are absent.',
        35: 'Pre-auth/proof/image/tail/activation overlaps await the WAN endpoint fix; queued/cancelled phases never complete recovery.',
        36: 'Capture-announced and writer-pending barriers have RED/GREEN bounded-release tests; migration overlap, archive validity and restarted writer cadence still await a real endpoint-capable arm.',
        40: 'Initial histories are compared. Restore branch/checkpoint lineage and authority generation are not exposed by the owned record seams.',
        45: 'Checkpoint boot/handover anchors and survivor segment indexing need ScenarioRunner.cpp:2653 and NetMatchService.cpp:3455 outside this lane.',
        50: 'Movement/aim submitted-render measurements are reported; other action/input-sequence stamps require FrameMan.cpp:277,344 outside this lane.',
        51: 'Remote-unit render discontinuities require FrameMan/LocalPrediction records outside this lane; local corrections retain their own labels.',
        53: 'Loss-to-first-controllable-input phase identity needs NetMatchService.cpp:939,5319 and NetWorldJoin.cpp:1139; aggregate catch-up is not a completed recovery oracle.',
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
        waits = []; holds = []
        for log_path, number, line in log:
            if match := re.search(r'\[net-frame-wait\] frame=(\d+) wait_ms=(\d+)', line):
                waits.append(dict(tick=int(match[1]), wait_ms=int(match[2]), line=number))
            if match := re.search(r'\[net-match\] hold peer=(\d+) frame=(\d+)', line):
                holds.append(dict(peer=int(match[1]), tick=int(match[2]), line=number))
            if re.search(r'RTE Assert|FATAL:|\[cross-record\] FAIL|\[net-ui-probe\] FAIL|\[net-match-service-e2e\] FAIL|Desync:|desync at|admission refused|\[Lua error\]|Segmentation fault', line, re.I):
                findings.append(dict(peer=name, path=str(log_path.relative_to(root)), line=number, text=line.strip()))
        raw_path = own / 'engine/feel/raw.jsonl'; raw = list(rows(raw_path))
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
        stalls = sum(r['steady_missing_frame_stalls'] for r in native_steps if 'steady_missing_frame_stalls' in r) if native_steps else None
        clock = [r for r in live[name] if r.get('phase') == 'live']
        timing = report.reduce_net_window(clock, waits, 300, manifest['ticks'], initial.get('sim_tick_ms'), stalls) if len(configs) <= 1 else {'complete': False, 'reason': 'multiple rounds require explicit segmented windows'}
        timing['sim_ms_per_tick'] = native.get('pace', {}).get('sim_ms_per_tick')
        tick_cost = [r for r in events[name] if r.get('type') == 'tick_timing' and r.get('phase') == 'live']
        sample_root = root if next(b for b in manifest['boxes'] if b['name'] == spec['box'])['kind'] == 'windows-local' else root / 'boxes' / spec['box']
        samples = [r for r in rows(sample_root / 'samples.jsonl') if r['peer'] == name]
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
        peers[name] = dict(box=spec['box'], role=spec['role'], instance=name, incarnation=spec['incarnation'], frames=len(live[name]),
            native=native, record=record, timing=timing, tick_compute_ms=report.distribution([r['compute_us']/1000 for r in tick_cost]),
            capture_ms=report.distribution([r['capture_us']/1000 for r in tick_cost]),
            latency_ms=report.distribution([r['ms'] for r in latency if r['ms'] is not None]),
            latency_lower_bounds_ms=report.distribution([r['latency_lower_bound_ms'] for r in latency]),
            latency_unreflected=sum(r['ms'] is None for r in latency),
            draw_ms=report.distribution([r['draw_ms'] for r in frames]), present_ms=report.distribution([r['present_ms'] for r in frames]),
            frame_interval_ms=report.distribution([r['interval_ms'] for r in frames if r.get('interval_ms') is not None]),
            frame_count=len(frames), frames_over_50_ms=[r['frame'] for r in frames if max(r['draw_ms'], r['present_ms'], r.get('interval_ms') or 0) > 50],
            effective_hz=(len(frames)-1)*1000/(frames[-1]['present_end_ms']-frames[0]['present_end_ms']) if len(frames)>1 and frames[-1]['present_end_ms']>frames[0]['present_end_ms'] else None,
            memory=memory, memory_by_incarnation=memory_by_incarnation, instrumentation=instrumentation, archives=archives,
            native_completion=completion, native_final_tick=final_tick,
            fragments=[str(fragment.relative_to(root)) for fragment in fragments], samples=samples, holds=holds, feel_status='PASS' if quiet and all(feel_pins) else 'FAIL' if quiet else 'UNDER LOAD' if under_load else 'REPORTED; quiet window not scheduled',
            feel_gated=quiet, feel_pass=all(feel_pins), wire_egress=None,
            wire_reason='Transport wire counters are not exposed at an owned seam; GnsTransport.cpp:894 detailed status is not a byte counter.',
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
    faults_applied = all(any(r.get('id') == f['id'] and r.get('applied') for r in fault_receipts) for f in manifest['faults'])
    recovery_events = [dict(id=r['id'], peer=r['source_peer'], incarnation=r['incarnation'], phase='fault_applied',
        wall_ms=r['applied_wall_ms'], clock_domain='engine_steady_clock', source=dict(path=r['_path'],line=r['_line']))
        for r in fault_receipts if r.get('applied') and r.get('applied_wall_ms') is not None]
    # A progress tick is not proof of control restoration. Only explicit terminal records may complete an arm.
    recovery_events += [dict(r,peer=name) for name,values in events.items() for r in values if r.get('type') == 'recovery']
    recoveries = []
    for fault in manifest['faults']:
        last_clock = max((r.get('wall_ms',0) for r in events.get(fault['peer'],[])), default=0)
        recoveries += report.reduce_recoveries([fault], recovery_events, now_ms=last_clock)
    holds = sum(len(p['holds']) for p in peers.values())
    checks = dict(three_real_boxes=len({manifest.get('preflights',{}).get(p['box'],{}).get('machine_id',p['box']) for p in peers.values() if p['record'].get('started')}) >= 3,
                  preflight_complete=len(manifest.get('preflights', {})) == len(manifest['boxes']) and not manifest.get('driver_findings'),
                  full_history=comparison['passed'] and not missing_boundaries, zero_desync=comparison['unequal_keys'] == 0 and bool(ranges),
                  zero_unscheduled_holds=holds == 0, native_completion=all(p['record'].get('exit_code') == 0 and
                      not p['record'].get('timed_out') and p['native'].get('exit_code') == 0 and p['native_completion'].get('completion') == 'completed' for p in peers.values()),
                  adopted_peer_count=all(p['configs'] and all(c['peer_count']==len(manifest['instances']) for c in p['configs']) for p in peers.values()),
                  faults_applied=faults_applied, bounded_recovery=all(r['passed'] for r in recoveries),
                  native_desync_checks=all(p['native'].get('desync_check', {}).get('mismatches') == 0 and
                      p['native'].get('desync_check', {}).get('compares', 0) > 0 and
                      p['native']['desync_check'].get('compare_margin', -1) >= 0 for p in peers.values()),
                  quiet_feel=all(not p['feel_gated'] or p['feel_pass'] for p in peers.values()),
                  binary_admission_limit=all(c.get('peer_limit', 0) >= len(manifest['instances']) for c in capabilities.values()),
                  record_integrity=all(any(r.get('type') == 'tick_timing' for r in values) and
                      not any(r.get('type') == 'record_loss' or (r.get('type') == 'tick_timing' and not r.get('partition_valid')) for r in values)
                      for values in events.values()),
                  no_engine_findings=not findings)
    if cadence: checks['shared_fullstate']=fullstate['passed']
    barrier_receipts=[dict(r,source_peer=name) for name,values in events.items() for r in values if r.get('type')=='capture_barrier']
    checks['capture_barrier_outcomes']=all(any(r.get('source_peer')==b['peer'] and r.get('id')==b['id'] and
        r.get('capture_phase')==b['phase'] and r.get('capture_tick')==b['tick'] and r.get('outcome')=='released' and
        r.get('wait_ms',float('inf'))<=b['timeout_ms'] for r in barrier_receipts) for b in manifest.get('capture_barriers',[]))
    if manifest['scenario'] != 'match':
        checks['coverage_minima'] = all(r['status'] in ('PASS', 'NOT COVERED', 'NOT APPLICABLE') for r in matrix)
        checks['unique_gameplay_budget'] = all(max((r.get('budget_tick',0) for r in values if r.get('type')=='progress'),default=0)>=manifest['ticks'] for values in events.values())
        endings = [r for values in events.values() for r in values if r.get('type')=='elimination_outcome' and r.get('result')=='activity_over']
        checks['forced_end_during_hold'] = any(r.get('overlap_hold_at_end') for r in endings)
        checks['forced_end_during_transfer'] = any(r.get('overlap_transfer_at_end') for r in endings)
        checks['changed_settings_rematch'] = all(len({(c.get('difficulty'),c.get('fog'),c.get('config_hash')) for c in p['configs']})>1 for p in peers.values())
        checks['fog_on_match'] = all(any(c.get('fog') for c in p['configs']) for p in peers.values())
        checks['validated_autosave_archives'] = False
    for name,peer in peers.items():
        peer['observations']=len(live[name])
        peer['frames']=comparison['peers'][name]['present']
    result = dict(version=1, run=manifest['run'], passed=all(checks.values()), checks=checks, manifest=manifest,
                  peers=peers, comparison=comparison, declared_ranges=ranges, missing_boundaries=missing_boundaries,
                  fullstate=fullstate, fullstate_records=fullstate_documents,
                  coverage=matrix, recoveries=recoveries, fault_receipts=fault_receipts, capabilities=capabilities, barrier_receipts=barrier_receipts,
                  findings=findings, requirements=requirements(manifest, comparison, peers))
    (root / 'result.json').write_text(json.dumps(result, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    write_page(root, result, events)
    write_index(root.parent)
    verdict = f'{"PASS" if result["passed"] else "FAIL"} {result["run"]}: ' + ', '.join(f'{n}={p["frames"]} frames' for n,p in peers.items()) + f'; unequal={comparison["unequal_keys"]} unknown={comparison["unknown_keys"]} holds={holds}'
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
    manifest = result['manifest']; parts = [f'<h1>{escape(result["run"])}</h1><p class="verdict {"pass" if result["passed"] else "fail"}">{"PASS" if result["passed"] else "FAIL"}</p>',
        f'<p>{len(result["peers"])} peers on {len(manifest["boxes"])} boxes · host {escape(manifest["host"])} · {manifest["ticks"]:,} committed ticks requested.</p>',
        '<p>Correctness gates apply throughout. Feel gates apply only in a declared quiet window with measured load absent. UNKNOWN history is never counted as equal.</p>',
        f'<p>Recovery deadline {manifest["deadlines"]["recovery_ms"]:,} ms · capture budget {manifest["deadlines"]["capture_ms"]} ms. Memory warm-up {manifest["memory"]["warmup_s"]} s, slope bound {manifest["memory"]["slope_bytes_per_minute"]:,} B/min, retention bound {manifest["memory"]["retained_bytes"]:,} B. Raw sizes are never reduced by unmeasured instrumentation.</p>',
        '<div class="cards">']
    for name, peer in result['peers'].items():
        pairs = [('Frames',peer['frames']),('Feel',peer['feel_status']),('Steady TPS',peer['timing'].get('steady_wall_tps')),
                 ('Waits over 50 ms',peer['timing'].get('steady_waits_over_50')),('Missing-frame stalls',peer['timing'].get('steady_missing_frame_stalls')),
                 ('Longest wait ms',peer['timing'].get('longest_stall_ms')),('Waiting %',peer['timing'].get('waiting_percent')),
                 ('Horizon drift ms',peer['timing'].get('confirmed_horizon_lag_ms')),('Compute p50 / p99 / max ms',' / '.join(value(peer['tick_compute_ms'][k]) for k in ('p50','p99','max'))),
                 ('Submitted Hz',peer['effective_hz']),('Submitted frames',peer['frame_count']),('Intervals over 50 ms',len(peer['frames_over_50_ms'])),
                 ('Own movement/aim latency p99 ms',peer['latency_ms']['p99']),('Unreflected input edges',peer['latency_unreflected'])]
        parts.append(f'<article><h2>{escape(name)} · {escape(peer["box"])}</h2><dl>' + ''.join(f'<dt>{escape(k)}</dt><dd>{escape(value(v))}</dd>' for k,v in pairs) +
                     f'</dl><p><a href="{escape(peer["paths"]["live"])}">Live hashes</a> · <a href="{escape(peer["paths"]["events"])}">Events</a> · <a href="{escape(peer["paths"]["log"])}">Log</a> · <a href="{escape(peer["paths"]["feel"])}">Feel samples</a></p></article>')
    parts.append('</div><h2>Tick compute time</h2>' + chart(result, events) + '<p>Samples measure non-overlapping compute, wait and capture durations. Cross-box wall-clock calibration is NOT COVERED; ticks align canonical events, and local clocks remain separate.</p>')
    parts.append('<h2>Gates</h2><ul>' + ''.join(f'<li class="{"pass" if ok else "fail"}">{escape(name)}: {"PASS" if ok else "FAIL"}</li>' for name,ok in result['checks'].items()) + '</ul>')
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
    parts.append('<h2>Faults and recovery</h2><p>Queued admission and cancelled reclaim are phase evidence, never completed recovery. The chaos seed fixes choices only.</p><pre>' + escape(json.dumps(dict(seed=manifest['chaos_seed'], schedule=manifest['faults'], applied=result['fault_receipts'], recoveries=result['recoveries']),indent=2)) + '</pre>')
    parts.append('<h2>Wire egress</h2><p>NOT COVERED. Transport wire counters are not exposed at an owned seam. Application bytes and host relayed bytes are not wire egress; no upstream curve is fabricated.</p>')
    parts.append('<h2>Capture and writer barriers</h2><p>Each arm has its own declared timeout; timeout is a failed outcome.</p><pre>' + escape(json.dumps(dict(schedule=manifest.get('capture_barriers',[]),receipts=result['barrier_receipts']),indent=2)) + '</pre>')
    parts.append('<h2>Retained autosaves</h2><p>CRCs, required entries and descriptor/world/manifest identities are checked. Archive restoration and sealed admission remain separate assertions; file integrity alone does not satisfy them.</p><pre>' + escape(json.dumps({name:p['archives'] for name,p in result['peers'].items()},indent=2)) + '</pre>')
    parts.append('<h2>Admission limits reported by each binary</h2><pre>' + escape(json.dumps(result['capabilities'],indent=2)) + '</pre>')
    parts.append('<h2>Memory and measured instrumentation</h2><p>Finite samples judge the declared bounds; they do not prove the absence of leaks. Resident/working-set and virtual/private bytes remain separate.</p>')
    for name,peer in result['peers'].items():
        parts.append(f'<details><summary>{escape(name)} — raw samples and per-incarnation judgment</summary><pre>' + escape(json.dumps(dict(
            samples=peer['samples'],bounds=peer['memory_by_incarnation'],instrumentation=peer['instrumentation']),indent=2)) + '</pre></details>')
    parts.append('<h2>Full-state scope and cadence</h2><p>Every listed Shared section is compared. Native PerPeer exclusions are retained below; their behavioral restoration remains a separate requirement.</p><pre>'+escape(json.dumps(dict(verdict=result['fullstate'],records=result['fullstate_records']),indent=2))+'</pre>')
    parts.append('<h2>Engine and driver findings</h2>')
    for finding in result['findings']:
        parts.append('<p class="fail">' + (f'<a href="{escape(finding["path"])}">{escape(finding["path"])}:{finding["line"]}</a> ' if 'path' in finding else '') + escape(finding.get('text',finding.get('reason',''))) + '</p>')
    parts.append('<h2>Numbered requirements</h2><p>Each verdict number retains its complete original assertion set. Partial observations do not satisfy the whole requirement.</p>')
    for item in result['requirements']:
        parts.append(f'<details id="requirement-{escape(item["number"])}"><summary>{escape(item["number"])} — {item["status"]}</summary><p>{escape(item["reason"])}</p><p>{escape(item["requirement"])}</p><p>{escape(item["reread"])}</p></details>')
    parts.append('<details><summary>Manifest, builds, config, load and raw metrics</summary><pre>' + escape(json.dumps(dict(manifest=manifest, peers=result['peers'], comparison=result['comparison']),indent=2)) + '</pre></details><p><a href="result.json">All reduced evidence</a> · <a href="../index.html">Run index</a></p>')
    document = '<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>' + escape(result['run']) + '</title><style>body{margin:0;background:#101924;color:#e1e9f0;font:16px/1.5 system-ui,sans-serif}main{max-width:1160px;margin:auto;padding:24px 16px}h1{font-size:clamp(24px,5vw,42px);overflow-wrap:anywhere}h2{font-size:21px;margin-top:28px}a{color:#83d4ff}.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,290px),1fr));gap:16px}article,details{background:#1c2938;border:1px solid #35465b;border-radius:10px;padding:16px;margin:12px 0}dl{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1fr);gap:6px}dt,dd{margin:0;overflow-wrap:anywhere}dd{text-align:right}pre{white-space:pre-wrap;overflow-wrap:anywhere;font:12px/1.5 ui-monospace,monospace}summary{cursor:pointer;font-weight:650}.pass{color:#80dcc0}.fail{color:#ff9b93}.verdict{font-size:24px;font-weight:750}svg{width:100%;background:#192635;border-radius:10px}li{overflow-wrap:anywhere}*{box-sizing:border-box}</style><main>' + ''.join(parts) + '</main></html>'
    (root / 'report.html').write_text(document,encoding='utf-8')


def write_index(root):
    items = []
    for path in root.glob('*/result.json'):
        try:
            result = load(path)
            if 'requirements' not in result: continue
            items.append((path.stat().st_mtime, path.parent.name, result))
        except (ValueError,OSError): continue
    text = '<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Multi-box runs</title><style>body{font:16px system-ui;background:#101924;color:#e1e9f0;padding:16px;max-width:900px;margin:auto}a{color:#83d4ff}li{margin:24px 0;overflow-wrap:anywhere}</style><h1>Multi-box runs</h1><ul>'
    for _, name, result in sorted(items, reverse=True):
        missing = ', '.join(str(r['number']) for r in result['requirements'] if r['status'] != 'PASS')
        text += f'<li><a href="{escape(name)}/report.html">{escape(name)}</a> — {"PASS" if result["passed"] else "FAIL"}<br>NOT COVERED verdict numbers: {escape(missing)}</li>'
    (root / 'index.html').write_text(text+'</ul>',encoding='utf-8')


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__); parser.add_argument('root',type=Path)
    raise SystemExit(0 if build_report(parser.parse_args().root)['passed'] else 1)
