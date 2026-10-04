"""Run one multi-box match through each machine's private runner.

Every match needs at least three real boxes. The manifest separates machines
from engine instances; the running binary's admission limit remains authoritative.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import subprocess
import sys
import time
import copy
import platform
import secrets
import world_mod_cross as acceptance_cross

HERE = Path(__file__).resolve().parent
# The lane that owns this run's scratch on every box: --lane or CC_CROSS_PEERS_LANE, never a default. The manifest
# writes it as {lane}, so a merged-away lane's root is never where a run lands.
LANE_ENV = 'CC_CROSS_PEERS_LANE'
SCRATCH_ROOT = Path('D:/mx')
SCRATCH = None
LANE = None
# The Mac inventory's live marker a Mac launch requires: --mac-guard or CC_CROSS_PEERS_MAC_GUARD, never a default. The
# manifest writes it as {mac_guard}, so a merged-away inventory lane's marker never guards a run.
MAC_GUARD_ENV = 'CC_CROSS_PEERS_MAC_GUARD'
MAC_GUARD = None
LIMIT = 4_000_000_000
MST = dt.timezone(dt.timedelta(hours=-7))


def setting_pair(value):
    key, separator, setting = value.partition('=')
    if not separator or not re.fullmatch(r'[A-Za-z][A-Za-z0-9_]*', key) or any(c in setting for c in '\r\n'):
        raise argparse.ArgumentTypeError('settings require KEY=VALUE on one line')
    return key, setting


def render_cap_hz(value):
    if str(value) not in ('0', '60'):
        raise argparse.ArgumentTypeError('[feel] invalid render settings: expected RenderCapHz = 0 or 60')
    return int(value)


def write_json(path, value):
    path=Path(path)
    temporary=path.with_name(path.name+f'.incoming-{os.getpid()}-{time.monotonic_ns()}')
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    temporary.replace(path)


def command(argv, timeout=60, check=True):
    result = subprocess.run(list(map(str, argv)), capture_output=True, text=True, timeout=timeout,
                            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    if check and result.returncode:
        raise RuntimeError(f'{argv[0]} exit={result.returncode}: {result.stderr[-1000:]}')
    return result.stdout


def quote_ps(value):
    return "'" + str(value).replace("'", "''") + "'"


def set_lane(lane):
    global LANE, SCRATCH
    if not lane or not re.fullmatch(r'[A-Za-z0-9_-]+', lane):
        raise ValueError(f'a lane name ([A-Za-z0-9_-]+) is required: --lane or {LANE_ENV}')
    LANE, SCRATCH = lane, SCRATCH_ROOT / lane


def with_lane(value):
    """A manifest value with {lane} and {mac_guard} replaced by this run's lane and Mac guard."""
    if isinstance(value, str):
        if '{lane}' in value:
            if LANE is None: raise ValueError(f'the manifest names {{lane}} but no lane was given: --lane or {LANE_ENV}')
            value = value.replace('{lane}', LANE)
        if '{mac_guard}' in value:
            if MAC_GUARD is None: raise ValueError(f'the manifest names {{mac_guard}} but no guard was given: --mac-guard or {MAC_GUARD_ENV}')
            value = value.replace('{mac_guard}', MAC_GUARD)
        return value
    if isinstance(value, list): return [with_lane(item) for item in value]
    if isinstance(value, dict): return {key: with_lane(item) for key, item in value.items()}
    return value


def load_boxes(path, roster='three-way'):
    manifest = with_lane(json.loads(Path(path).read_text(encoding='utf-8-sig')))
    if roster is not None:
        manifest['instances'] = [peer for peer in manifest['instances'] if not peer.get('rosters') or roster in peer['rosters']]
        active = {peer['box'] for peer in manifest['instances']}
        manifest['boxes'] = [box for box in manifest['boxes'] if box['name'] in active]
    boxes, peers = manifest['boxes'], manifest['instances']
    by_name = {box['name']: box for box in boxes}
    if len({box['name'].casefold() for box in boxes}) != len(boxes) or len({p['name'].casefold() for p in peers}) != len(peers):
        raise ValueError('box names and instance names must each be unique, including Windows case aliases')
    if len({p['box'] for p in peers}) < 3:
        raise ValueError('every match requires three real boxes')
    for box in boxes:
        if box['kind'] not in ('windows-local', 'windows-task', 'posix-ssh'):
            raise ValueError(f'unknown box kind: {box["kind"]}')
        for key in ('tree', 'executable', 'runner', 'ports', 'sampler', 'scratch', 'python', 'directory_port'):
            if not box.get(key): raise ValueError(f'{box["name"]}: missing {key}')
        if box['kind'] != 'windows-local' and not box.get('ssh'):
            raise ValueError(f'{box["name"]}: remote route needs ssh alias')
        if box['kind'] == 'windows-task' and (49675 <= box['directory_port'] <= 49974 or 50000 <= box['directory_port'] <= 50259):
            raise ValueError('Windows task directory listener is inside the excluded TCP ranges')
        if not re.fullmatch(r'[A-Za-z0-9_-]+', box['name']): raise ValueError('unsafe box name')
    used = {}
    counts={box['name']:sum(peer['box']==box['name'] for peer in peers) for box in boxes}
    for box in boxes:
        declared=box.get('peers_per_box',counts[box['name']])
        if type(declared) is not int or declared<1 or declared!=counts[box['name']]:
            raise ValueError(f'{box["name"]}: peers_per_box must equal its explicit instance count ({counts[box["name"]]})')
        box['peers_per_box']=declared
    for peer in peers:
        if peer['box'] not in by_name or not re.fullmatch(r'[A-Za-z0-9_-]+', peer['name']):
            raise ValueError('invalid instance name or box')
        box = by_name[peer['box']]
        first, last = peer['port_block']
        if not box['ports'][0] <= first <= last <= box['ports'][1]: raise ValueError('instance ports leave box block')
        if box['directory_port'] in range(first, last + 1): raise ValueError('directory and engine ports overlap')
        for port in range(first, last + 1):
            key = (box['name'], port)
            if key in used: raise ValueError('instances on the same box share a port')
            used[key] = peer['name']
        if peer.get('seat', 'player') not in ('player', 'member', 'spectator', 'late-joiner'):
            raise ValueError('unknown seat request')
    return manifest


def schedule_for(options, peers, boxes):
    if options.schedule:
        faults = json.loads(options.schedule.read_text(encoding='utf-8'))
    elif options.scenario == 'match':
        faults = []
    else:
        host=next(p['name'] for p in peers if p['name']==options.host or p['box']==options.host)
        clients = [peer for peer in peers if peer['name'] != host]
        remote_windows = next((p['name'] for p in clients if boxes[p['box']]['kind'] == 'windows-task'), clients[0]['name'])
        posix = next((p['name'] for p in clients if boxes[p['box']]['kind'] == 'posix-ssh'), clients[-1]['name'])
        faults = [dict(id='edith-live-stall',tick=7200, peer=remote_windows, action='live-stall', duration_ms=600),
                  dict(id='mac-announced-rejoin',tick=14400, peer=posix, action='announced-leave-rejoin'),
                  dict(id='edith-crash-restart',tick=21600, peer=remote_windows, action='crash-restart'),
                  dict(id='mac-ack-drop',tick=28800, peer=posix, action='ack-drop'),
                  dict(id='mac-loss',tick=28800, peer=posix, action='loss', percent=5, duration_ticks=1800),
                  dict(id='end-under-hold',tick=7200,peer=host,action='brain-eliminate',phase='hold',target_peer=remote_windows,
                       target_incarnation=0,recovery_id='edith-live-stall',phase_window_ticks=600),
                  dict(id='end-under-catchup',tick=14400,peer=host,action='brain-eliminate',phase='catch_up',target_peer=posix,
                       target_incarnation=1,recovery_id='mac-announced-rejoin',phase_window_ticks=6000)]
    if options.scenario == 'chaos':
        import random
        rng = random.Random(options.chaos_seed)
        faults = [dict(tick=rng.randrange(600, options.ticks-600), peer=rng.choice(peers)['name'],
                       action='live-stall', duration_ms=rng.choice([200, 600, 1500])) for _ in range(options.chaos_faults)]
        faults.sort(key=lambda f: (f['tick'], f['peer']))
    names = {p['name'] for p in peers}
    incarnations = dict.fromkeys(names, 0)
    faults.sort(key=lambda f: (f['tick'], f['peer']))
    for number, fault in enumerate(faults, 1):
        if fault['peer'] not in names or fault['tick'] < 1: raise ValueError('invalid fault target or tick')
        if fault['action'] == 'host-kill':
            host = next(p['name'] for p in peers if p['name'] == options.host or p['box'] == options.host)
            if fault['peer'] != host or options.scenario != 'match':
                raise ValueError('host-kill requires the declared host in the HL4 match arm')
            preparation = [other for other in faults if other is not fault]
            silence = [other for other in preparation if other['action'] == 'silence']
            bans = [other for other in preparation if other['action'] == 'moderation-ban']
            if preparation and not (len(preparation) == 2 and len(silence) == len(bans) == 1
                    and silence[0]['peer'] != host and bans[0]['peer'] == host
                    and bans[0].get('target_peer') in names - {host, silence[0]['peer']}
                    and all(other['tick'] < fault['tick'] for other in preparation)
                    and all(type(silence[0].get(key)) is int and silence[0][key] > 0 for key in ('duration_ms', 'duration_ticks'))):
                raise ValueError('HL4 permits only one non-host silence and one host moderation-ban of a different seat before host-kill')
        incarnation = fault.get('incarnation', incarnations[fault['peer']])
        restarting = fault['action'] in ('announced-leave-rejoin', 'crash-restart')
        fault.update(id=fault.get('id', f'fault-{number}'), incarnation=incarnation,
                     return_incarnation=incarnation+int(restarting),
                     deadline_ms=fault.get('deadline_ms', options.recovery_deadline_ms),
                     outcomes=fault.get('outcomes', ['first_controllable_input', 'match_over_goodbye']))
        if restarting: incarnations[fault['peer']] = incarnation + 1
    return faults


def require_distinct_machines(preflights):
    if len({value['machine_id'] for value in preflights.values()}) != len(preflights):
        raise RuntimeError('each declared box must resolve to a distinct real machine')


def host_stall_spec(value):
    """TICK:MS for the engine's per-round live stall: a positive tick and 1..20000 ms, as the engine accepts."""
    tick, _, ms = value.partition(':')
    if not (tick.isdigit() and ms.isdigit() and int(tick) > 0 and 0 < int(ms) <= 20000):
        raise argparse.ArgumentTypeError('expected TICK:MS with a positive tick and 1..20000 ms')
    return f'{int(tick)}:{int(ms)}'


