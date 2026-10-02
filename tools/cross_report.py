"""Build a self-contained, evidence-linked multi-box report and its run index."""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import html
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess

from feel import report
from feel.records import open_record, record_path, presentation_records
from compare_sim_traces import CORE

HERE = Path(__file__).resolve().parent
REQUIRED_SUBSYSTEMS = CORE | {'controller'}
CORE_CHECKS=('three_real_boxes','preflight_complete','full_history','zero_desync','zero_unscheduled_holds',
             'hold_evidence_complete','native_completion','adopted_peer_count','adopted_roster','native_desync_checks','record_integrity','binary_admission_limit')
CAPTURE_ROWS={
    1:'capture-rows lane row 1: unsupported userdata in object[...][AI][Behavior].slot[12] at Source/Managers/LuaMan.cpp:1613',
    2:'capture-rows lane row 2: Windows/Mac hex-float text at Source/System/FloatText.h:324-326,608-610; PieMenu.cpp:259; Arm.cpp:506; Scene.cpp:2056,2059,2142'}



def unapplied_faults(faults, receipts):
    """The scheduled faults no receipt shows applied, by id: a fault whose target incarnation never reached its tick is one."""
    return [f['id'] for f in faults if not any(r.get('id') == f['id'] and r.get('applied') for r in receipts)]

def judge_attempt(manifest,checks,peers,matrix,recoveries,mixed_builds=()):
    core=all(checks.get(name,False) for name in CORE_CHECKS)
    engine_red=not checks.get('zero_unscheduled_holds',False) and bool(checks.get('only_capture_induced_holds')) and all(checks.get(name,False) for name in CORE_CHECKS if name not in ('full_history','zero_unscheduled_holds'))
    oracle=lambda passed,reason='':dict(status='PASS' if passed else 'FAIL',reason=reason)
    pending=manifest.get('capture_rows_pending',[1,2])
    memory=[value for peer in peers.values() for value in peer.get('memory_by_incarnation',{}).values()]
    memory_status='PASS' if memory and all(m['passed'] for m in memory) else \
        'FAIL' if any(m.get('sizes') or m.get('missing_samples') for m in memory) else 'NOT COVERED'
    memory_reason='Declared per-incarnation warm-up, slope and retention; raw sizes and measured instrumentation remain separate.'
    census=[c for peer in peers.values() for c in peer.get('memory_census',{}).values() if c.get('status')!='NOT COVERED']
    if census:
        memory_status='FAIL' if any(c['status']=='FAIL' for c in census) else 'PASS'
        memory_reason=('Each process each minute, net of the full-state instrument cache: warm-up is a falling slope reaching under '
                       f"{census[0]['warm_slope_bound']} MB/min and staying under it; the declared-bounds read stays reported beside it.")
    coverage_status='FAIL' if any(row['status']=='FAIL' for row in matrix) else \
        'NOT COVERED' if any(row['status']=='NOT COVERED' for row in matrix) else 'PASS'
    oracles=dict(
        preflight=dict(oracle(checks.get('preflight_complete',False),'; '.join(mixed_builds) or
            'Every box preflighted without a driver finding, and every incarnation ran the executable its preflight hashed.'),reasons=list(mixed_builds)),
        live_hashes=oracle(checks.get('full_history',False) and checks.get('zero_desync',False),'Every declared comparable key; UNKNOWN never equals.'),
        unscheduled_holds=oracle(checks.get('zero_unscheduled_holds',False) and checks.get('hold_evidence_complete',False),
            'Every incarnation must retain its stdout hold log; missing evidence cannot establish zero holds.'),
        native_completion=oracle(checks.get('native_completion',False)),
        full_state=oracle(checks.get('shared_fullstate',False) and not pending,
            'NOT COVERED by '+ '; '.join(CAPTURE_ROWS[row] for row in pending) if pending else 'All promised Shared/canonical/restored observations must compare.'),
        recovery=oracle(checks.get('bounded_recovery',False),'Every recovery id must reach its declared terminal outcome within its declared deadline.'),
        fault_effects=oracle(checks.get('faults_applied',False) and checks.get('native_fault_effects',False),'Arming H4 is separate from observing its native ack/commit effect.'),
        coverage=dict(status=coverage_status,reason='Each matrix row retains its own minimum, counts and reason.'),
        feel=dict(status=('PASS' if checks.get('quiet_feel',False) else 'FAIL') if any(p.get('feel_gated') for p in peers.values()) else
                         'UNDER LOAD' if any(p.get('feel_status')=='UNDER LOAD' for p in peers.values()) else 'REPORTED',reason='Gated only in a declared quiet window without measured load.'),
        memory=dict(status=memory_status,reason=memory_reason),
        record_integrity=oracle(checks.get('record_integrity',False)),
        engine_findings=oracle(checks.get('no_engine_findings',False),'All findings remain visible, including the named capture rows.'),
        exits=oracle(checks.get('all_incarnation_exits',False),'Each incarnation must exit normally or have its own scheduled, actually injected crash receipt.'),
        pace=oracle(checks.get('box_pace',False),'Every box whose own sim fits the tick holds >= 59.5 ticks/s over the match: a slow presenter sheds frames, never ticks.'))
    if mixed_builds:
        for name in ('live_hashes','full_state'): oracles[name]=dict(status='VOID',reason='Compared across a mixed build; the preflight names both executables.')
    if not manifest.get('faults'): oracles['recovery']['status']='NOT APPLICABLE'
    if not manifest.get('faults'): oracles['fault_effects']['status']='NOT APPLICABLE'
    if manifest['scenario']!='match':
        # Each item is judged for what the schedule asks of it; one the schedule never asks for is not applicable, with its reason.
        phases={f.get('phase','hold') for f in manifest.get('faults',[]) if f.get('action')=='brain-eliminate'}
        forced={'hold':checks.get('forced_end_during_hold',False),'catch_up':checks.get('forced_end_during_transfer',False)}
        oracles['forced_ends']=oracle(all(forced.get(phase,False) for phase in phases),'Actual activity-over must overlap the named recovery phase; a stale hint is insufficient.') if phases else             dict(status='NOT APPLICABLE',reason='The schedule forces no end.')
        oracles['rematches']=oracle(checks.get('changed_settings_rematch',False) and checks.get('fog_on_match',False)) if checks.get('round_ended',True) else             dict(status='NOT APPLICABLE',reason='No round ended inside the budget, so no rematch carried changed settings.')
        # A restarted peer returns through its ticket and the host's image; only a restart from its own archive exercises an autosave restore.
        oracles['autosaves']=oracle(checks.get('validated_autosave_archives',False),'Archive integrity alone does not prove restoration or sealed admission.')             if any(f.get('action')=='crash-restart' and f.get('restore')=='archive' for f in manifest.get('faults',[])) else             dict(status='NOT APPLICABLE',reason='The schedule restarts no peer from its own archive (a restarted peer returns through its ticket and the host image), so no autosave restoration or sealed admission is exercised; the restore arms judge it.')
    required = (*CORE_CHECKS, 'shared_fullstate', 'all_incarnation_exits', 'no_engine_findings', 'box_pace')
    if manifest['scenario'] != 'match':
        required += ('bounded_recovery', 'faults_applied', 'native_fault_effects')
    workload = (manifest['ticks'] == 1201 and not manifest.get('faults')) if manifest['scenario'] == 'match' else (
        manifest['ticks'] == 72000 if manifest['scenario'] == 'soak' else
        bool(manifest.get('faults')) if manifest['scenario'] == 'chaos' else False)
    v1 = all(checks.get(name, False) for name in required) and workload and bool(manifest.get('fullstate_every')) and not pending and not mixed_builds
    return dict(core_passed=core,core_engine_red=engine_red,mixed_builds=list(mixed_builds),
        v1_passed=bool(v1), v1_checks=list(required), v1_workload=workload,
        gate_b_eligible=(core or engine_red) and checks.get('shared_fullstate',False) and not pending and manifest['scenario']=='match' and manifest['ticks']==1201 and bool(manifest.get('fullstate_every')) and not manifest.get('faults'),oracles=oracles)


