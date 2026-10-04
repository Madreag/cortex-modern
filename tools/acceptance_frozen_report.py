"""Collect row inputs with the frozen cross reporter's unchanged reductions.

The native collection below is retained from the authorized wave's cross_report
prefix. Only its name and return surface differ; all shared reducers are imported
from the actual frozen module. The wrapper applies NOTE 14's explicit PC compiler
exception while retaining the raw load evidence and every timing threshold.
No frozen file or reporting function is modified.
"""
from cross_report import (Path, event_paths, judge_exit, load, pace_verdict, peer_root, presentation_contract, presentation_index, re, read_log, record_path, report, rows, source_rows)
from acceptance_native_load import apply_report_policy

COLLECTION_COMMIT = '4dd83eaa8bc50d98e090c9f1043a183fd66b6abe'
COLLECTION_SHA256 = '8fb732bf109aa3a4f6cd8d3dde949dc1de0404f6dc9aa140c443f97804b5d0d6'


def collect_native(root):
    root = Path(root).resolve(); manifest = load(root / 'manifest.json', {})
    live, events, peers, findings, paths = {}, {}, {}, list(manifest.get('driver_findings', [])), {}
    peer_waits = {}
    mixed_builds = []
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
        expected=manifest.get('preflights',{}).get(spec['box'],{}).get('executable_sha256')
        for fragment in fragments:
            runner=load(fragment/'record.json',{}) or load(fragment/'engine/launch.json',{})
            if not expected or runner.get('exe_sha256')!=expected:
                mixed_builds.append(f'{spec["box"]}: {name} incarnation {int(fragment.name.split("-")[-1])} ran executable sha256 '
                                    f'{runner.get("exe_sha256")} but the preflight hashed {expected}')
        live[name] = [row for fragment in fragments for row in source_rows(fragment/'live.jsonl',root)]
        events[name] = [row for fragment in fragments for path in event_paths(fragment) for row in source_rows(path,root)]
        for observed in [*live[name],*events[name]]:
            if observed.get('type')=='malformed_record':
                findings.append(dict(peer=name,path=observed['_path'],line=observed['_line'],text='Malformed or truncated record: '+observed['error']))
        log = [(fragment/'engine'/leaf,number,line) for fragment in fragments for leaf in ('stdout.log','stderr.log')
               for number,line in read_log(fragment/'engine'/leaf)]
        record, native = load(own / 'record.json', {}), load(own / 'match-report.json', {})
        waits = []; holds = []; own_hold_notifications=[]; observed_round=None; observed_native_round=None; observed_log=None
        for log_path, number, line in log:
            if log_path!=observed_log: observed_log=log_path; observed_round=None; observed_native_round=None
            if match := re.search(r'\[cross-context\] round=(\d+) source_round=(\d+)',line):
                observed_native_round=int(match[1]); observed_round=int(match[2])
            if match := re.search(r'\[net-frame-wait\] frame=(\d+) wait_ms=(\d+)', line):
                waits.append(dict(tick=int(match[1]), wait_ms=int(match[2]), line=number, round=observed_native_round, source_round=observed_round))
            if match := re.search(r'\[net-match\] hold peer=(\d+) frame=(\d+)', line):
                holds.append(dict(peer=int(match[1]), tick=int(match[2]), line=number,source_round=observed_round,round=observed_native_round,path=str(log_path.relative_to(root))))
                if any(r.get('type')=='adopted_config' and r.get('peer')==int(match[1]) and r.get('source_round')==observed_round for r in events[name]):
                    own_hold_notifications.append(dict(tick=int(match[2]),source_round=observed_round,round=observed_native_round,incarnation=int(log_path.parent.parent.name.split('-')[-1]),path=str(log_path.relative_to(root)),line=number))
            if match := re.search(r'\[net-lockstep\] hold of this seat at (\d+)',line):
                own_hold_notifications.append(dict(tick=int(match[1]),source_round=observed_round,round=observed_native_round,incarnation=int(log_path.parent.parent.name.split('-')[-1]),path=str(log_path.relative_to(root)),line=number))
            if re.search(r'RTE Assert|FATAL:|EXCEPTION_ACCESS_VIOLATION|Runtime Error due to unhandled exception|Rejected .*command|\[cross-record\] FAIL|\[net-ui-probe\] FAIL|\[net-match-service-e2e\].*(?:FAIL|setup failed)|\[net-match\] controller sync failed:|\[net-plane\].*ASSERT|\[fullstate(?:-refusal)?\].*(?:failed:|refused:|problem=)|Desync:|desync at|admission refused|\[Lua error\]|Segmentation fault', line, re.I):
                findings.append(dict(peer=name, path=str(log_path.relative_to(root)), line=number, text=line.strip()))
        peer_waits[name] = waits
        raw_path = own / 'engine/feel/raw.jsonl'; raw = list(rows(raw_path))
        presentation_window=presentation_index(raw_path.with_name('raw.index.json'))
        presentation_by_incarnation={}
        presentation_valid=True
        for fragment in fragments:
            document=presentation_index(fragment/'engine/feel/raw.index.json')
            normal_exit=load(fragment/'record.json',{}).get('exit_code')==0
            valid=presentation_contract(manifest,document,normal_exit)
            if fragment!=own:
                for row in rows(fragment/'engine/feel/raw.jsonl'):
                    if row.get('type')=='malformed_record':
                        valid=False
                        findings.append(dict(peer=name,path=str((fragment/'engine/feel/raw.index.json').relative_to(root)),line=row['_line'],text=row['error']))
            presentation_by_incarnation[fragment.name]=dict(index=document,declared_bound_valid=valid)
            presentation_valid &= valid
        for row in raw:
            if row.get('type')=='malformed_record':
                presentation_valid=False
                findings.append(dict(peer=name,path=str(raw_path.relative_to(root)),line=row['_line'],text=row['error']))
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
                     stalls == 0,
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
        memory_census={fragment.name.split('-')[-1]: report.reduce_memory_census(''.join(line for _, line in read_log(fragment/'engine/stdout.log')),
                        **manifest['memory'], elapsed_s=load(fragment/'record.json',{}).get('elapsed_seconds', 0))
                       for fragment in fragments}
        from feel.harness_cost import reduce_costs
        harness_cost = {fragment.name: reduce_costs([fragment/'engine/stdout.log']) for fragment in fragments}
        archives=[r for fragment in fragments for r in source_rows(fragment/'archives.jsonl',root)]
        payload_sizes = [r['trace_vector_payload_bytes'] for r in events[name]
                         if r.get('type') == 'tick_timing' and 'trace_vector_payload_bytes' in r]
        instrumentation = dict(first_bytes=payload_sizes[0] if payload_sizes else None,
            last_bytes=payload_sizes[-1] if payload_sizes else None, peak_bytes=max(payload_sizes) if payload_sizes else None,
            growth_bytes=payload_sizes[-1]-payload_sizes[0] if payload_sizes else None,
            scope='Measured raw tick-hash vector and subsystem-vector capacities only; allocator and other buffers excluded. No subtraction from resident/private totals.')
        trace=load(own/'trace.json',{})
        completion=trace.get('runs',[{}])[-1].get('strings',{}) if trace.get('runs') else {}
        final_tick=trace.get('runs',[{}])[-1].get('numeric',{}).get('final_tick') if trace.get('runs') else None
        peers[name] = dict(box=spec['box'], role=spec['role'], instance=name, incarnation=int(own.name.split('-')[-1]), frames=len(live[name]),
            observed_waits_over_50=sum(r['wait_ms']>50 for r in waits) if log else None, observed_wait_records=len(waits),
            native=native, record=record, timing=timing, presentation_window=presentation_window, pace=pace_verdict(native),
            retired_diagnostics=[r for fragment in fragments for r in source_rows(fragment/'retired-diagnostics.jsonl',root)],
            presentation_by_incarnation=presentation_by_incarnation,presentation_valid=presentation_valid,
            tick_compute_ms=report.distribution([r['compute_us']/1000 for r in tick_cost]),
            tick_timing_valid=bool(tick_cost) and all(r.get('partition_valid') for r in tick_cost),
            capture_ms=report.distribution([r['capture_us']/1000 for r in tick_cost]),
            latency_ms=report.distribution([r['ms'] for r in latency if r['ms'] is not None]),
            latency_lower_bounds_ms=report.distribution([r['latency_lower_bound_ms'] for r in latency]),
            latency_unreflected=sum(r['ms'] is None for r in latency),
            draw_ms=report.distribution([r['draw_ms'] for r in frames]), present_ms=report.distribution([r['present_ms'] for r in frames]),
            frame_interval_ms=report.distribution([r['interval_ms'] for r in frames if r.get('interval_ms') is not None]),
            frame_count=len(frames), frames_over_50_ms=[r['frame'] for r in frames if max(r['draw_ms'], r['present_ms'], r.get('interval_ms') or 0) > 50],
            effective_hz=(len(frames)-1)*1000/(frames[-1]['present_end_ms']-frames[0]['present_end_ms']) if len(frames)>1 and frames[-1]['present_end_ms']>frames[0]['present_end_ms'] else None,
            memory=memory, memory_by_incarnation=memory_by_incarnation, memory_census=memory_census, instrumentation=instrumentation,
            harness_cost=harness_cost, instrument_valid=all(row['passed'] for row in harness_cost.values()), archives=archives,
            recovery_observations=recovery_observations,payload_clock_last_ms=max([payload_done.get('payload_monotonic_ms',0),*[r.get('payload_monotonic_ms',0) for r in samples],*[r.get('upper_wall_ms',0) for r in recovery_observations]]),
            native_completion=completion, native_final_tick=final_tick, exits=exits,own_hold_notifications=own_hold_notifications,
            hold_evidence_complete=all((f/'engine/stdout.log').is_file() and (f/'engine/stdout.log').stat().st_size>0 for f in fragments),
            fragments=[str(fragment.relative_to(root)) for fragment in fragments], samples=samples, holds=holds, feel_status='PASS' if quiet and all(feel_pins) else 'FAIL' if quiet else 'UNDER LOAD' if under_load else 'REPORTED; quiet window not scheduled',
            feel_gated=quiet, feel_pass=all(feel_pins), wire_egress=None,
            wire_reason='Transport wire counters are not exposed at an owned seam; GnsTransport.cpp:904 detailed-status text is not a per-tick counter API.',
            configs=configs, paths={kind: str(record_path(own / leaf).relative_to(root)) for kind,leaf in [('live','live.jsonl'),('events','events.jsonl'),('log','engine/stdout.log'),('feel','engine/feel/raw.jsonl'),('native','match-report.json')]})
    return dict(manifest=manifest, peers=peers, events=events, live=live, paths=paths, findings=findings, capabilities=capabilities, mixed_builds=mixed_builds)


def collect(root):
    return apply_report_policy(collect_native(root))