def make_plan(options):
    manifest = load_boxes(options.boxes, options.roster)
    boxes = {b['name']: b for b in manifest['boxes']}
    peers = manifest['instances']
    hosts = [p for p in peers if p['name'] == options.host or p['box'] == options.host]
    if len(hosts) != 1: raise ValueError('--host must select exactly one engine instance')
    host = hosts[0]['name']
    stem = options.out.name
    if not re.fullmatch(r'[A-Za-z0-9_-]+', stem): raise ValueError('run name must be a simple directory leaf')
    faults = schedule_for(options, peers, boxes)
    if any(f['peer']==host and f['action'] in ('crash-restart','announced-leave-rejoin') for f in faults):
        raise ValueError('host removal by restart/rejoin has no four-box oracle; use the explicit host-kill HL4 arm')
    barriers = json.loads(options.barriers.read_text(encoding='utf-8')) if options.barriers else []
    for barrier in barriers:
        if barrier['peer'] not in {p['name'] for p in peers} or barrier['phase'] not in ('capture_announced','writer_pending') or \
                not 1 <= barrier['timeout_ms'] <= 120000 or barrier['tick'] < 1:
            raise ValueError('invalid capture barrier target, phase, tick or deadline')
    specs = []
    for peer in peers:
        box = boxes[peer['box']]
        root = str(PurePosixPath(box['scratch']) / stem)
        own = str(PurePosixPath(root) / peer['name'] / 'incarnation-0')
        flags = ['-seed', str(options.seed), '-max-ticks', str(options.ticks),
                 '-net-live-tick-hashes', own + '/live.jsonl', '-out', own + '/trace.json',
                 '-input-script', own + '/input.txt', '-feel-measure', own + '/engine/feel',
                 '-feel-render-settings', own + '/engine/runtime/Userdata/FeelRender.ini',
                 '-net-match-service-e2e', '-net-port', str(peer['port_block'][0]),
                 '-net-match-ticks', str(options.ticks - 1), '-net-match-peers', str(len(peers)),
                 '-net-match-humans', str(sum(p.get('seat', 'player') == 'player' for p in peers)),
                 '-net-match-mode', 'pvp-skirmish' if options.roster == 'four-way' else 'coop-pve' if options.roster == 'ai-heavy' else 'pvpve',
                 '-net-match-cpu-slots', '0' if options.roster == 'four-way' else '2' if options.roster == 'ai-heavy' else '1',
                 '-net-match-service-preset', 'Multi Box Combat', '-net-match-service-module', 'UserScenes.rte',
                 '-net-match-service-scene', options.scene, '-net-match-service-scene-module', 'Base.rte',
                 '-net-match-auto-delay', '-net-local-prediction', 'on', '-net-ice', 'on', '-net-player-name', peer['name'],
                 '-net-reconnect-ticket', str(PurePosixPath(root) / peer['name'] / 'participant.ticket'), '-net-match-report', own + '/match-report.json',
                 '-net-cross-schedule', own + '/faults.json', '-net-cross-host-options', own + '/host-options.json']
        if options.fullstate_every:
            flags += ['-net-fullstate-hash-every', str(options.fullstate_every), '-net-fullstate-dump', own + '/fullstate']
        if getattr(options, 'memory_census_ticks', 0):
            flags += ['-memory-census-ticks', str(options.memory_census_ticks)]
        if peer['name'] == host:
            flags += ['-net-host', '-net-replay-out', own + '/match.ccreplay']
            # The host's simulation stalls every round for the forced host-stall arm; its link and its plane stay live.
            if getattr(options, 'host_stall', None): flags += ['-net-test-live-stall-each-round', options.host_stall]
            if options.scenario != 'match': flags += ['-net-autosave-seconds', '180']
        else:
            flags += ['-net-join-session', '<published-session-id>']
        if options.scenario != 'match':
            flags += ['-net-cross-rematches', '4096']
        specs.append(dict(peer=peer['name'], box=peer['box'], role='host' if peer['name'] == host else peer.get('seat', 'player'),
            incarnation=0, root=root, own=own, repo=box['tree'], executable=box['executable'], flags=flags,
            userdata=own+'/engine/runtime/Userdata', participant_key_root=own+'/engine/runtime/Userdata',
            port_block=peer['port_block'], under_load_by_design=box['peers_per_box']>1,
            env={**box.get('environment', {}), 'CCCP_HEADLESS': '1', 'PYTHONDONTWRITEBYTECODE': '1', 'CC_TEST_CROSS_RECORDS': own + '/events.jsonl',
                 'CC_TEST_CROSS_RUN': stem, 'CC_TEST_CROSS_INSTANCE': peer['name'], 'CC_TEST_CROSS_EXECUTION': 'process-0',
                 'CC_TEST_CROSS_INCARNATION': '0', 'CC_TEST_NET_UI_SCRIPT': own + '/probe.json',
                 'CC_TEST_CROSS_RECOVERIES': own + '/recoveries.json',
                 'CC_TEST_CROSS_BOT': own + '/bot.json', 'CC_TEST_CROSS_EVENT_RAW_LIMIT': str(64*1024**3)}, timeout=options.timeout, ticks=options.ticks,
            settings={}, roster=options.roster, scene=options.scene,
            # D1 changes the former EROL-PC host to Z13, preserving that host's workload settings.
            initial_skill=100 if boxes[hosts[0]['box']]['kind'] == 'windows-task' and hosts[0]['box'] != 'Z13' else 50,
            faults=[f for f in faults if f['peer'] == peer['name']], barriers=[b for b in barriers if b['peer']==peer['name']]))
        if getattr(options, 'keep_fullstate_sections', ''):
            specs[-1]['keep_fullstate_sections'] = [name for name in options.keep_fullstate_sections.split(',') if name]
            specs[-1]['env']['CC_TEST_FULLSTATE_DUMP_SECTIONS'] = ','.join(specs[-1]['keep_fullstate_sections'])
        if box['kind'] == 'windows-local':
            specs[-1]['settings'] = dict(options.local_setting)
            if options.local_render_cap is not None:
                specs[-1]['render_cap'] = options.local_render_cap
        specs[-1]['recoveries']=[f for f in specs[-1]['faults'] if f['action'] not in ('brain-eliminate', 'host-kill', 'silence', 'moderation-ban')]
        specs[-1]['forced_ends']=[f for f in faults if f['action']=='brain-eliminate']
        if specs[-1]['barriers']:
            specs[-1]['env']['CC_TEST_CROSS_CAPTURE_BARRIER'] = own+'/barriers.json'
    return dict(version=1, run=stem, lane=LANE, mac_guard=MAC_GUARD, started=dt.datetime.now(MST).strftime('%Y-%m-%d %I:%M:%S %p MST'),
                driver_commit=command(['git','-C',HERE.parent,'rev-parse','HEAD']).strip(),
                driver_tracked_changes=command(['git','-C',HERE.parent,'status','--porcelain','--untracked-files=no']).splitlines(),
                driver_sources={str(path.relative_to(HERE)):digest_file(path) for path in
                    (HERE/'cross_peers.py',HERE/'cross_report.py',HERE/'feel/report.py',HERE/'feel/records.py')},
                boxes=manifest['boxes'], driver=manifest.get('driver'), instances=peers, specs=specs, host=host, ticks=options.ticks,
                scenario=options.scenario, acceptance_row={'match': 17, 'soak': 18, 'chaos': 19}[options.scenario],
                acceptance_arm=getattr(options, 'acceptance_arm', None),
                roster=options.roster, scene=options.scene, seed=options.seed,
                chaos_seed=options.chaos_seed if options.scenario == 'chaos' else None,
                seed_scope='choices only; transport and OS timing are not reproduced', faults=faults,
                capture_barriers=barriers,
                overlap_policy='ordered by committed unique gameplay budget; early ends carry outstanding triggers',
                deadlines=dict(recovery_ms=options.recovery_deadline_ms, capture_ms=options.capture_budget_ms,
                               launch_s=options.timeout, reservation_s=options.timeout, payload_ready_s=180, payload_release_s=360, session_publication_s=180),
                memory=dict(warmup_s=120, slope_bytes_per_minute=8*1024*1024,
                            retained_bytes=128*1024*1024, sample_seconds=60),
                storage=dict(total_bytes=LIMIT, event_bytes_per_instance=256*1024*1024,
                             event_expanded_bytes_per_instance=64*1024**3,
                             live_bytes_per_instance=512*1024*1024, failure_window_ticks=600,
                             presentation_chunk_bytes=8*1024*1024, presentation_retained_chunks=16,
                             fullstate_dump_captures_per_incarnation=2, periodic_pngs_per_incarnation=10,
                             diagnostic_window='Last two writer-complete full-state dump directories and ten completed periodic PNGs per incarnation. All native hashes, scope lines, live hashes, event records and retirement SHA256 receipts retained. No tick-duration guarantee; win/lobby screenshots retained.',
                             presentation_window='Last 16 sealed gzip chunks plus one active chunk, at most 136 MiB expanded; no tick-duration guarantee. Feel statistics cover retained rows only.'),
                quiet_window=options.quiet_window, pathfinding='production asynchronous; no -tick-hashes override',
                fullstate_every=options.fullstate_every,
                host_stall=getattr(options, 'host_stall', None),
                capture_rows_pending=[],
                required_gates=['three_real_boxes', 'matching_content', 'same_commit', 'full_history', 'zero_desync',
                                'zero_unscheduled_holds', 'native_completion', 'bounded_recovery'],
                limitations={'migration': 'HL4 requires actual host termination and native successor/moderation evidence; no host restart/rejoin oracle',
                             'team_members': 'NOT COVERED until netcode row 14 lands',
                             'peer_counts': 'NOT COVERED until netcode row 13 lands'})


def coordinator(plan):
    driver = plan.get('driver')
    if driver:
        if (driver.get('kind') != 'coordinator' or type(driver.get('directory_port')) is not int
                or not 1 <= driver['directory_port'] <= 65535):
            raise ValueError('the driver needs kind=coordinator and a valid directory_port')
        if any(box['kind'] == 'windows-local' for box in plan['boxes']):
            raise ValueError('a driver-only declaration cannot include a local game peer')
        return driver
    local = [box for box in plan['boxes'] if box['kind'] == 'windows-local']
    if len(local) != 1:
        raise ValueError('declare a separate coordinator when no game peer is windows-local')
    return local[0]