def pace_verdict(native, tick_ms=1000/60):
    """A box whose own simulation fits the tick holds the round's rate: it sheds presentation, never ticks. A box whose sim alone
    cannot is a slow machine, held by the bound; its rate is reported, not gated."""
    pace = (native or {}).get('pace') or {}
    sim, tps = pace.get('sim_ms_per_tick'), pace.get('wall_tps')
    gated = sim is not None and sim < tick_ms
    return dict(sim_ms_per_tick=sim, wall_tps=tps, gated=gated, passed=not gated or (tps is not None and tps >= 59.5))


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


def crash_hold(hold,crash_ids):
    """A hold inside a crashed seat's own recovery is the scheduled crash's: a crashed process leaves no input timing to associate."""
    if hold.get('scheduled_recovery_id') in crash_ids and hold.get('classification')=='other':
        return dict(classification='scheduled-fault',reason='The held seat is the one the schedule crashed, inside its recovery from the crash to its first controllable input.')
    return {}


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


def host_clock(host_rows):
    """The host's own wall clock at each (round, tick) it simulated: one clock for every box's events."""
    clock = {}
    for r in host_rows:
        if r.get('phase') == 'live' and isinstance(r.get('tick'), int) and isinstance(r.get('wall_ms'), (int, float)):
            clock.setdefault((str(r.get('round')), r['tick']), r['wall_ms'])
    return clock


def fault_windows(faults, receipts, clock):
    """Each applied fault with a duration, as a span of the host's clock from the frame it was applied at."""
    windows = []
    for receipt in receipts:
        fault = next((f for f in faults if f.get('id') == receipt.get('id')), None)
        if not fault or not receipt.get('applied') or not fault.get('duration_ms'): continue
        start = clock.get((str(receipt.get('round')), receipt.get('applied_frame')))
        if start is None: continue
        windows.append(dict(id=fault['id'], peer=receipt.get('peer'), start_ms=start, end_ms=start + fault['duration_ms']))
    return windows


def fault_window_hold(hold, windows, clock):
    """A hold of the faulted seat inside its scheduled fault's window is that fault's."""
    at = clock.get((str(hold.get('round')), hold.get('tick')))
    if at is None: return None
    return next((w['id'] for w in windows if w['peer'] == hold.get('peer') and w['start_ms'] <= at <= w['end_ms']), None)


DRIVER_STOPS = ('stopped without deletion',)


def driver_stop(findings):
    """The reason the driver itself stopped the run (its scratch guard), if it did: such a run is reported STOPPED, never judged."""
    return next((f['reason'] for f in findings if f.get('kind') == 'driver' and any(stop in f.get('reason', '') for stop in DRIVER_STOPS)), None)


def attempt_label(result):
    if result.get('stopped'): return f"STOPPED ({result['stopped']}); NOT JUDGED"
    if result.get('v1_passed'): return 'V1 PASS'
    if 'v1_passed' not in result: return 'V1 NOT GRADED'
    if result.get('mixed_builds'): return 'PREFLIGHT RED (mixed build); FULL GATE VOID'
    if result.get('core_engine_red'): return 'CORE-ENGINE-RED; FULL GATE RED'
    return 'CORE PASS; FULL GATE RED' if result.get('core_passed') else 'CORE FAIL; FULL GATE RED'


