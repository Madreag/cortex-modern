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

HERE = Path(__file__).resolve().parent
REQUIRED_SUBSYSTEMS = {'controller', 'sim_rng', 'lua_state', 'scene', 'actors'}


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
        3: 'MIXED is refused: NetMatchConfig.cpp:647 prohibits a CPU slot sharing a human team; all roster arms are not proven.',
        20: 'SoundContainer.cpp:405 authority and music-transition emissions are outside this lane; muted output proves no audible result.',
        22: 'A fourth real box and its persistent-world arrival arm are absent.',
        35: 'Pre-auth/proof/image/tail/activation overlaps await the WAN endpoint fix; queued/cancelled phases never complete recovery.',
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
    if manifest.get('preflights') and len(manifest['preflights']) == len(manifest['boxes']) and metrics and all(p['record'].get('started') and p['samples'] for p in metrics.values()):
        for item in items:
            if item['number'] == 66:
                item.update(status='PASS', reason='Each box has one owning runner payload, local scripts and far-side engine PID samples.', evidence=['manifest.json'])
    return items


def build_report(root):
    root = Path(root).resolve(); manifest = load(root / 'manifest.json', {})
    live, events, peers, findings, paths = {}, {}, {}, list(manifest.get('driver_findings', [])), {}
    for spec in manifest['specs']:
        name = spec['peer']; first_own = peer_root(root, manifest, spec)
        fragments=sorted(first_own.parent.glob('incarnation-*'),key=lambda p:int(p.name.split('-')[-1])) or [first_own]
        own=fragments[-1]; paths[name] = own
        live[name] = [row for fragment in fragments for row in source_rows(fragment/'live.jsonl',root)]
        events[name] = [row for fragment in fragments for path in event_paths(fragment) for row in source_rows(path,root)]
        for observed in [*live[name],*events[name]]:
            if observed.get('type')=='malformed_record':
                findings.append(dict(peer=name,path=observed['_path'],line=observed['_line'],text='Malformed or truncated record: '+observed['error']))
        log = [(fragment/'engine/stdout.log',number,line) for fragment in fragments for number,line in read_log(fragment/'engine/stdout.log')]
        record, native = load(own / 'record.json', {}), load(own / 'match-report.json', {})
        waits = []; holds = []
        for log_path, number, line in log:
            if match := re.search(r'\[net-frame-wait\] frame=(\d+) wait_ms=(\d+)', line):
                waits.append(dict(tick=int(match[1]), wait_ms=int(match[2]), line=number))
            if match := re.search(r'\[net-match\] hold peer=(\d+) frame=(\d+)', line):
                holds.append(dict(peer=int(match[1]), tick=int(match[2]), line=number))
            if re.search(r'RTE Assert|\[cross-record\] FAIL|\[net-ui-probe\] FAIL|Desync:|desync at|admission refused|\[Lua error\]|Segmentation fault', line, re.I):
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
        trace=load(own/'trace.json',{})
        completion=trace.get('runs',[{}])[-1].get('strings',{}) if trace.get('runs') else {}
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
            memory=memory, memory_by_incarnation=memory_by_incarnation, native_completion=completion,
            fragments=[str(fragment.relative_to(root)) for fragment in fragments], samples=samples, holds=holds, feel_status='PASS' if quiet and all(feel_pins) else 'FAIL' if quiet else 'UNDER LOAD' if under_load else 'REPORTED; quiet window not scheduled',
            feel_gated=quiet, feel_pass=all(feel_pins), wire_egress=None,
            wire_reason='Transport wire counters are not exposed at an owned seam; GnsTransport.cpp:894 detailed status is not a byte counter.',
            configs=configs, paths={kind: str(record_path(own / leaf).relative_to(root)) for kind,leaf in [('live','live.jsonl'),('events','events.jsonl'),('log','engine/stdout.log'),('feel','engine/feel/raw.jsonl'),('native','match-report.json')]})
    host_rows = live.get(manifest['host'], [])
    ranges = []
    if host_rows:
        first = host_rows[0]
        if all(first.get(field) is not None for field in report.HISTORY_FIELDS[:-1]):
            ranges = [dict(**{field:first[field] for field in report.HISTORY_FIELDS[:-1]}, first=1, last=manifest['ticks'], peers=list(peers))]
    comparison = report.compare_histories(live, ranges, REQUIRED_SUBSYSTEMS)
    matrix = coverage(events, peers, manifest)
    fault_receipts = [r for values in events.values() for r in values if r.get('type') == 'fault']
    faults_applied = all(any(r.get('id') == f['id'] and r.get('applied') for r in fault_receipts) for f in manifest['faults'])
    recoveries = report.reduce_recoveries(manifest['faults'], [], now_ms=0)
    holds = sum(len(p['holds']) for p in peers.values())
    checks = dict(three_real_boxes=len({manifest.get('preflights',{}).get(p['box'],{}).get('machine_id',p['box']) for p in peers.values() if p['record'].get('started')}) >= 3,
                  preflight_complete=len(manifest.get('preflights', {})) == len(manifest['boxes']) and not manifest.get('driver_findings'),
                  full_history=comparison['passed'], zero_desync=comparison['unequal_keys'] == 0 and bool(ranges),
                  zero_unscheduled_holds=holds == 0, native_completion=all(p['record'].get('exit_code') == 0 and
                      not p['record'].get('timed_out') and p['native'].get('exit_code') == 0 and p['native_completion'].get('completion') == 'completed' for p in peers.values()),
                  adopted_peer_count=all(p['configs'] and all(c['peer_count']==len(manifest['instances']) for c in p['configs']) for p in peers.values()),
                  faults_applied=faults_applied, bounded_recovery=all(r['passed'] for r in recoveries),
                  quiet_feel=all(not p['feel_gated'] or p['feel_pass'] for p in peers.values()),
                  no_engine_findings=not findings)
    if manifest['scenario'] != 'match':
        checks['coverage_minima'] = all(r['status'] in ('PASS', 'NOT COVERED', 'NOT APPLICABLE') for r in matrix)
        checks['required_soak_evidence'] = False
    for name,peer in peers.items():
        peer['observations']=len(live[name])
        peer['frames']=comparison['peers'][name]['present']
    result = dict(version=1, run=manifest['run'], passed=all(checks.values()), checks=checks, manifest=manifest,
                  peers=peers, comparison=comparison, coverage=matrix, recoveries=recoveries, fault_receipts=fault_receipts,
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
    ceiling = max(maxima, default=1)
    for index, (peer, rows_) in enumerate(events.items()):
        timed = [r for r in rows_ if r.get('type') == 'tick_timing']
        stride = max(1, len(timed)//400)
        selected = timed[::stride]
        points = ' '.join(f'{20+min(r["tick"],budget)/budget*720:.1f},{150-min(r["compute_us"]/1000/ceiling,1)*125:.1f}' for r in selected)
        parts.append(f'<polyline fill="none" stroke="{colors[index%len(colors)]}" stroke-width="1.5" points="{points}"><title>{escape(peer)}</title></polyline>')
    parts.append(f'<text x="20" y="175" fill="#b9c5d3" font-size="12">0 ticks · shared committed-tick axis · {budget:,} ticks</text></svg>')
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
    for row in result['coverage']:
        parts.append(f'<details><summary>{escape(row["item"])} — {row["status"]}</summary><p>{escape(row["unit"])}; minimum {row["minimum"]}. {escape(row["reason"])}</p><pre>{escape(json.dumps(row["peers"],indent=2))}</pre></details>')
    parts.append('<h2>Faults and recovery</h2><p>Queued admission and cancelled reclaim are phase evidence, never completed recovery. The chaos seed fixes choices only.</p><pre>' + escape(json.dumps(dict(seed=manifest['chaos_seed'], schedule=manifest['faults'], applied=result['fault_receipts'], recoveries=result['recoveries']),indent=2)) + '</pre>')
    parts.append('<h2>Wire egress</h2><p>NOT COVERED. Transport wire counters are not exposed at an owned seam. Application bytes and host relayed bytes are not wire egress; no upstream curve is fabricated.</p>')
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