def dry_run(plan):
    print(json.dumps(plan, indent=2))
    local = coordinator(plan)
    for box in plan['boxes']:
        root = str(PurePosixPath(box['scratch']) / plan['run'])
        if box['kind'] != 'windows-local':
            print(shlex.join(['scp', '-q', '<box-payload.json>', f'{box["ssh"]}:{root}/payload.json']))
            print(shlex.join(['ssh', '-N', '-o', 'ExitOnForwardFailure=yes', '-R',
                  f'127.0.0.1:{box["directory_port"]}:127.0.0.1:{local["directory_port"]}', box['ssh']]))
        if box['kind'] == 'windows-task':
            print(shlex.join(['scp', '-q', '<staged-task.ps1>', f'{box["ssh"]}:{box["task_script"]}']))
            print(shlex.join(['ssh', box['ssh'], 'Start-ScheduledTask -TaskName ' + box['runner']]))
        else:
            cmd = [box['python'], box['tree'] + '/tools/cross_peers.py', '--payload', root + '/payload.json']
            print(shlex.join(['ssh', box['ssh'], shlex.join(cmd)] if box['kind'] == 'posix-ssh' else cmd))
        print(f'preflight {box["name"]}: build identity, executable, Data and staged module hashes, load, guard, scratch bytes')
    print('DRY RUN: no files, processes, listeners or engines created')


def inventory_guard():
    paths = list(Path('D:/mx').glob('inventory-confirming-*/steps.log'))
    if not paths: return None
    newest = max(paths, key=lambda p: p.stat().st_mtime)
    # Only the newest stream owns the box; old unfinished logs stay evidence.
    with newest.open('rb') as stream:
        stream.seek(max(0,newest.stat().st_size-65536))
        tail=stream.read().decode('utf-8',errors='replace').splitlines()
        last = next(reversed([line.strip() for line in tail if re.search(r'\] S\d+ (start|exit)', line)]), '')
    return f'{newest}: {last}' if re.search(r'\] S3 start\s*$', last) else None


def scratch_bytes(root):
    total = 0
    for directory, names, files in os.walk(root, followlinks=False):
        names[:] = [n for n in names if not Path(directory, n).is_symlink()
                    and not getattr(Path(directory, n), 'is_junction', lambda: False)()]
        for name in files:
            path = Path(directory, name)
            try:
                if not path.is_symlink(): total += path.stat().st_size
            except FileNotFoundError:
                # Closed evidence is compressed and its verified source removed
                # by the owning payload while the coordinator samples storage.
                continue
    return total


def box_load(own_pids=()):
    if sys.platform == 'win32':
        script = "Get-CimInstance Win32_Process | Where-Object { $_.Name -match '^(Cortex Command.*|cl|link|MSBuild)\\.exe$' } | Select-Object ProcessId,Name,ExecutablePath | ConvertTo-Json -Compress"
        raw = command(['pwsh', '-NoProfile', '-Command', script]).strip()
        rows = json.loads(raw) if raw else []
        rows = rows if isinstance(rows, list) else [rows]
        return [row for row in rows if row['ProcessId'] not in own_pids]
    raw = command(['ps', '-axo', 'pid=,comm='])
    rows = []
    for line in raw.splitlines():
        fields = line.strip().split(None, 1)
        if len(fields) == 2 and re.search(r'CortexCommand|cc1plus|ninja|clang|g\+\+', fields[1]) and int(fields[0]) not in own_pids:
            rows.append(dict(ProcessId=int(fields[0]), Name=Path(fields[1]).name))
    return rows


def digest_file(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''): digest.update(block)
    return digest.hexdigest()


def content_manifest(repo):
    root = Path(repo) / 'Data'
    result = {}
    for directory, names, files in os.walk(root, followlinks=False):
        names[:] = sorted(n for n in names if not Path(directory, n).is_symlink())
        for name in sorted(files):
            path = Path(directory, name)
            if path.is_symlink(): raise RuntimeError(f'content symlink needs an explicit manifest: {path}')
            if path.suffix.lower() in ('.pyc', '.ds_store'): continue
            if path.name == '.DS_Store': continue
            result[path.relative_to(root).as_posix()] = digest_file(path)
    return result


def owns_reservation(box):
    if not box.get('exclusive_marker'): return False
    try: value=json.loads(Path(box['exclusive_marker']).read_text(encoding='utf-8'))
    except (OSError,ValueError): return False
    return isinstance(value,dict) and bool(value.get('token')) and value['token']==os.environ.get('CCCP_FEEL_MATRIX_RUN')


def acquire_reservation(box,root,timeout):
    marker=Path(box['exclusive_marker']); deadline=time.monotonic()+timeout; previous=os.environ.get('CCCP_FEEL_MATRIX_RUN')
    if owns_reservation(box):
        return dict(marker=str(marker),record=json.loads(marker.read_text(encoding='utf-8')),previous=previous,borrowed=True)
    while time.monotonic()<deadline:
        if marker.exists() or box_load(): time.sleep(.5); continue
        token=secrets.token_hex(24)
        value=dict(stream_root=str(root),stamp=dt.datetime.now(MST).isoformat(),pid=os.getpid(),token=token)
        try:
            with marker.open('x',encoding='utf-8') as stream: json.dump(value,stream)
        except FileExistsError: continue
        os.environ['CCCP_FEEL_MATRIX_RUN']=token
        claim=dict(marker=str(marker),record=value,previous=previous)
        if not owns_reservation(box):
            release_reservation(claim); raise RuntimeError('box reservation was replaced before launch')
        if box_load(): release_reservation(claim); time.sleep(.5); continue
        return claim
    raise TimeoutError(f'box reservation deadline after {timeout} seconds; no engine launched')


def release_reservation(claim):
    if claim.get('borrowed'):return False
    marker=Path(claim['marker']); removed=False
    try:
        value=json.loads(marker.read_text(encoding='utf-8'))
        if isinstance(value,dict) and value.get('token')==claim['record']['token']:
            marker.unlink(); removed=True
    except (OSError,ValueError): pass
    if claim['previous'] is None: os.environ.pop('CCCP_FEEL_MATRIX_RUN',None)
    else: os.environ['CCCP_FEEL_MATRIX_RUN']=claim['previous']
    return removed


def assert_box_guard(box):
    if box['kind']=='posix-ssh':
        from acceptance_posix_guard import assert_available
        assert_available({**os.environ,**box.get('environment',{})},required_free=box.get('guard_file'))
    if box.get('exclusive_marker') and Path(box['exclusive_marker']).exists() and not owns_reservation(box):
        raise RuntimeError(f'{box["name"]} launch guard active: exclusive measurement reservation {box["exclusive_marker"]}')
    if box.get('guard_file') and not Path(box['guard_file']).is_file():
        raise RuntimeError(f'{box["name"]} launch guard active: {box["guard_file"]} absent')
    if box['kind'] == 'windows-local':
        if reason := inventory_guard(): raise RuntimeError(reason)
        engines = [r for r in box_load() if 'Cortex Command' in r['Name'] and
                   str(r.get('ExecutablePath', '')).replace('\\', '/').lower() == box['executable'].lower()]
        if engines: raise RuntimeError('an engine of this lane is already running')
    size = scratch_bytes(box['scratch'])
    if size >= LIMIT: raise RuntimeError(f'scratch reached 4 GB: {box["scratch"]} bytes={size}; stopped without deletion')


def preflight_payload(path):
    payload = json.loads(Path(path).read_text(encoding='utf-8'))
    box = payload['box']
    assert_box_guard(box)
    repo = Path(box['tree'])
    content = content_manifest(repo)
    head = command(['git', '-C', repo, 'rev-parse', 'HEAD'], check=False).strip()
    stamp_path = Path(box.get('build_receipt') or repo / 'tools/cross_peers/build.json')
    build = json.loads(stamp_path.read_text(encoding='utf-8')) if stamp_path.is_file() else {}
    if sys.platform == 'win32':
        identity = command(['pwsh','-NoProfile','-Command','(Get-CimInstance Win32_ComputerSystemProduct).UUID']).strip()
        cpu = command(['pwsh','-NoProfile','-Command','(Get-CimInstance Win32_Processor).Name']).strip()
    elif sys.platform == 'darwin':
        hardware = command(['ioreg','-rd1','-c','IOPlatformExpertDevice'])
        match = re.search(r'"IOPlatformUUID"\s*=\s*"([^"]+)"', hardware)
        if not match: raise RuntimeError('Mac hardware identity unavailable')
        identity, cpu = match[1], command(['sysctl','-n','machdep.cpu.brand_string']).strip()
    else:
        identity, cpu = Path('/etc/machine-id').read_text().strip(), platform.processor()
    if not identity: raise RuntimeError('box hardware identity unavailable')
    result = dict(box=box['name'], kind=box['kind'], head=head, build=build,
                  machine_id=hashlib.sha256((platform.system()+':'+identity).encode()).hexdigest(),
                  hostname=platform.node(), cpu=cpu, architecture=platform.machine(), os=platform.platform(),
                  executable_sha256=digest_file(box['executable']), content=content,
                  modules={p: h for p, h in content.items() if p.endswith('/Index.ini')},
                  fixture={name: digest_file(repo / 'tools/feel' / name) for name in ('CrossCombat.lua', 'CrossCombat.ini')},
                  load=box_load(), scratch_bytes=scratch_bytes(box['scratch']))
    if any(acceptance_cross.is_row(s) for s in payload.get('specs', ())):
        acceptance_cross.preflight_driver(box, result)
    if any(s.get('acceptance_row') in ('mod-match', 'mod-refusal') for s in payload.get('specs', ())):
        acceptance_cross.preflight_mod(box, result)
    write_json(Path(path).parent / 'preflight.json', result)
    print(f'preflight {box["name"]} files={len(content)} exe={result["executable_sha256"][:16]}')
    return 0


def stage_combat(run, spec):
    from feel_measure import private_settings
    private_settings(run, spec.get('render_cap', 60))
    module = Path(run.cwd) / 'Userdata/UserScenes.rte'
    module.mkdir(exist_ok=True)
    for source, target in [('CrossCombat.lua', 'CrossCombat.lua'), ('CrossCombat.ini', 'Index.ini')]:
        (module / target).write_bytes((Path(spec['repo']) / 'tools/feel' / source).read_bytes())
    (Path(spec['own']) / 'engine/feel').mkdir(exist_ok=True)