def verdict_line(result):
    peers=result['peers']; comparison=result['comparison']
    holds=sum(len(p['holds']) for p in peers.values())
    classifications=dict(Counter(h.get('classification','other') for p in peers.values() for h in p['holds']))
    unkeyed=lambda p: f' ({p["unkeyed"]} unkeyed: {",".join(p["first_unkeyed"]["missing"])} from tick {p["first_unkeyed"]["tick"]})' if p.get('unkeyed') else ''
    text=f'{attempt_label(result)} {result["run"]}: '+', '.join(f'{n}={p["frames"]} frames{unkeyed(p)}' for n,p in peers.items())
    text+=f'; unequal={comparison["unequal_keys"]} unknown={comparison["unknown_keys"]} holds={holds} unscheduled={result["unscheduled_holds"]} classifications={classifications}; '
    text+='; '.join(name+'='+oracle['status'] for name,oracle in result['oracles'].items())
    if result.get('assigned_capture_rows'): text+='; full-state NOT COVERED by capture-rows lane rows '+','.join(result['assigned_capture_rows'])
    return text


def classify_hold(hold,events,peers):
    result=dict(classification='other',reason='Held peer timing or adopted hold bound is missing or ambiguous.',
                held_instance=None,incarnation=None,execution=None,capture_tick=None,capture_us=None,compute_us=None,wait_us=None,
                hold_bound_us=None,evidence_path=None,evidence_line=None,own_hold_notifications=[],timing_valid=False,association=None)
    matches=[]
    for name,records in events.items():
        configs=[r for r in records if r.get('type')=='adopted_config' and r.get('peer')==hold['peer'] and r.get('source_round')==hold.get('source_round') and
                 (hold.get('round') is None or r.get('round')==hold['round'])]
        if not configs: continue
        notices=[r for r in peers[name].get('own_hold_notifications',[]) if r['tick']==hold['tick'] and r['source_round']==hold.get('source_round') and
                 (hold.get('round') is None or r.get('round')==hold['round'])]
        incarnations={r['incarnation'] for r in notices}
        if len(incarnations)!=1: continue
        timing=[r for r in records if r.get('type')=='tick_timing' and r.get('phase')=='live' and r.get('peer')==hold['peer'] and
                r.get('source_round')==hold.get('source_round') and r.get('tick',float('inf'))<=hold['tick'] and
                (hold.get('round') is None or r.get('round')==hold['round']) and
                (not incarnations or r.get('incarnation') in incarnations)]
        if not timing: continue
        tick=max(r['tick'] for r in timing); timing=[r for r in timing if r['tick']==tick]
        if len(timing)!=1: continue
        row=timing[0]
        config=next((c for c in reversed(configs) if c.get('incarnation')==row.get('incarnation') and c.get('tick',0)<=tick),None)
        if not config: continue
        ticks=config.get('config',{}).get('rules',{}).get('slow_player_bound_ticks')
        ms=config.get('sim_tick_ms')
        if not isinstance(ticks,(int,float)) or not isinstance(ms,(int,float)): continue
        bound=max(1,math.floor(ticks*ms))*1000
        matches.append(dict(held_instance=name,incarnation=row.get('incarnation'),execution=row.get('execution'),
            capture_tick=tick,capture_us=row.get('capture_us'),compute_us=row.get('compute_us'),wait_us=row.get('wait_us'),hold_bound_us=bound,
            evidence_path=row.get('_path'),evidence_line=row.get('_line'),own_hold_notifications=notices,
            timing_valid=row.get('partition_valid') is True,
            association='Held peer last live committed timing at or before the hold boundary; later catch-up timing excluded.'))
    if len(matches)!=1: return result
    result.update(matches[0])
    if not result['timing_valid']:
        result['reason']='Held peer timing partition is invalid; no capture attribution is justified.'
        return result
    if isinstance(result['capture_us'],(int,float)) and result['capture_us']>result['hold_bound_us']:
        result.update(classification='capture-induced',reason='Inventory engine rows 374/395/403: capture work exceeds the adopted slow-player bound; Source/Main.cpp:7187 timing/capture seam and Source/Network/NetLockstep.cpp:5903.')
    else:
        result['reason']='Capture did not exceed the bound; compute/wait values shown. The record does not identify any further blocking cause.'
    return result


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
    cpu_teams=[] if roster=='four-way' else [1,2] if roster=='ai-heavy' else [2] if roster in ('mixed','allies') else [humans]
    mode='pvp-skirmish' if roster=='four-way' else 'coop-pve' if roster=='ai-heavy' else 'pvpve'
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


def presentation_contract(manifest,document,normal_exit):
    storage=manifest.get('storage',{})
    if 'presentation_chunk_bytes' not in storage: return True
    return bool(document and document.get('chunk_bytes')==storage['presentation_chunk_bytes'] and
        document.get('retained_chunks')==storage['presentation_retained_chunks'] and
        len(document.get('parts',[]))<=storage['presentation_retained_chunks'] and
        (not normal_exit or document.get('complete') is True))


def presentation_index(path):
    try: return load(path,{})
    except (OSError,ValueError) as error: return dict(index_error=str(error))


def write_rerun_command(root,manifest):
    root=Path(root)
    current=load(HERE/'cross_peers/boxes.json',{})
    routes={b['name']:b for b in current.get('boxes',[])}
    boxes=[dict(routes.get(b['name'],b) if routes.get(b['name'],{}).get('kind')==b['kind'] else b,
                peers_per_box=sum(p['box']==b['name'] for p in manifest['instances'])) for b in manifest['boxes']]
    inputs={'boxes':dict(version=1,boxes=boxes,instances=manifest['instances']),
            'schedule':manifest.get('faults',[]),'barriers':manifest.get('capture_barriers',[])}
    for name,document in inputs.items():
        (root/f'rerun-{name}.json').write_text(json.dumps(document,indent=2)+'\n',encoding='utf-8')
    args=['python','tools/cross_peers.py','--host',manifest['host'],'--scenario',manifest['scenario'],
          '--ticks',str(manifest['ticks']),'--roster',manifest['roster'],'--scene',manifest['scene'],
          '--seed',str(manifest['seed']),'--fullstate-every',str(manifest.get('fullstate_every',0)),
          '--timeout',str(manifest['deadlines']['launch_s']),'--recovery-deadline-ms',str(manifest['deadlines']['recovery_ms']),
          '--capture-budget-ms',str(manifest['deadlines']['capture_ms'])]
    for name in inputs: args += ['--'+name,str(root/f'rerun-{name}.json')]
    if manifest.get('chaos_seed') is not None: args += ['--chaos-seed',str(manifest['chaos_seed'])]
    if manifest.get('lane'): args += ['--lane',manifest['lane']]
    if manifest.get('mac_guard'): args += ['--mac-guard',manifest['mac_guard']]
    args += ['--out',root.as_posix()+'-rerun']
    return subprocess.list2cmdline(args)


def fullstate_expected(host_rows, cadence):
    """The periodic samples every peer owes: each live gameplay tick on the cadence and each round's first eligible tick."""
    expected, first_eligible = set(), {}
    for observed in host_rows:
        if observed.get('phase')!='live' or observed.get('paused') or not observed.get('gameplay_tick') or not observed.get('effective_start_frame'):
            continue
        if observed['tick'] < observed['effective_start_frame']: continue
        round_id=observed['round']; first_eligible.setdefault(round_id,observed['tick'])
        if observed['tick'] % cadence == 0: expected.add((round_id,observed['tick'],'sample'))
    expected.update((r,t,'sample') for r,t in first_eligible.items())
    return sorted(expected)


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
        32: 'The adopted wave supplies held-seat end records at NetMatchService.cpp:1026,5357,8654. Three-box faulted final-tail equality and completed-lobby outcomes remain unproved; an end-record notification alone is not a full hash history.',
        34: 'Hold and catch-up ends are wired into the default schedule with peer/round/incarnation-bound phase signals and native end witnesses. Both executed overlaps and the post-migration end are not yet demonstrated; migration awaits the endpoint seam.',
        35: 'Pre-auth/proof/image/tail/activation overlaps await the WAN endpoint fix; queued/cancelled phases never complete recovery.',
        36: 'Capture-announced and writer-pending barriers have RED/GREEN bounded-release tests; migration overlap, archive validity and restarted writer cadence still await a real endpoint-capable arm.',
        40: 'Initial history uses the configured start frame; settled live authority is read from the public runner report. NetMatchService.cpp:7831 omits the private catch-up branch/checkpoint digest (NetMatchService.h:1590,1643), so restored keys stay UNKNOWN.',
        45: 'Checkpoint boot/handover anchors and survivor segment indexing need ScenarioRunner.cpp:2653 and NetMatchService.cpp:3455 outside this lane.',
        47: 'Raw memory samples, measured instrumentation and declared bounds are judged per attempt. A complete comparable workload across every round, checkpoint and recovery is absent; any observed bound failure remains RED.',
        50: 'Movement/aim submitted-render measurements are reported; other action/input-sequence stamps require FrameMan.cpp:277,344 outside this lane.',
        51: 'Remote-unit render discontinuities require FrameMan/LocalPrediction records outside this lane; local corrections retain their own labels.',
        53: 'Fresh produced/applied controller input and authenticated goodbye now have native terminal records, bound to a continuous payload clock. Full phase-specific queued/cancelled admission and survivor end-to-end runs remain incomplete at NetMatchService.cpp:939,5319 and NetWorldJoin.cpp:1139.',
        54: 'FrameMan.cpp:277,344 lacks round/execution ids; multi-round raw presentation reduction cannot safely reuse single-round identities.',
        69: 'The real Void Wanderers mission/economy/scene-transition and unchanged-reference arm has not run.',
        71: 'Fog-on reveal/capture/rejoin is not yet demonstrated; a fog-off fight does not cover it.',
        'reread-2b': 'Success-positioned throw, ejection, door and first-controllable-input records exist. Full melee/shield/emitter/bleeding/settling/script assertions and every scheduled recovery outcome remain unproved (6,11,14,18,53).',
        'reread-2c': 'Exclusive tick timing and native fresh-input recovery terminals exist at Main.cpp:625. Complete phase-specific recovery identity, deadline and survivor evidence have not been demonstrated (52,53).',
        'reread-7': 'First soak excludes host kill and declares fault budgets. The adopted wave has held-seat end records; every queued/cancelled/refused outcome, full terminal hash history and 36000-tick chain remain unproved (32,35,62).'}
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