def engine_pid(run):
    if getattr(run, 'hop', False):
        state = Path(run.hop_state)
        if state.is_file(): return json.loads(state.read_text(encoding='utf-8')).get('engine_pid')
        return None
    return run.record.get('pid')


def sample_memory(run):
    if sys.platform == 'win32':
        from soak_two_peer import process_memory
        return process_memory(run)
    pid = engine_pid(run)
    if not pid: return None
    fields = command(['ps', '-o', 'rss=,vsz=', '-p', str(pid)], check=False).split()
    return dict(resident=int(fields[0])*1024, virtual=int(fields[1])*1024) if len(fields) == 2 else None


def retain_checkpoints(run, spec, *, final=False):
    if acceptance_cross.is_row(spec):
        return
    from feel.records import inspect_checkpoint
    import shutil
    own = Path(spec['own']); retained = own / 'archives'
    retained.mkdir(exist_ok=True)
    for path in sorted((Path(run.cwd)/'Autosaves').glob('*.ccsave')):
        if (retained/path.name).is_file(): continue
        checked = inspect_checkpoint(path)
        if not checked['passed'] and not final: continue
        checked.update(peer=spec['peer'],incarnation=spec['incarnation'],observed_wall_ms=time.monotonic()*1000)
        if checked['passed']:
            if scratch_bytes(Path(spec['root']).parent) + 2*path.stat().st_size >= LIMIT:
                raise RuntimeError('checkpoint retention would reach the 4 GB scratch cap')
            for source in (path,path.with_suffix('.ccmanifest')):
                if source.is_symlink(): raise RuntimeError('checkpoint is a symlink')
                target=retained/source.name; shutil.copyfile(source,target)
                if digest_file(source)!=digest_file(target): raise RuntimeError('retained checkpoint differs from its source')
            checked['retained']=str((retained/path.name).relative_to(own))
        with (own/'archives.jsonl').open('a',encoding='utf-8') as index:
            index.write(json.dumps(checked)+'\n')