def local_host_render(manifest, peers):
    local = {box['name'] for box in manifest['boxes'] if box['kind'] == 'windows-local'}
    spec = next((spec for spec in manifest['specs'] if spec['peer'] == manifest['host'] and spec['box'] in local), None)
    if spec is None:
        return None
    settings = spec.get('settings', {})
    tps = peers.get(spec['peer'], {}).get('timing', {}).get('steady_wall_tps')
    return dict(peer=spec['peer'], box=spec['box'], size=f"{settings.get('ResolutionX', 960)}x{settings.get('ResolutionY', 540)}",
                render_cap=spec.get('render_cap', 60), wall_tps=tps,
                reason='Existing steady wall TPS from retained live events; reported only.' if tps is not None else
                       'No wall TPS available in the retained events; reported only.')


HOLD_OF_SEAT = re.compile(r'\[net-lockstep\] hold of this seat at (\d+)')
# A seat that lost its host with no survivor to take over rejoins it: it leaves its round after the last tick it applied.
LOST_HOST_REJOIN = re.compile(r'\[net-match\] recovery requested tick=(\d+) catch_up=\d+ reason=PeerHeld')
ROUND_START = re.compile(r'\[net-lockstep\] start round=(\d+) frame=')
RETURNED_LIVE = re.compile(r'\[net-match\] rejoin phase TailReplay -> Active')
ROUND_OVER = re.compile(r'\[net-match-service-e2e\] activity over at frame')
COMPLETED_HELD = re.compile(r'\[net-match\] completed_by_(?:end_record|next_round|host_goodbye)=1 held_from=\d+')


def held_away_ranges(live, ranges, logs):
    """A seat that left a round while held (its own completed_by_* line) never simulated that round past its last record:
    from the tick after it to the round's end is its away range, as an image rejoin's is. Any other missing key stays UNKNOWN."""
    away = {}
    for name, peer_rows in live.items():
        # Each departure with the round and frame its hold began: ticks the seat ran from there were on a branch it then abandoned.
        by_round, ordered = {}, []
        for text in logs.get(name, []):
            held, current = None, None
            for line in text.splitlines():
                if (found := ROUND_START.search(line)): current = found.group(1)
                if (found := HOLD_OF_SEAT.search(line)): held = (current, int(found.group(1)))
                elif (found := LOST_HOST_REJOIN.search(line)): held = (current, int(found.group(1)) + 1)
                elif COMPLETED_HELD.search(line) or (held is not None and ROUND_OVER.search(line)):
                    # A round that ended while the seat was still held or replaying its way back is one it left at its hold.
                    if held is not None and held[0] is not None: by_round[held[0]] = held[1]
                    else: ordered.append(held[1] if held else None)
                    held = None
                elif RETURNED_LIVE.search(line): held = None
        for interval in ranges:
            prefix = tuple(interval[field] for field in report.HISTORY_FIELDS[:-1])
            ticks = sorted({r['tick'] for r in peer_rows if isinstance(r.get('tick'), int) and tuple(r.get(field) for field in report.HISTORY_FIELDS[:-1]) == prefix})
            if not ticks or ticks[0] != interval['first'] or ticks[-1] >= interval['last'] or len(ticks) != ticks[-1] - ticks[0] + 1: continue
            if str(interval['match']) in by_round: held = by_round[str(interval['match'])]
            elif ordered: held = ordered.pop(0)
            else: continue
            first = held if held is not None and ticks[0] < held <= ticks[-1] + 1 else ticks[-1] + 1
            away[(name, prefix)] = (first, interval['last'])
    return away


PROPOSE_HOLD = re.compile(r'\[net-lockstep\] propose hold peer=(\d+) next_frame=(\d+)')
HEARD_THROUGH = re.compile(r' heard_through=(\d+)')
HOLD_CAUSE = re.compile(r' cause=(\w+)')
HOLD_LINE = re.compile(r'\[net-match\] hold peer=(\d+) frame=(\d+)')
# The A1 rules: a seat whose machine or stream cannot keep the round's pace is held, the design (ruling s).
DESIGN_CAUSES = ('capacity', 'late_stream', 'quiet')


def hold_causes(host_text):
    """The cause the host named for each hold it authored, by (round, seat, frame). A log from before the cause field names a late stream
    by its own arrival fields: the host had heard nothing from the seat past the frame before."""
    causes, pending, current = {}, {}, None
    for line in host_text.splitlines():
        if (found := ROUND_START.search(line)): current = found.group(1)
        elif (found := PROPOSE_HOLD.search(line)):
            heard, named = HEARD_THROUGH.search(line), HOLD_CAUSE.search(line)
            peer, frame, heard, named = int(found.group(1)), int(found.group(2)), heard and heard.group(1), named and named.group(1)
            pending[peer] = named or ('late_stream' if heard is not None and int(heard) < frame else None)
        elif (found := HOLD_LINE.search(line)) and int(found.group(1)) in pending:
            causes[(current, int(found.group(1)), int(found.group(2)))] = pending.pop(int(found.group(1)))
    return causes


def void_abandoned(rows):
    """The engine's own retraction: an {abandon_from: N, round: R} record voids the rows of round R from N its seat wrote before it."""
    kept = []
    for r in rows:
        if 'abandon_from' in r and r.get('session') is None:
            first, retracted = r['abandon_from'], str(r.get('round'))
            kept = [k for k in kept if not (str(k.get('round')) == retracted and isinstance(k.get('tick'), int) and k['tick'] >= first)]
            continue
        kept.append(r)
    return kept


RELAUNCHED_HELD = re.compile(r'\[net-match\] held client: replaying the private committed tail|\[net-match\] rejoin phase Loading -> TailReplay')