def prepare_instance(spec, pin, box, runtime=None):
    from run_sim_test import make_run, seed_settings, RUNTIME_SETTINGS
    if box['kind'] != 'windows-local' and (spec.get('settings') or 'render_cap' in spec):
        raise ValueError('local settings and render cap require a windows-local instance')
    cap = render_cap_hz(spec.get('render_cap', 60))
    settings = {'SessionDirectoryUrl': f'127.0.0.1:{box["directory_port"]}', 'SessionDirectoryCertSha256': pin,
                'SessionDirectoryInstallKey': f'cross-{spec["peer"]}-install', 'NetworkIceEnable': '1',
                'NetworkConnectionMode': 'DirectOnly', 'NetworkHostRelayMode': 'Off'}
    settings.update(spec.get('settings', {}))
    if acceptance_cross.is_row(spec):
        settings = acceptance_cross.directory_settings(spec, settings)
    own = Path(spec['own']); own.mkdir(parents=True, exist_ok=False)
    rows = []
    for start in range(1, spec['ticks'] + 1, 240):
        end = min(start + 179, spec['ticks'])
        direction = 'L_RIGHT' if (start // 240) % 2 == 0 else 'L_LEFT'
        rows += [f'player=0 {start} {end} {direction} FIRE AIM=0.9,-0.1',
                 f'player=0 {start+180} {min(start+210, spec["ticks"])} WEAPON_RELOAD'] if start+210 <= spec['ticks'] else [f'player=0 {start} {end} FIRE AIM=-0.9,-0.1']
    (own / 'input.txt').write_text('\n'.join(rows) + '\n', encoding='utf-8')
    write_json(own / 'faults.json', [dict(f, action='outage', declared_action='silence') if f['action'] == 'silence' else f
                                  for f in spec['faults'] if f['incarnation'] == spec['incarnation'] and f['action'] != 'host-kill'])
    write_json(own / 'recoveries.json',dict(cases=spec.get('recoveries',spec['faults']),starts=spec.get('recovery_starts',{}),forced_ends=spec.get('forced_ends',[])))
    write_json(own / 'barriers.json',spec.get('barriers',[]))
    teams = dict(human_teams=[0,0,1], cpu_teams=[2]) if spec['roster'] in ('mixed','allies') else \
            dict(human_teams=[0,0,0], cpu_teams=[1,2]) if spec['roster'] == 'ai-heavy' else {}
    write_json(own / 'host-options.json', [dict(difficulty=spec['initial_skill'], ai_skill=spec['initial_skill'], fog=False,
                                              scene=spec['scene'], scene_module='Base.rte', **teams),
                                          dict(difficulty=100, ai_skill=100, fog=True, scene='Ketanot Hills', scene_module='Base.rte', **teams)])
    write_json(own / 'bot.json', [dict(round=0, **{'from': 3601, 'to': spec['ticks']})] if spec['ticks'] >= 3601 else [])
    write_json(own / 'probe.json', dict(schema=1, timeout_ms=120000, activate_at_tick=1, activate_phase='Running', repeat_rounds=True, steps=[
        dict(op='assert_window', equals=dict(width=int(settings.get('ResolutionX', 960)), height=int(settings.get('ResolutionY', 540)))),
        dict(op='assert_buy', input_player=0), dict(op='assert_pie', input_player=0), dict(op='finish')]))
    if box['kind'] == 'posix-ssh':
        configure_posix_box(box)
        os.environ['CCCP_TEST_BINARY'] = spec['executable']
        os.environ['CCCP_POSIX_HOP'] = 'ssh' if sys.platform=='darwin' else 'off'
    fresh_acceptance = runtime is None and acceptance_cross.is_row(spec)
    if fresh_acceptance and spec['acceptance_row'].startswith('mod-'):
        runtime = acceptance_cross.prepare_mod_runtime(spec)
    run = make_run(Path(spec['repo']), spec['flags'], own / 'engine', timeout=spec['timeout'], env=spec['env'], runtime=runtime)
    if fresh_acceptance and runtime is not None:
        # A fresh private mod overlay was seeded by prepare_mod_runtime. The
        # retained-runtime runner records only its path, so carry those actual
        # defaults before the measurement settings update their receipt.
        receipt_path = Path(run.out) / 'runtime.json'
        receipt = json.loads(receipt_path.read_text(encoding='utf-8'))
        receipt.setdefault('settings_overrides', dict(RUNTIME_SETTINGS))
        write_json(receipt_path, receipt)
    if fresh_acceptance or runtime is None:
        if acceptance_cross.is_row(spec):
            acceptance_cross.stage_activity(run, spec)
        else:
            stage_combat(run, spec)
    else:
        (own / 'engine/feel').mkdir(exist_ok=True)
    seed_settings(run, settings)
    render_path = Path(run.cwd) / 'Userdata/FeelRender.ini'
    render_path.write_text(f'RenderCapHz = {cap}\n', encoding='utf-8')
    if '-feel-render-settings' in run.argv:
        run.argv[run.argv.index('-feel-render-settings') + 1] = str(render_path)
    else:
        run.argv += ['-feel-render-settings', str(render_path)]
    metadata = json.loads((Path(run.out) / 'runtime.json').read_text(encoding='utf-8'))
    metadata.setdefault('settings_overrides', {}).update(settings)
    metadata['settings_sha256'] = digest_file(Path(run.cwd) / 'Userdata/Settings.ini')
    metadata['feel_render_settings'] = dict(path=str(render_path), sha256=digest_file(render_path))
    metadata['render_cap'] = cap
    write_json(Path(run.out) / 'runtime.json', metadata)
    return run


class Tail:
    def __init__(self, path):
        self.path, self.offset, self.pending, self.part = Path(path), 0, b'', 0
        self.drained=False

    def read(self):
        self.drained=False
        path = self.path if not self.part else Path(str(self.path) + f'.part{self.part}')
        if not path.is_file(): return []
        with path.open('rb') as stream:
            stream.seek(self.offset); block = stream.read(4*1024*1024); self.offset += len(block)
        pieces = (self.pending + block).split(b'\n'); self.pending = pieces.pop()
        rows = [json.loads(line) for line in pieces if line.strip()]
        self.drained=not self.pending and self.offset>=path.stat().st_size and not Path(str(self.path)+f'.part{self.part+1}').is_file()
        if not block and not self.pending and Path(str(self.path) + f'.part{self.part+1}').is_file():
            self.part += 1; self.offset = 0
        return rows

    def compress_consumed(self):
        from feel.records import compress_closed_record
        for number in range(self.part):
            path = self.path if number == 0 else Path(str(self.path)+f'.part{number}')
            if path.is_file(): compress_closed_record(path, self.path.parent)
        retained=sum(path.stat().st_size for path in self.path.parent.glob('events.jsonl*') if path.is_file())
        if retained >= 256*1024*1024:
            raise RuntimeError(f'event retention budget reached for {self.path}: {retained} bytes')


def restart_spec(spec, progress):
    next_spec = copy.deepcopy(spec)
    next_spec['incarnation'] += 1
    old = spec['own']
    own = str(PurePosixPath(old).parent / f'incarnation-{next_spec["incarnation"]}')
    next_spec['own'] = own
    next_spec['flags'] = [flag.replace(old, own) for flag in spec['flags']]
    next_spec['flags'].append('-net-cross-ticket-rejoin')
    next_spec['env'] = {key: str(val).replace(old, own) for key,val in spec['env'].items()}
    next_spec['env'].update(CC_TEST_CROSS_INCARNATION=str(next_spec['incarnation']),
                            CC_TEST_CROSS_EXECUTION=f'process-{next_spec["incarnation"]}',
                            CC_TEST_CROSS_BUDGET_BASE=str(progress.get('budget_base', 0)),
                            CC_TEST_CROSS_MATCH_FIRST_TICK=str(progress.get('first_gameplay_tick', 1)))
    next_spec['faults'] = [f for f in next_spec['faults'] if f['tick'] > progress.get('budget_tick', 0)]
    return next_spec


def termination_receipt(fault, spec, pid, progress, record, polled, before, after):
    """Publish only after the owning process wrapper observed termination and finish."""
    terminated = (type(pid) is int and pid > 0 and polled is not None and
                  record.get('exit_code') == polled and not record.get('timed_out') and
                  record.get('injected_termination') == 'scheduled crash ' + fault['id'])
    return dict(id=fault['id'], peer=spec['peer'], incarnation=spec['incarnation'],
                execution=progress.get('execution'), action=fault['action'],
                engine_pid=pid, process_terminated=terminated, exit_code=polled,
                before_wall_ms=before, after_wall_ms=after, actual=progress)


def crash_due(fault, progress, in_lobby):
    """A scheduled crash-restart is due at its budget tick; a lobby one only inside a rematch lobby, a round one only early in a
    running round (its frame between round_frame_from and round_frame_to), so the relaunch can return inside that round."""
    if progress.get('budget_tick', 0) < fault['tick']: return False
    phase = fault.get('phase', 'play')
    if phase == 'lobby': return in_lobby
    if phase == 'round':
        frame = progress.get('applied_frame', 0)
        return not in_lobby and fault.get('round_frame_from', 300) <= frame <= fault.get('round_frame_to', 600)
    return True


def seal_evidence(own):
    """Compress only closed instance records; retain verified original-byte digests."""
    from feel.records import compress_closed_record
    own = Path(own).resolve()
    candidates = [own / 'live.jsonl', own / 'engine/feel/raw.jsonl']
    candidates += list(own.glob('events.jsonl*'))
    if (own / 'fullstate').is_dir():
        candidates += [p for p in (own / 'fullstate').rglob('*') if p.is_file()]
    for path in candidates:
        if path.is_file() and not path.is_symlink() and path.suffix not in ('.gz', '.partial', '.json'):
            if not path.resolve().is_relative_to(own): raise RuntimeError('record leaves its instance root')
            compress_closed_record(path, own)


def configure_posix_box(box):
    if sys.platform.startswith('linux'):
        session = command(['systemctl', '--user', 'show-environment'], timeout=10, check=False)
        for line in session.splitlines():
            key, _, value = line.partition('=')
            if key in ('DISPLAY', 'XAUTHORITY', 'XDG_RUNTIME_DIR'):
                os.environ[key] = value
    os.environ.update(box.get('environment', {}))


def read_capabilities(box, root):
    from run_sim_test import make_run
    assert_box_guard(box)
    if box['kind'] == 'posix-ssh':
        configure_posix_box(box)
        os.environ['CCCP_TEST_BINARY'] = box['executable']
        os.environ['CCCP_POSIX_HOP'] = 'ssh' if sys.platform == 'darwin' else 'off'
    run = make_run(Path(box['tree']), ['-net-cross-capabilities'], root / 'capabilities', timeout=30,
                   env={'CCCP_HEADLESS': '1'})
    try:
        record = run.start().finish()
        if record.get('exit_code') != 0: raise RuntimeError('binary capability query failed')
        with (root / 'capabilities/stdout.log').open(encoding='utf-8-sig', errors='replace') as stream:
            values = [json.loads(line.split('] ', 1)[1]) for line in stream if line.startswith('[cross-capabilities] ')]
        if len(values) != 1 or values[0].get('schema') != 1 or not values[0].get('peer_limit'):
            raise RuntimeError('binary did not produce one valid capability record')
        write_json(root / 'capabilities.json', dict(**values[0], executable_sha256=digest_file(box['executable'])))
        return values[0]
    finally:
        run.close()


def wait_for_payload_release(root, seconds):
    deadline=time.monotonic()+seconds
    while True:
        if (root/'stop.json').is_file(): raise RuntimeError('coordinator cancelled before engine launch')
        if (root/'launch-go.json').is_file(): return
        if time.monotonic()>=deadline: raise TimeoutError('all-box launch release deadline')
        time.sleep(.2)


def release_ready_payloads(boxes,payloads,root,processes,seconds):
    deadline=time.monotonic()+seconds
    ready=set()
    while len(ready)<len(boxes) and time.monotonic()<deadline:
        for box in boxes.values():
            box_root=payloads[box['name']][2]
            if box['name'] in processes and processes[box['name']].poll() is not None:
                raise RuntimeError(f'{box["name"]}: owning payload exited before all-box launch release')
            if ((root/'done.json').is_file() if box['kind']=='windows-local' else remote_exists(box,box_root+'/done.json')):
                raise RuntimeError(f'{box["name"]}: owning payload exited before all-box launch release')
            if box['name'] in ready: continue
            if ((root/'launch-ready.json').is_file() if box['kind']=='windows-local' else remote_exists(box,box_root+'/launch-ready.json')):
                ready.add(box['name'])
        if (root/'stop.json').is_file(): raise RuntimeError('coordinator cancelled before all-box readiness')
        if len(ready)<len(boxes): time.sleep(.2)
    if len(ready)!=len(boxes): raise RuntimeError('not every box reached guarded launch readiness before the declared deadline')
    release=root/'launch-go.json'
    write_json(release,dict(boxes=sorted(ready),released_wall_ms=time.time()*1000))
    for box in boxes.values():
        if box['kind']!='windows-local': stage_remote(box,release,payloads[box['name']][2]+'/launch-go.json')
    return ready


def launch_guard_selftest(root):
    """Exercise the actual QUNS wait and all-box barrier without constructing an engine."""
    from unittest.mock import patch
    import run_sim_test
    import win32_test_runner as runner
    root=Path(root); root.mkdir(parents=True,exist_ok=True)
    if (root/'launch-go.json').exists(): raise FileExistsError('self-test needs fresh release evidence')
    boxes={name:dict(name=name,kind=kind) for name,kind in
           [('local','windows-local'),('task','windows-task'),('posix','posix-ssh')]}
    payloads={name:(None,None,str(root if name=='local' else root/name)) for name in boxes}
    for name in ('task','posix'):
        (root/name).mkdir(); write_json(root/name/'launch-ready.json',dict(simulated_capability=True))
    states=[]; observations=[]; pending=iter(['QUNS_BUSY','QUNS_BUSY',None]); advanced=False
    def query():
        state=next(pending); states.append(state)
        if (root/'launch-go.json').exists(): raise AssertionError('release preceded guard clearance')
        observations.append(dict(kind='guard',state=state))
        return state
    def advance(_seconds):
        nonlocal advanced
        if advanced: return
        advanced=True
        runner.wait_while_user_fullscreen({},lambda:None,limit_seconds=3,poll_seconds=1)
        write_json(root/'launch-ready.json',dict(simulated_capability=True))
        observations.append(dict(kind='local_capability_ready'))
    def publish(_box,source,destination):
        Path(destination).write_bytes(Path(source).read_bytes())
        observations.append(dict(kind='release',path=destination))
    with patch.dict(os.environ,{'CC_RUNNER_IGNORE_FULLSCREEN':'0'}), \
         patch.object(runner,'user_fullscreen_state',side_effect=query), \
         patch.object(runner,'create_process',side_effect=AssertionError('engine creation forbidden')) as native, \
         patch.object(run_sim_test,'make_run',side_effect=AssertionError('engine construction forbidden')) as make, \
         patch.object(time,'sleep',side_effect=advance), \
         patch(__name__+'.remote_exists',side_effect=lambda _box,path:Path(path).is_file()), \
         patch(__name__+'.stage_remote',side_effect=publish):
        ready=release_ready_payloads(boxes,payloads,root,{},2)
        wait_for_payload_release(root,0)
    result=dict(passed=states==['QUNS_BUSY','QUNS_BUSY',None] and len(ready)==3,
        engine_launches=native.call_count+make.call_count,states=states,
        release_after_last_guard_clear=all(row['kind']!='release' for row in observations[:3]),observations=observations,
        scope='Simulated QUNS transitions through the production runner wait and coordinator barrier; no engine, SSH or task launch.')
    write_json(root/'guard-selftest.json',result)
    return result


def refuse_mixed_build(preflight, box, spec, run):
    """An engine whose runner hashed another executable than this box's preflight is a mixed build, never a match."""
    expected, actual = preflight.get('executable_sha256'), run.record.get('exe_sha256')
    if not expected or actual != expected:
        raise RuntimeError(f'{box["name"]}: mixed build refused: {spec["peer"]} incarnation {spec["incarnation"]} started '
                           f'executable sha256 {actual} but the preflight hashed {expected}')


def run_payload(path):
    from run_sim_test import make_run
    from feel.records import CaptureSealer, RecoveryLedger, NativeFaultEffects, LobbyWatch
    payload = json.loads(Path(path).read_text(encoding='utf-8'))
    root, box = Path(path).parent, payload['box']
    runs, started, completed, readers, progress, fired = {}, {}, set(), {}, {}, set()
    capture_sealers = {}
    lobby_watches = {}
    effect_readers = {}; effect_rows = {}
    recovery_ledgers={}; clean_reads={}
    specifications = {s['peer']: s for s in payload['specs']}
    next_sample, verdict = 0, 0
    try:
        assert_box_guard(box)
        write_json(root / 'payload-owned.json', dict(box=box['name'], runner_pid=os.getpid()))
        preflight = json.loads((root / 'preflight.json').read_text(encoding='utf-8'))
        if box['kind'] == 'windows-local' and len(payload['specs']) != 1:
            raise RuntimeError('this lane permits only one local engine at a time; dry-run supports larger manifests')
        capabilities = read_capabilities(box, root)
        requested = payload['specs'][0]['flags']
        requested = int(requested[requested.index('-net-match-peers') + 1])
        if requested > capabilities['peer_limit']:
            raise RuntimeError(f'admission refused by build capability: requested {requested} peers, limit {capabilities["peer_limit"]}; Main clamps its legacy argument, so the driver refuses to silently shrink the roster')
        write_json(root/'launch-ready.json',dict(box=box['name'],runner_pid=os.getpid(),capabilities=capabilities))
        wait_for_payload_release(root,360)
        def launch_spec(spec):
            if spec['role'] != 'host':
                session_path = root / spec.get('session_leaf', 'session.json')
                deadline = time.monotonic() + spec.get('session_wait_s', 180)
                while not session_path.is_file() and time.monotonic() < deadline:
                    if (root/'stop.json').is_file():
                        raise RuntimeError('coordinator cancelled while waiting for the published session')
                    time.sleep(.2)
                if not session_path.is_file(): raise RuntimeError('session directory publication deadline')
                session = json.loads(session_path.read_text(encoding='utf-8'))['session']
                spec['flags'] = [session if flag == '<published-session-id>' else flag for flag in spec['flags']]
            assert_box_guard(box)
            run = prepare_instance(spec, payload['pin'], box)
            runs[spec['peer']] = run
            before_launch=time.monotonic()*1000
            run.start(); started[spec['peer']] = time.monotonic()
            refuse_mixed_build(preflight, box, spec, run)
            readers[spec['peer']] = Tail(Path(spec['own']) / 'events.jsonl')
            capture_sealers[spec['peer']] = CaptureSealer(spec['own'], captures=None if spec.get('preserve_evidence') or spec.get('keep_fullstate_sections') else 2)
            lobby_watches[spec['peer']] = LobbyWatch(Path(spec['own'])/'engine/stdout.log')
            effect_readers[spec['peer']]=NativeFaultEffects(Path(spec['own'])/'engine/stdout.log',spec['faults'],spec['incarnation'])
            effect_rows[spec['peer']]=[]
            progress[spec['peer']] = {}
            recovery_ledgers[spec['peer']]=RecoveryLedger(root/'recovery-observed.jsonl',spec['peer'],f'payload:{box["name"]}:{os.getpid()}',before_launch,spec.get('recoveries',spec['faults']))
            clean_reads[spec['peer']]=before_launch
            write_json(Path(spec['own']) / 'instance.json', spec)
            write_json(Path(spec['own']) / 'started.json', dict(peer=spec['peer'], incarnation=0, engine_pid=engine_pid(run)))
        for spec in payload['specs']:
            if not spec.get('defer_until_session'):
                launch_spec(spec)
        payload_deadline = time.monotonic()+max(s['timeout'] for s in specifications.values())+90
        with (root / 'samples.jsonl').open('w', encoding='utf-8') as samples:
            while len(completed) < len(specifications):
                now = time.monotonic()
                if now > payload_deadline: raise TimeoutError('payload hang guard expired, including any unreleased late join')
                if (root / 'stop.json').is_file(): raise RuntimeError('coordinator cancelled this box payload')
                if box.get('exclusive_marker') and Path(box['exclusive_marker']).exists() and not owns_reservation(box):
                    raise RuntimeError(f'{box["name"]}: exclusive measurement reservation appeared during this payload')
                if box['kind'] == 'windows-local' and (reason := inventory_guard()): raise RuntimeError(reason)
                if now >= next_sample:
                    own_pids = [engine_pid(r) for r in runs.values()]
                    load = box_load(own_pids)
                    all_sampled = True
                    for spec in specifications.values():
                        peer = spec['peer']; run = runs.get(peer)
                        if run is None or peer in completed: continue
                        measured = sample_memory(run)
                        all_sampled &= bool(measured and engine_pid(run))
                        row = dict(peer=peer, incarnation=spec['incarnation'], execution=f'process-{spec["incarnation"]}', engine_pid=engine_pid(run),
                                   elapsed_s=now-started[peer],payload_monotonic_ms=now*1000,load=load,same_box_instances=len(runs),
                                   **(measured or {}))
                        samples.write(json.dumps(row) + '\n'); samples.flush()
                        retain_checkpoints(run, spec)
                    next_sample = now + (60 if all_sampled else 1)
                    assert_box_guard({**box, 'kind': 'windows-task'} if box['kind'] == 'windows-local' else box)
                for spec in list(specifications.values()):
                    peer = spec['peer']
                    if peer not in runs:
                        if not (root / spec.get('session_leaf', 'session.json')).is_file(): continue
                        launch_spec(spec)
                    run = runs[peer]
                    if peer in completed: continue
                    read_began=time.monotonic()*1000
                    observed_rows=readers[peer].read()
                    if run.poll() is not None:
                        # An announced leave can exit with its final progress record
                        # behind a chunk boundary. Consume the sealed tail before
                        # deciding whether this incarnation requires a restart.
                        for _ in range(256):
                            if readers[peer].drained: break
                            observed_rows.extend(readers[peer].read())
                        if not readers[peer].drained: raise RuntimeError(f'{peer}: exited event stream did not drain')
                    observed_at=time.monotonic()*1000
                    recovery_ledgers[peer].observe(observed_rows,spec['incarnation'],clean_reads[peer],observed_at)
                    if readers[peer].drained: clean_reads[peer]=read_began
                    for observed in observed_rows:
                        if observed.get('type') == 'progress': progress[peer] = observed
                    if not spec.get('preserve_evidence'):
                        readers[peer].compress_consumed()
                    acceptance_cross.observe_soak(spec, run, now)
                    if not spec.get('preserve_evidence'):
                        capture_sealers[peer].poll()
                    if effects := effect_readers[peer].poll(observed_at):
                        effect_rows[peer]+=effects
                        write_json(Path(spec['own'])/'h4-effects.json',effect_rows[peer])
                    current = progress[peer]
                    # A lobby-phase drop waits for the rematch lobby, so the host loses the link where the round's seats are settled.
                    in_lobby = lobby_watches[peer].poll()
                    due = next((f for f in spec['faults'] if f['action'] in ('crash-restart', 'host-kill') and f['incarnation'] == spec['incarnation'] and f['id'] not in fired
                                and crash_due(f, current, in_lobby)), None)
                    leaving = next((f for f in spec['faults'] if f['action'] == 'announced-leave-rejoin' and f['incarnation'] == spec['incarnation'] and f['id'] not in fired
                                    and current.get('budget_tick', 0) >= f['tick']), None)
                    if due or (leaving and run.poll() is not None):
                        fault = due or leaving; fired.add(fault['id'])
                        if due and run.poll() is not None: raise RuntimeError(f'{peer}: process exited before scheduled crash {fault["id"]}')
                        receipt = dict(type='lifecycle', id=fault['id'], peer=peer, incarnation=spec['incarnation'],
                                       action=fault['action'], phase=fault.get('phase', 'play'), in_lobby=in_lobby, requested_tick=fault['tick'], actual=current,
                                       observed_wall_ms=time.monotonic()*1000, engine_pid=engine_pid(run))
                        with (root / 'lifecycle.jsonl').open('a', encoding='utf-8') as lifecycle:
                            lifecycle.write(json.dumps(receipt)+'\n')
                        retained = Path(run.cwd)
                        if due:
                            crash_before=time.monotonic()*1000
                            run.terminate(reason=f'scheduled crash {fault["id"]}')
                            if fault['action'] != 'host-kill':
                                recovery_ledgers[peer].external_start(fault,spec['incarnation'],crash_before,time.monotonic()*1000,current)
                        record = run.finish(); run.close()
                        if fault['action'] == 'host-kill':
                            for _ in range(256):
                                final_rows = readers[peer].read()
                                for observed in final_rows:
                                    if observed.get('type') == 'progress': current = observed
                                if readers[peer].drained: break
                            if not readers[peer].drained:
                                raise RuntimeError(f'{peer}: terminated host event stream did not drain')
                            terminated = termination_receipt(fault, spec, receipt['engine_pid'], current, record,
                                                             record.get('exit_code'), crash_before, time.monotonic()*1000)
                            with (root / 'terminations.jsonl').open('a', encoding='utf-8') as stream:
                                stream.write(json.dumps(terminated) + '\n')
                            if not terminated['process_terminated']:
                                raise RuntimeError(f'{peer}: host termination was not observed')
                            write_json(Path(spec['own']) / 'record.json', record)
                            retain_checkpoints(run, spec, final=True)
                            if not spec.get('preserve_evidence'):
                                seal_evidence(spec['own'])
                            completed.add(peer)
                            continue
                        if leaving and record.get('exit_code') == 0:
                            ended = time.monotonic() * 1000
                            recovery_ledgers[peer].fault_reset(fault['id'], spec['incarnation'], observed_at, ended,
                                dict(action='announced-leave-rejoin', process_exited=True, exit_code=record['exit_code']))
                        write_json(Path(spec['own']) / 'record.json', record)
                        retain_checkpoints(run, spec, final=True)
                        if not spec.get('preserve_evidence'):
                            seal_evidence(spec['own'])
                        new_spec = restart_spec(spec, current)
                        new_spec['recovery_starts']=recovery_ledgers[peer].restart_inputs(new_spec['incarnation'])
                        render = new_spec['flags'].index('-feel-render-settings') + 1
                        new_spec['flags'][render] = str(retained / 'Userdata/FeelRender.ini')
                        assert_box_guard(box)
                        fresh = prepare_instance(new_spec, payload['pin'], box, runtime=retained)
                        runs[peer] = fresh; specifications[peer] = new_spec
                        readers[peer] = Tail(Path(new_spec['own']) / 'events.jsonl')
                        capture_sealers[peer] = CaptureSealer(new_spec['own'], captures=None if new_spec.get('keep_fullstate_sections') else 2)
                        lobby_watches[peer] = LobbyWatch(Path(new_spec['own'])/'engine/stdout.log')
                        effect_readers[peer]=NativeFaultEffects(Path(new_spec['own'])/'engine/stdout.log',new_spec['faults'],new_spec['incarnation'])
                        effect_rows[peer]=[]
                        fresh.start(); started[peer] = time.monotonic()
                        refuse_mixed_build(preflight, box, new_spec, fresh)
                        write_json(Path(new_spec['own']) / 'instance.json', new_spec)
                        write_json(Path(new_spec['own']) / 'started.json', dict(peer=peer, incarnation=new_spec['incarnation'], engine_pid=engine_pid(fresh)))
                        continue
                    if run.poll() is not None or now - started[peer] > spec['timeout']:
                        record = run.finish(); run.close(); completed.add(peer)
                        for _ in range(64):
                            tail_rows=readers[peer].read()
                            observed_at=time.monotonic()*1000
                            recovery_ledgers[peer].observe(tail_rows,spec['incarnation'],clean_reads[peer],observed_at)
                            if readers[peer].drained or not tail_rows: break
                        write_json(Path(spec['own']) / 'record.json', record)
                        if (record.get('exit_code') != 0 or record.get('timed_out')) and not acceptance_cross.expected_refusal(spec, record): verdict = 1
                time.sleep(.05)
    except Exception as error:
        verdict = 1
        write_json(root / 'payload-error.json', dict(error=str(error)))
        print(f'PAYLOAD FAIL: {error}', flush=True)
    finally:
        for peer, run in runs.items():
            run.close()
            write_json(Path(specifications[peer]['own']) / 'record.json', run.record)
            try:
                retain_checkpoints(run, specifications[peer], final=True)
                acceptance_cross.restore_activity(specifications[peer])
                acceptance_cross.retain_native_screens(specifications[peer], run)
                if not specifications[peer].get('preserve_evidence'):
                    seal_evidence(specifications[peer]['own'])
            except Exception as error:
                verdict = 1
                write_json(root / 'seal-error.json', dict(peer=peer, error=str(error)))
        write_json(root / 'done.json', dict(exit_code=verdict,completed=sorted(completed),payload_monotonic_ms=time.monotonic()*1000,runner_pid=os.getpid()))
    return verdict


def remote_command(box, args):
    if box['kind'] == 'windows-task':
        return ['ssh', box['ssh'], '& ' + ' '.join(quote_ps(a) for a in args)]
    return ['ssh', box['ssh'], shlex.join(list(map(str, args)))]


def stage_remote(box, local, remote):
    # Payloads poll publication names. SCP creates a destination before its writer
    # closes, so expose the final name only after the transfer has completed.
    incoming=remote+f'.incoming-{os.getpid()}-{time.monotonic_ns()}'
    command(['scp', '-q', str(local), f'{box["ssh"]}:{incoming}'], timeout=120)
    publish=(f'Move-Item -LiteralPath {quote_ps(incoming)} -Destination {quote_ps(remote)} -Force'
             if box['kind']=='windows-task' else f'mv -f -- {shlex.quote(incoming)} {shlex.quote(remote)}')
    command(['ssh',box['ssh'],publish])


def remote_exists(box, path):
    script = f'if (Test-Path -LiteralPath {quote_ps(path)}) {{ "yes" }}' if box['kind'] == 'windows-task' else f'test -f {shlex.quote(path)} && printf yes'
    return command(['ssh', box['ssh'], script], check=False).strip() == 'yes'


def require_payload_success(box, outcome):
    if outcome.get('exit_code') != 0:
        raise RuntimeError(f'{box["name"]}: owning payload exited {outcome.get("exit_code")}; stop the other peers instead of continuing a reduced match')


def read_payload_outcome(box,path):
    if box['kind']=='windows-local':return json.loads(Path(path).read_text(encoding='utf-8'))
    script=f'Get-Content -LiteralPath {quote_ps(path)} -Raw' if box['kind']=='windows-task' else f'cat {shlex.quote(path)}'
    return json.loads(command(['ssh',box['ssh'],script]))


def fetch_box(box, root, local):
    import tarfile
    remote_tar = root + '/evidence.tar'
    # Runtime data and participant credentials never leave the owning box.
    if box['kind'] == 'windows-task':
        script = f"Set-Location -LiteralPath {quote_ps(root)}; tar.exe -cf {quote_ps(remote_tar)} --exclude='evidence.tar' --exclude='runtime' --exclude='*.ticket' --exclude='*.key' --exclude='key.pem' ."
    else:
        script = f"cd {shlex.quote(root)} && tar -cf {shlex.quote(remote_tar)} --exclude=evidence.tar --exclude=runtime --exclude='*.ticket' --exclude='*.key' --exclude=key.pem ."
    command(['ssh', box['ssh'], script], timeout=120)
    archive = local / 'evidence.tar'
    command(['scp', '-q', f'{box["ssh"]}:{remote_tar}', str(archive)], timeout=120)
    with tarfile.open(archive) as stream:
        for member in stream.getmembers():
            target = (local / member.name).resolve()
            if member.issym() or member.islnk() or not target.is_relative_to(local.resolve()):
                raise RuntimeError('unsafe evidence archive member')
            if member.isfile():
                target.parent.mkdir(parents=True, exist_ok=True)
                with stream.extractfile(member) as source, target.open('wb') as sink:
                    import shutil
                    shutil.copyfileobj(source, sink)
    archive.unlink()
    script = f'Remove-Item -LiteralPath {quote_ps(remote_tar)}' if box['kind'] == 'windows-task' else f'rm -- {shlex.quote(remote_tar)}'
    command(['ssh', box['ssh'], script])


def run_plan(plan, root):
    import edith_cross
    import test_directory_ice_join as directory
    root = root.resolve()
    if not root.is_relative_to(SCRATCH.resolve()) or root == SCRATCH.resolve():
        raise ValueError(f'run root must be inside {SCRATCH}')
    root.mkdir(parents=True, exist_ok=False)
    write_json(root / 'manifest.json', plan)
    boxes = {box['name']: box for box in plan['boxes']}
    local = coordinator(plan)
    local_peer = next((box for box in boxes.values() if box['kind'] == 'windows-local'), None)
    service, processes, tunnels, handles, preflights, payloads = None, {}, [], [], {}, {}
    findings, launched = [], set()
    claim=None
    try:
        if local_peer:
            claim=acquire_reservation(local_peer,root,plan['deadlines'].get('reservation_s',plan['deadlines']['launch_s']))
            plan['reservation']=dict(claim['record'],policy='One scenario; marker absent and no local Cortex Command process before atomic claim.')
        else:
            plan['driver_role'] = dict(box=local['name'], game_peer=False)
        write_json(root/'manifest.json',plan)
        for box in boxes.values():
            box_root = str(PurePosixPath(box['scratch']) / plan['run'])
            payload = dict(box=box, specs=sorted([s for s in plan['specs'] if s['box'] == box['name']],key=lambda s:s['role']!='host'), pin='pending')
            local_box = root / 'boxes' / box['name']; local_box.mkdir(parents=True)
            local_payload = local_box / 'payload.json'; write_json(local_payload, payload)
            if box['kind'] == 'windows-local':
                write_json(root / 'payload.json', payload)
                preflight_payload(root / 'payload.json')
                preflights[box['name']] = json.loads((root / 'preflight.json').read_text())
            else:
                if box.get('guard_file') and not remote_exists(box, box['guard_file']):
                    raise RuntimeError(f'{box["name"]}: inventory guard active: {box["guard_file"]} absent')
                mkdir = (f'New-Item -ItemType Directory -Path {quote_ps(box_root)} | Out-Null' if box['kind'] == 'windows-task'
                         else f'mkdir -p {shlex.quote(str(PurePosixPath(box_root).parent))} && mkdir {shlex.quote(box_root)}')
                command(['ssh', box['ssh'], mkdir])
                stage_remote(box, local_payload, box_root + '/payload.json')
                command(remote_command(box, [box['python'], box['tree'] + '/tools/cross_peers.py', '--preflight', box_root + '/payload.json']), timeout=180)
                command(['scp', '-q', f'{box["ssh"]}:{box_root}/preflight.json', str(local_box / 'preflight.json')], timeout=120)
                preflights[box['name']] = json.loads((local_box / 'preflight.json').read_text())
            payloads[box['name']] = (payload, local_payload, box_root)
        reference_box = local_peer or next(box for box in boxes.values() if box['kind'].startswith('windows'))
        reference = preflights[reference_box['name']]
        plan['source_sha'] = plan['driver_commit']
        require_distinct_machines(preflights)
        for box in boxes.values():
            value = preflights[box['name']]
            if any(value[k] != reference[k] for k in ('content', 'modules', 'fixture')):
                raise RuntimeError(f'{box["name"]}: content/module/fixture manifests differ before launch')
            if box['kind'].startswith('windows') and value['executable_sha256'] != reference['executable_sha256']:
                raise RuntimeError(f'{box["name"]}: Windows executable hash differs')
            if value['build'].get('commit') != plan['source_sha'] or value['build'].get('executable_sha256') != value['executable_sha256']:
                raise RuntimeError(f'{box["name"]}: no build receipt tying this executable to {plan["source_sha"]}')
        plan['preflights'] = preflights
        acceptance_cross.check_driver_preflights(plan, preflights)
        acceptance_cross.check_mod_preflights(plan, preflights)
        # The directory default is the coordinator's own committed header; a driver-only plan has no local tree.
        public = acceptance_cross.is_row(plan) and acceptance_cross.public_directory_available(HERE.parent)
        if public:
            pin = ''
        else:
            if acceptance_cross.is_row(plan):
                plan.update(directory_mode='loopback-fallback', public_directory_down=True, own_certificate=True)
                for spec in plan['specs']: spec['directory_mode'] = 'loopback-fallback'
            cert, key, pin = edith_cross.make_cert(root)
            service = directory.start_service(root, local['directory_port'], cert, key)
            for box in boxes.values():
                if box['kind'] == 'windows-local': continue
                handle = (root / f'tunnel-{box["name"]}.log').open('w', encoding='utf-8'); handles.append(handle)
                proc = subprocess.Popen(['ssh', '-N', '-o', 'ExitOnForwardFailure=yes', '-R',
                    f'127.0.0.1:{box["directory_port"]}:127.0.0.1:{local["directory_port"]}', box['ssh']],
                    stdin=subprocess.DEVNULL, stdout=handle, stderr=subprocess.STDOUT,
                    creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                tunnels.append(proc)
            time.sleep(1)
            if any(p.poll() is not None for p in tunnels): raise RuntimeError('directory tunnel failed')
        host_box = next(s['box'] for s in plan['specs'] if s['peer'] == plan['host'])
        # The task payload claims EDITH first and waits for publication if it is a client.
        ordered = sorted(boxes.values(), key=lambda b: 0 if b['kind']=='windows-task' else 1 if b['name']==host_box else 2)
        for box in ordered:
            payload, local_payload, box_root = payloads[box['name']]
            payload['pin'] = pin
            write_json(local_payload, payload)
            if box['kind'] == 'windows-local':
                write_json(root / 'payload.json', payload)
                handle = (root / 'payload.log').open('w', encoding='utf-8'); handles.append(handle)
                processes[box['name']] = subprocess.Popen([sys.executable, str(Path(__file__)), '--payload', str(root / 'payload.json')],
                    stdout=handle, stderr=subprocess.STDOUT, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                launched.add(box['name'])
            else:
                stage_remote(box, local_payload, box_root + '/payload.json')
                if box['kind'] == 'windows-task':
                    state = command(['ssh', box['ssh'], f'(Get-ScheduledTask -TaskName {box["runner"]}).State']).strip()
                    if state != 'Ready': raise RuntimeError(f'{box["name"]}: task is {state}; another payload owns it')
                    script = (HERE / 'edith/cross_session.ps1').read_text(encoding='utf-8')
                    for field, value in dict(PYTHON=box['python'], DRIVER=box['tree'] + '/tools/cross_peers.py', ROOT=box_root).items():
                        script = script.replace('{{' + field + '}}', str(value))
                    local_script = root / f'cross-session-{box["name"]}.ps1'; local_script.write_text(script, encoding='utf-8')
                    if acceptance_cross.is_row(plan):
                        from edith.remote_box import RemoteBox
                        RemoteBox(box['ssh'], task=box['runner'], session_script=box['task_script']).start_task(local_script, budget_s=1)
                    else:
                        stage_remote(box, local_script, box['task_script'])
                        command(['ssh', box['ssh'], f'Start-ScheduledTask -TaskName {box["runner"]}'])
                    launched.add(box['name'])
                    ownership_deadline = time.monotonic()+30
                    while not remote_exists(box, box_root+'/payload-owned.json') and time.monotonic()<ownership_deadline:
                        time.sleep(.25)
                    if not remote_exists(box, box_root+'/payload-owned.json'):
                        raise RuntimeError(f'{box["name"]}: task did not acknowledge this payload; no other engines are launched')
                else:
                    handle = (root / f'payload-{box["name"]}.log').open('w', encoding='utf-8'); handles.append(handle)
                    processes[box['name']] = subprocess.Popen(remote_command(box, [box['python'], box['tree'] + '/tools/cross_peers.py', '--payload', box_root + '/payload.json']),
                        stdin=subprocess.DEVNULL, stdout=handle, stderr=subprocess.STDOUT,
                        creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                    launched.add(box['name'])
        # Every owning payload must finish its guarded capability launch before
        # any match engine starts. Otherwise a remote host can consume its setup
        # deadline while another box's runner waits for the user's fullscreen app.
        release_ready_payloads(boxes,payloads,root,processes,plan['deadlines']['payload_ready_s'])
        deadline,session=time.monotonic()+plan['deadlines']['session_publication_s'],None
        while time.monotonic()<deadline:
            if public:
                session = acceptance_cross.published_session(plan)
                if session: break
            else:
                rows=directory.list_sessions(local['directory_port'])
                if len(rows)==1: session=rows[0]['session_id']; break
            if host_box in processes and processes[host_box].poll() is not None: break
            time.sleep(.5)
        if not session: raise RuntimeError('host published no session before the declared publication deadline')
        write_json(root/'session.json',dict(session=session)); plan['session']=session
        for box in boxes.values():
            if box['kind']!='windows-local': stage_remote(box,root/'session.json',payloads[box['name']][2]+'/session.json')
        acceptance_clock, late_released = {}, False
        deadline, finished = time.monotonic()+max(s['timeout'] for s in plan['specs'])+60, set()
        while time.monotonic() < deadline and len(finished) < len(boxes):
            if not late_released and acceptance_cross.late_join_due(plan, acceptance_clock):
                late = plan['late_join']
                spec = next(s for s in plan['specs'] if s['peer']==late['peer'])
                target = boxes[spec['box']]
                stage_remote(target,root/'session.json',payloads[target['name']][2]+'/session-late.json')
                host_spec = next(s for s in plan['specs'] if s['peer']==plan['host'])
                write_json(root/'late-join-released.json',dict(peer=late['peer'],host_tick=acceptance_cross.latest_tick(Path(host_spec['own'])/'live.jsonl'),host_elapsed_s=time.monotonic()-acceptance_clock['host_started']))
                late_released = True
            if claim and not owns_reservation(local_peer): raise RuntimeError('this run lost its box reservation')
            for box in boxes.values():
                if box['name'] in finished: continue
                done = root / 'done.json' if box['kind'] == 'windows-local' else payloads[box['name']][2] + '/done.json'
                if (Path(done).is_file() if box['kind'] == 'windows-local' else remote_exists(box, done)):
                    require_payload_success(box,read_payload_outcome(box,done))
                    finished.add(box['name'])
            if scratch_bytes(SCRATCH) >= LIMIT: raise RuntimeError('local scratch reached 4 GB; stopped without deletion')
            time.sleep(2)
        if len(finished) != len(boxes): raise RuntimeError('one or more box payloads exceeded the run deadline')
    except Exception as error:
        findings.append(dict(kind='driver', reason=str(error)))
        print(f'RUN FINDING: {error}', flush=True)
    finally:
        for box in boxes.values():
            if box['name'] not in payloads: continue
            try:
                stop = root / 'stop.json'; write_json(stop, dict(reason='coordinator teardown'))
                if box['kind'] != 'windows-local' and not remote_exists(box, payloads[box['name']][2] + '/done.json'):
                    stage_remote(box, stop, payloads[box['name']][2] + '/stop.json')
            except Exception as error: findings.append(dict(kind='cleanup', box=box['name'], reason=str(error)))
        # Let each owning payload close its engine job/hop and seal its records.
        # Killing an SSH parent first can leave the actual far-side engine alive.
        teardown_deadline = time.monotonic() + 45
        pending = set(launched)
        while pending and time.monotonic() < teardown_deadline:
            for name in list(pending):
                box = boxes[name]
                done = root / 'done.json' if box['kind'] == 'windows-local' else payloads[name][2] + '/done.json'
                if (Path(done).is_file() if box['kind'] == 'windows-local' else remote_exists(box, done)):
                    pending.remove(name)
                elif name in processes and processes[name].poll() is not None:
                    pending.remove(name)
            if pending: time.sleep(.25)
        for name in pending:
            findings.append(dict(kind='cleanup', box=name, reason='owning payload did not acknowledge teardown within 45 seconds'))
        for proc in processes.values():
            if proc.poll() is None:
                proc.terminate()
                try: proc.wait(timeout=15)
                except subprocess.TimeoutExpired: proc.kill()
        if claim:
            released=release_reservation(claim)
            plan['reservation']['released']=released
            if not released: findings.append(dict(kind='reservation',reason='reservation token was absent or replaced; no foreign marker removed'))
        for box in boxes.values():
            if box['kind'] == 'windows-local' or box['name'] not in payloads: continue
            try:
                if plan.get('preserve_evidence'):
                    acceptance_cross.fetch_preserved(box, payloads[box['name']][2], root / 'boxes' / box['name'])
                else:
                    fetch_box(box, payloads[box['name']][2], root / 'boxes' / box['name'])
            except Exception as error: findings.append(dict(kind='fetch', box=box['name'], reason=str(error)))
        for proc in tunnels:
            proc.terminate(); proc.wait(timeout=10)
        if service is not None:
            service.terminate(); service.wait(timeout=10)
        for handle in handles: handle.close()
        plan['preflights'] = preflights; plan['driver_findings'] = findings
        write_json(root / 'manifest.json', plan)
    import cross_report
    result = cross_report.build_report(root)
    return 0 if result['passed'] else 1


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--boxes', type=Path, default=HERE / 'cross_peers/boxes.json')
    parser.add_argument('--lane', default=os.environ.get(LANE_ENV), help=f'the lane whose scratch holds the run on every box (or {LANE_ENV}); no default')
    parser.add_argument('--mac-guard', default=os.environ.get(MAC_GUARD_ENV),
                        help=f"the Mac inventory's live marker a Mac launch requires (or {MAC_GUARD_ENV}); no default")
    parser.add_argument('--out', type=Path, help='default: <lane scratch>/dry-run')
    parser.add_argument('--host', default='erol')
    parser.add_argument('--local-setting', action='append', type=setting_pair, default=[], metavar='KEY=VALUE',
                        help='seed windows-local after directory settings; repeatable, last value wins')
    parser.add_argument('--local-render-cap', type=render_cap_hz, metavar='HZ', help='windows-local render cap: 0 or 60')
    parser.add_argument('--host-stall', type=host_stall_spec, metavar='TICK:MS', help="stall the host's simulation MS milliseconds at round tick TICK, every round")
    parser.add_argument('--scenario', choices=['match', 'soak', 'chaos', 'endurance'], default='match')
    parser.add_argument('--acceptance-arm', choices=['L4P'], help='the V1 item 17 sustained four-box impairment arm')
    parser.add_argument('--roster', choices=['three-way', 'four-way', 'allies', 'ai-heavy', 'mixed'], default='three-way')
    parser.add_argument('--scene', default='Grasslands')
    parser.add_argument('--ticks', type=int)
    parser.add_argument('--timeout', type=int, default=900)
    parser.add_argument('--seed', type=int, default=42)
    parser.add_argument('--chaos-seed', type=int, default=260926)
    parser.add_argument('--chaos-faults', type=int, default=8)
    parser.add_argument('--schedule', type=Path)
    parser.add_argument('--barriers', type=Path, help='capture/writer phase schedule with peer, id, phase, tick, round and timeout_ms; each receipt names its release file')
    parser.add_argument('--recovery-deadline-ms', type=int, default=120000)
    parser.add_argument('--capture-budget-ms', type=float, default=1000)
    parser.add_argument('--fullstate-every', type=int, default=600)
    parser.add_argument('--keep-fullstate-sections', default='', help='comma-separated full-state sections every capture keeps as text, all captures retained (a diff run)')
    parser.add_argument('--memory-census-ticks', type=int, default=1800, help="the engine's memory census every this many round ticks (its checkpoint cache is the instrument's own share); 0 = off")
    parser.add_argument('--quiet-window', action='store_true', help='lead-scheduled quiet window; load still suppresses feel gating')
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--selftest-launch-guard',action='store_true',help='exercise simulated QUNS clearance and all-box release without starting an engine')
    parser.add_argument('--payload', type=Path, help=argparse.SUPPRESS)
    parser.add_argument('--preflight', type=Path, help=argparse.SUPPRESS)
    options = parser.parse_args(argv)
    if not (options.payload or options.preflight):
        try: set_lane(options.lane)
        except ValueError as error: parser.error(str(error))
        global MAC_GUARD
        MAC_GUARD = options.mac_guard or None
        if MAC_GUARD is None and '{mac_guard}' in options.boxes.read_text(encoding='utf-8-sig'):
            parser.error(f"{options.boxes} guards the Mac with the live inventory marker: --mac-guard or {MAC_GUARD_ENV}")
        options.out = options.out or SCRATCH / 'dry-run'
    options.ticks = options.ticks or (1201 if options.scenario == 'match' else 72000 if options.scenario == 'soak' else 36000)
    if options.ticks < 2 or min(options.timeout, options.recovery_deadline_ms, options.capture_budget_ms) <= 0:
        parser.error('tick budget and deadlines must be positive')
    if options.fullstate_every < 0: parser.error('fullstate cadence must be nonnegative')
    return options


def main(argv=None):
    arguments = list(sys.argv[1:] if argv is None else argv)
    if acceptance_cross.named_row(arguments):
        return acceptance_cross.main(arguments)
    options = parse_args(argv)
    if options.selftest_launch_guard:
        if not options.out.resolve().is_relative_to(SCRATCH.resolve()): raise ValueError('self-test output must stay inside lane scratch')
        result=launch_guard_selftest(options.out)
        print(json.dumps(result,indent=2)); return 0 if result['passed'] else 1
    if options.payload: return run_payload(options.payload)
    if options.preflight: return preflight_payload(options.preflight)
    plan = make_plan(options)
    if options.dry_run:
        dry_run(plan)
        return 0
    return run_plan(plan, options.out)


if __name__ == '__main__':
    raise SystemExit(main())