def adopt_restored_histories(live, ranges, logs):
    """A held seat that rejoined from an image starts its round again at the frame it resumes on (its configured start): from there
    its live records are that round's own and are compared, as an image rejoin's are; the frames between its last record and that
    frame are its away range. The image's catch-up replay before it carries no branch and is not compared here."""
    adopted, away = {}, {}
    for name, peer_rows in live.items():
        relaunched = any(RELAUNCHED_HELD.search(text) for text in logs.get(name, []))
        rows = peer_rows
        if relaunched:
            for interval in ranges:
                prefix = tuple(interval[field] for field in report.HISTORY_FIELDS[:-1])
                session, match, branch, source = prefix
                keyed = sorted(r['tick'] for r in rows if isinstance(r.get('tick'), int) and tuple(r.get(field) for field in report.HISTORY_FIELDS[:-1]) == prefix)
                restored = [r for r in rows if r.get('history_branch') is None and r.get('phase') == 'live' and isinstance(r.get('tick'), int) and
                            r.get('session') == session and r.get('match') == match and r.get('source_round') == source]
                replayed = any(r.get('phase') == 'catchup' and r.get('session') == session and r.get('match') == match for r in rows)
                # A seat held from its round's start that only replayed the round never played it: the whole round is its away range.
                if not keyed and not restored and replayed:
                    away[(name, prefix)] = [(interval['first'], interval['last'])]
                    continue
                if not keyed or not restored: continue
                # Each return starts a contiguous history at its own resume frame; the frames between them are away.
                segments = []
                for r in sorted(restored, key=lambda r: r['tick']):
                    if segments and r['tick'] == segments[-1][-1]['tick'] + 1 and r.get('configured_start_frame') == segments[-1][0]['tick']: segments[-1].append(r)
                    else: segments.append([r])
                if any(s[0].get('configured_start_frame') != s[0]['tick'] for s in segments) or segments[0][0]['tick'] <= keyed[-1]: continue
                chosen = {id(r) for s in segments for r in s}
                rows = [dict(r, history_branch=branch) if id(r) in chosen else r for r in rows]
                gaps, last = [], keyed[-1]
                for s in segments:
                    if s[0]['tick'] > last + 1: gaps.append((last + 1, s[0]['tick'] - 1))
                    last = s[-1]['tick']
                if gaps: away[(name, prefix)] = gaps
        # A catch-up replay runs before its seat lands, in the seat's away range (OR1), whether the seat relaunched or caught up in place.
        rows = [r for r in rows if not (r.get('history_branch') is None and r.get('phase') == 'catchup')]
        adopted[name] = rows
    return adopted, away


def build_report(root):
    root = Path(root).resolve(); manifest = load(root / 'manifest.json', {})
    live, events, peers, findings, paths = {}, {}, {}, list(manifest.get('driver_findings', [])), {}
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
            if runner and (not expected or runner.get('exe_sha256')!=expected):
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
                waits.append(dict(tick=int(match[1]), wait_ms=int(match[2]), line=number))
            if match := re.search(r'\[net-match\] hold peer=(\d+) frame=(\d+)', line):
                holds.append(dict(peer=int(match[1]), tick=int(match[2]), line=number,source_round=observed_round,round=observed_native_round,path=str(log_path.relative_to(root))))
                if any(r.get('type')=='adopted_config' and r.get('peer')==int(match[1]) and r.get('source_round')==observed_round for r in events[name]):
                    own_hold_notifications.append(dict(tick=int(match[2]),source_round=observed_round,round=observed_native_round,incarnation=int(log_path.parent.parent.name.split('-')[-1]),path=str(log_path.relative_to(root)),line=number))
            if match := re.search(r'\[net-lockstep\] hold of this seat at (\d+)',line):
                own_hold_notifications.append(dict(tick=int(match[1]),source_round=observed_round,round=observed_native_round,incarnation=int(log_path.parent.parent.name.split('-')[-1]),path=str(log_path.relative_to(root)),line=number))
            if re.search(r'RTE Assert|FATAL:|EXCEPTION_ACCESS_VIOLATION|Runtime Error due to unhandled exception|Rejected .*command|\[cross-record\] FAIL|\[net-ui-probe\] FAIL|\[net-match-service-e2e\].*(?:FAIL|setup failed)|\[net-match\] controller sync failed:|\[net-plane\].*ASSERT|\[fullstate(?:-refusal)?\].*(?:failed:|refused:|problem=)|Desync:|desync at|admission refused|\[Lua error\]|Segmentation fault', line, re.I):
                findings.append(dict(peer=name, path=str(log_path.relative_to(root)), line=number, text=line.strip()))
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
        memory_census={fragment.name.split('-')[-1]: report.reduce_memory_census(''.join(line for _, line in read_log(fragment/'engine/stdout.log')))
                       for fragment in fragments}
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
            memory=memory, memory_by_incarnation=memory_by_incarnation, memory_census=memory_census, instrumentation=instrumentation, archives=archives,
            recovery_observations=recovery_observations,payload_clock_last_ms=max([payload_done.get('payload_monotonic_ms',0),*[r.get('payload_monotonic_ms',0) for r in samples],*[r.get('upper_wall_ms',0) for r in recovery_observations]]),
            native_completion=completion, native_final_tick=final_tick, exits=exits,own_hold_notifications=own_hold_notifications,
            hold_evidence_complete=all((f/'engine/stdout.log').is_file() and (f/'engine/stdout.log').stat().st_size>0 for f in fragments),
            fragments=[str(fragment.relative_to(root)) for fragment in fragments], samples=samples, holds=holds, feel_status='PASS' if quiet and all(feel_pins) else 'FAIL' if quiet else 'UNDER LOAD' if under_load else 'REPORTED; quiet window not scheduled',
            feel_gated=quiet, feel_pass=all(feel_pins), wire_egress=None,
            wire_reason='Transport wire counters are not exposed at an owned seam; GnsTransport.cpp:904 detailed-status text is not a per-tick counter API.',
            configs=configs, paths={kind: str(record_path(own / leaf).relative_to(root)) for kind,leaf in [('live','live.jsonl'),('events','events.jsonl'),('log','engine/stdout.log'),('feel','engine/feel/raw.jsonl'),('native','match-report.json')]})
    host_rows = live.get(manifest['host'], [])
    ranges, missing_boundaries = report.declared_history_ranges(host_rows,
        [r for r in events.get(manifest['host'],[]) if r.get('type')=='match_boundary'], peers,
        smoke_ticks=manifest['ticks'] if manifest['scenario']=='match' else None,
        final_tick=peers[manifest['host']]['native_final_tick'])
    logs = {name: [''.join(line for _, line in read_log(root / fragment / 'engine/stdout.log')) for fragment in peer['fragments']] for name, peer in peers.items()}
    compared, restored_away = adopt_restored_histories({name: void_abandoned(rows) for name, rows in live.items()}, ranges, logs)
    away = {**held_away_ranges(compared, ranges, logs), **restored_away}
    comparison = report.compare_histories(compared, ranges, REQUIRED_SUBSYSTEMS, away)
    comparison['away_ranges'] = [dict(peer=peer, prefix=list(prefix), first=first, last=last) for (peer, prefix), spans in away.items()
                                 for first, last in (spans if isinstance(spans, list) else [spans])]
    fullstate_documents={name:report.parse_fullstate([root/fragment/'engine/stdout.log' for fragment in peer['fragments']]) for name,peer in peers.items()}
    cadence=manifest.get('fullstate_every',0)
    fullstate=report.compare_fullstate_histories(fullstate_documents,fullstate_expected(host_rows,cadence)) if cadence else dict(passed=False,status='NOT COVERED',reason='full-state instrumentation disabled')
    matrix = coverage(events, peers, manifest)
    fault_receipts = [dict(r, source_peer=name) for name,values in events.items() for r in values if r.get('type') == 'fault']
    fault_receipts += [dict(r['native'],source_peer=name,id=r['id'],type='fault',applied=True,source='owning payload termination')
        for name,p in peers.items() for r in p['recovery_observations'] if r.get('phase')=='fault_applied' and r.get('native',{}).get('action')=='crash-restart']
    unapplied = unapplied_faults(manifest['faults'], fault_receipts)
    faults_applied = not unapplied
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
    clock = host_clock(host_rows)
    causes = hold_causes(''.join(line for fragment in peers[manifest['host']]['fragments'] for _, line in read_log(root / fragment / 'engine/stdout.log')))
    windows = fault_windows(manifest['faults'], fault_receipts, clock)
    crash_ids = {f['id'] for f in manifest['faults'] if f['action']=='crash-restart'}
    for p in peers.values():
        for hold in p['holds']:
            hold['scheduled_recovery_id']=scheduled_hold(hold,fault_receipts,recoveries)
            hold.update(classify_hold(hold,events,peers))
            hold.update(crash_hold(hold,crash_ids))
            if not hold['scheduled_recovery_id'] and (window := fault_window_hold(hold, windows, clock)):
                hold.update(scheduled_recovery_id=window, classification='scheduled-fault',
                            reason='The held seat is the faulted one and the hold falls inside its scheduled fault window, on the host clock.')
            if not hold['scheduled_recovery_id'] and (cause := causes.get((str(hold.get('round')), hold['peer'], hold['tick']))) in DESIGN_CAUSES:
                hold.update(design_cause=cause, classification='capacity (design)',
                            reason='The host held the seat under its A1 rule (' + cause + '): the seat could not keep the round pace.')
    unscheduled_holds=sum(not h['scheduled_recovery_id'] and not h.get('design_cause') for p in peers.values() for h in p['holds'])
    checks = dict(three_real_boxes=len({manifest.get('preflights',{}).get(p['box'],{}).get('machine_id',p['box']) for p in peers.values() if p['record'].get('started')}) >= 3,
                  preflight_complete=len(manifest.get('preflights', {})) == len(manifest['boxes']) and not manifest.get('driver_findings') and not mixed_builds,
                  full_history=comparison['passed'] and not missing_boundaries, zero_desync=comparison['unequal_keys'] == 0 and bool(ranges),
                  hold_evidence_complete=all(p['hold_evidence_complete'] for p in peers.values()),
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
                      not any(r.get('type') in ('record_loss', 'record_rotation') or (r.get('type') == 'tick_timing' and not r.get('partition_valid')) for r in values)
                      for values in events.values()),
                  no_engine_findings=not findings)
    checks['all_incarnation_exits']=all(p['exits'] and all(e['passed'] for e in p['exits']) for p in peers.values())
    checks['box_pace']=bool(peers) and all(p['pace']['passed'] for p in peers.values())
    checks['record_integrity'] &= all(p['presentation_valid'] for p in peers.values())
    checks['only_capture_induced_holds']=all(h['classification']=='capture-induced' for p in peers.values() for h in p['holds'] if not h['scheduled_recovery_id'])
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
        checks['round_ended'] = any(r.get('type')=='match_boundary' for values in events.values() for r in values)
        checks['validated_autosave_archives'] = False
        checks['memory_bounds']=all(p['memory_by_incarnation'] and all(m['passed'] for m in p['memory_by_incarnation'].values()) for p in peers.values())
        # Ruling (w): with the engine's census the bar is each process's slope net of the instrument, never its total.
        census=[c for p in peers.values() for c in p.get('memory_census',{}).values() if c['status']!='NOT COVERED']
        if census: checks['memory_bounds']=all(c['status']=='PASS' for c in census)
    for name,peer in peers.items():
        peer['observations']=len(live[name])
        peer['frames']=comparison['peers'][name]['present']
        # Rows the writer left without a history key are retained and counted, never compared.
        unkeyed=[r for r in live[name] if any(r.get(field) is None for field in report.HISTORY_FIELDS)]
        peer['unkeyed']=len(unkeyed)
        peer['first_unkeyed']=dict(tick=unkeyed[0].get('tick'),path=unkeyed[0].get('_path'),line=unkeyed[0].get('_line'),
            missing=[field for field in report.HISTORY_FIELDS if unkeyed[0].get(field) is None]) if unkeyed else None
    judgment=judge_attempt(manifest,checks,peers,matrix,recoveries,mixed_builds)
    rerun=write_rerun_command(root,manifest)
    result = dict(version=2, run=manifest['run'], passed=judgment['v1_passed'], diagnostic_passed=all(checks.values()), checks=checks, manifest=manifest,**judgment,
                  assigned_capture_rows={str(row):CAPTURE_ROWS[row] for row in manifest.get('capture_rows_pending',[1,2])},rerun_after_capture_fix=rerun,
                  faults_unapplied=unapplied,
                  peers=peers, local_host_render=local_host_render(manifest, peers),
                  comparison=comparison, declared_ranges=ranges, missing_boundaries=missing_boundaries,
                  fullstate=fullstate, fullstate_records=fullstate_documents,
                  coverage=matrix, recoveries=recoveries, fault_receipts=fault_receipts, capabilities=capabilities, barrier_receipts=barrier_receipts,
                  native_recovery_records=native_recovery_records,native_fault_effects=effects,unscheduled_holds=unscheduled_holds,
                  findings=findings, triage=load(root/'triage.json',[]), requirements=requirements(manifest, comparison, peers),
                  stopped=driver_stop(findings))
    (root / 'result.json').write_text(json.dumps(result, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    write_page(root, result, events)
    write_index(root.parent)
    verdict=verdict_line(result)
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
        f'<p>Recovery deadline {manifest["deadlines"]["recovery_ms"]:,} ms · capture budget {manifest["deadlines"]["capture_ms"]} ms. Memory warm-up {manifest["memory"]["warmup_s"]} s, slope bound {manifest["memory"]["slope_bytes_per_minute"]:,} B/min, retention bound {manifest["memory"]["retained_bytes"]:,} B. Raw sizes are never reduced by unmeasured instrumentation.</p>']
    if metric := result.get('local_host_render'):
        cap = 'uncapped (0 Hz)' if metric['render_cap'] == 0 else f'{metric["render_cap"]} Hz'
        parts.append(f'<p>Local host {escape(metric["peer"])} render size {escape(metric["size"])} · cap {escape(cap)} · '
                     f'wall TPS {escape(value(metric["wall_tps"]))}. {escape(metric["reason"])}</p>')
    parts.append('<div class="cards">')
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
    parts.append('<h2>Every hold, classified</h2><p>CORE-ENGINE-RED means every unscheduled hold is capture-induced, with zero live mismatches and native completion. Missing live frames and full-state failures remain red in their own oracles. Repeated log observations of the same boundary are all retained. Unknown causes are other.</p><pre>'+escape(json.dumps({name:peer['holds'] for name,peer in result['peers'].items()},indent=2))+'</pre>')
    parts.append('<h2>Presentation retention</h2><p>'+escape(manifest.get('storage',{}).get('presentation_window','Legacy unbounded live presentation stream; compressed after exit.'))+'</p><p>Feel statistics describe the last incarnation’s retained window, not discarded rows. CRC, decoded byte counts and sequence continuity are checked for every retained chunk and incarnation. Native bounds must match the declared bounds; a normal exit must close its index.</p><pre>'+escape(json.dumps({name:peer.get('presentation_by_incarnation',{}) for name,peer in result['peers'].items()},indent=2))+'</pre>')
    parts.append('<h2>Diagnostic dump window</h2><p>'+escape(manifest.get('storage',{}).get('diagnostic_window','No live diagnostic window was declared for this historical run. Any later archival retirement is separately receipted.'))+'</p><pre>'+escape(json.dumps({name:peer.get('retired_diagnostics',[]) for name,peer in result['peers'].items()},indent=2))+'</pre>')
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
    parts.append('<p>Scheduled faults never applied (their target incarnation was not in the match at their tick): ' +
                 escape(', '.join(result.get('faults_unapplied', [])) or 'none') + '</p>')
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
    parts.append('<p>Rerun command preserves the declared deadlines, roster, expanded fault schedule and barriers. Known box routes use the current manifest and its guards; original routes remain in this run’s manifest. Add --quiet-window only for a new window scheduled by the lead.</p><pre>'+escape(result['rerun_after_capture_fix'])+'</pre>')
    parts.append('<h2>Engine and driver findings</h2>')
    for finding in result['findings']:
        parts.append('<p class="fail">' + (f'<a href="{escape(finding["path"])}">{escape(finding["path"])}:{finding["line"]}</a> ' if 'path' in finding else '') + escape(finding.get('text',finding.get('reason',''))) + '</p>')
    if result.get('triage'):
        parts.append('<h2>Source triage</h2><p>Inspection notes link observed failures to source seams. These notes do not change any oracle or classify missing evidence as passing.</p><pre>'+escape(json.dumps(result['triage'],indent=2))+'</pre>')
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
    raise SystemExit(0 if result['v1_passed'] else 1)
