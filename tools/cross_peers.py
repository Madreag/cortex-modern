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

HERE = Path(__file__).resolve().parent
SCRATCH = Path('D:/mx/astra-cross-peers-build-20260926')
LIMIT = 4_000_000_000
MST = dt.timezone(dt.timedelta(hours=-7))


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, allow_nan=False) + '\n', encoding='utf-8')


def command(argv, timeout=60, check=True):
    result = subprocess.run(list(map(str, argv)), capture_output=True, text=True, timeout=timeout,
                            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    if check and result.returncode:
        raise RuntimeError(f'{argv[0]} exit={result.returncode}: {result.stderr[-1000:]}')
    return result.stdout


def quote_ps(value):
    return "'" + str(value).replace("'", "''") + "'"


def load_boxes(path):
    manifest = json.loads(Path(path).read_text(encoding='utf-8-sig'))
    boxes, peers = manifest['boxes'], manifest['instances']
    by_name = {box['name']: box for box in boxes}
    if len(by_name) != len(boxes) or len({p['name'] for p in peers}) != len(peers):
        raise ValueError('box names and instance names must each be unique')
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
        remote_windows = next((p['name'] for p in peers if boxes[p['box']]['kind'] == 'windows-task'), peers[1]['name'])
        posix = next((p['name'] for p in peers if boxes[p['box']]['kind'] == 'posix-ssh'), peers[-1]['name'])
        faults = [dict(tick=7200, peer=remote_windows, action='live-stall', duration_ms=600),
                  dict(tick=14400, peer=posix, action='announced-leave-rejoin'),
                  dict(tick=21600, peer=remote_windows, action='crash-restart'),
                  dict(tick=28800, peer=posix, action='ack-drop'),
                  dict(tick=28800, peer=posix, action='loss', percent=5, duration_ticks=1800)]
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
        if fault['action'] == 'host-kill': raise ValueError('migration with EDITH is NOT COVERED until the endpoint fix')
        incarnation = fault.get('incarnation', incarnations[fault['peer']])
        restarting = fault['action'] in ('announced-leave-rejoin', 'crash-restart')
        fault.update(id=fault.get('id', f'fault-{number}'), incarnation=incarnation,
                     return_incarnation=incarnation+int(restarting),
                     deadline_ms=fault.get('deadline_ms', options.recovery_deadline_ms),
                     outcomes=fault.get('outcomes', ['first_controllable_input', 'match_over_goodbye']))
        if restarting: incarnations[fault['peer']] = incarnation + 1
    return faults


def make_plan(options):
    manifest = load_boxes(options.boxes)
    boxes = {b['name']: b for b in manifest['boxes']}
    peers = manifest['instances']
    hosts = [p for p in peers if p['name'] == options.host or p['box'] == options.host]
    if len(hosts) != 1: raise ValueError('--host must select exactly one engine instance')
    host = hosts[0]['name']
    stem = options.out.name
    if not re.fullmatch(r'[A-Za-z0-9_-]+', stem): raise ValueError('run name must be a simple directory leaf')
    faults = schedule_for(options, peers, boxes)
    if any(f['peer']==host and f['action'] in ('crash-restart','announced-leave-rejoin') for f in faults):
        raise ValueError('host removal requires the WAN migration endpoint fix; the first soak keeps its host alive')
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
                 '-net-match-mode', 'coop-pve' if options.roster == 'ai-heavy' else 'pvpve',
                 '-net-match-cpu-slots', '2' if options.roster == 'ai-heavy' else '1',
                 '-net-match-service-preset', 'Multi Box Combat', '-net-match-service-module', 'UserScenes.rte',
                 '-net-match-service-scene', options.scene, '-net-match-service-scene-module', 'Base.rte',
                 '-net-match-auto-delay', '-net-local-prediction', 'on', '-net-ice', 'on', '-net-player-name', peer['name'],
                 '-net-reconnect-ticket', str(PurePosixPath(root) / peer['name'] / 'participant.ticket'), '-net-match-report', own + '/match-report.json',
                 '-net-cross-schedule', own + '/faults.json', '-net-cross-host-options', own + '/host-options.json']
        if options.fullstate_every:
            flags += ['-net-fullstate-hash-every', str(options.fullstate_every), '-net-fullstate-dump', own + '/fullstate']
        if peer['name'] == host:
            flags += ['-net-host', '-net-replay-out', own + '/match.ccreplay']
            if options.scenario != 'match': flags += ['-net-autosave-seconds', '180']
        else:
            flags += ['-net-join-session', '<published-session-id>']
        if options.scenario != 'match':
            flags += ['-net-cross-rematches', '4096']
        specs.append(dict(peer=peer['name'], box=peer['box'], role='host' if peer['name'] == host else peer.get('seat', 'player'),
            incarnation=0, root=root, own=own, repo=box['tree'], executable=box['executable'], flags=flags,
            env={'CCCP_HEADLESS': '1', 'PYTHONDONTWRITEBYTECODE': '1', 'CC_TEST_CROSS_RECORDS': own + '/events.jsonl',
                 'CC_TEST_CROSS_RUN': stem, 'CC_TEST_CROSS_INSTANCE': peer['name'], 'CC_TEST_CROSS_EXECUTION': 'process-0',
                 'CC_TEST_CROSS_INCARNATION': '0', 'CC_TEST_NET_UI_SCRIPT': own + '/probe.json',
                 'CC_TEST_CROSS_BOT': own + '/bot.json', 'CC_TEST_CROSS_EVENT_RAW_LIMIT': str(64*1024**3)}, timeout=options.timeout, ticks=options.ticks,
            settings={}, roster=options.roster, scene=options.scene,
            initial_skill=100 if boxes[hosts[0]['box']]['kind'] == 'windows-task' else 50,
            faults=[f for f in faults if f['peer'] == peer['name']], barriers=[b for b in barriers if b['peer']==peer['name']]))
        if specs[-1]['barriers']:
            specs[-1]['env']['CC_TEST_CROSS_CAPTURE_BARRIER'] = own+'/barriers.json'
    return dict(version=1, run=stem, started=dt.datetime.now(MST).strftime('%Y-%m-%d %H:%M:%S MST'),
                driver_commit=command(['git','-C',HERE.parent,'rev-parse','HEAD']).strip(),
                driver_tracked_changes=command(['git','-C',HERE.parent,'status','--porcelain','--untracked-files=no']).splitlines(),
                driver_sources={str(path.relative_to(HERE)):digest_file(path) for path in
                    (HERE/'cross_peers.py',HERE/'cross_report.py',HERE/'feel/report.py',HERE/'feel/records.py')},
                boxes=manifest['boxes'], instances=peers, specs=specs, host=host, ticks=options.ticks,
                scenario=options.scenario, roster=options.roster, scene=options.scene, seed=options.seed,
                chaos_seed=options.chaos_seed if options.scenario == 'chaos' else None,
                seed_scope='choices only; transport and OS timing are not reproduced', faults=faults,
                capture_barriers=barriers,
                overlap_policy='ordered by committed unique gameplay budget; early ends carry outstanding triggers',
                deadlines=dict(recovery_ms=options.recovery_deadline_ms, capture_ms=options.capture_budget_ms,
                               launch_s=options.timeout),
                memory=dict(warmup_s=120, slope_bytes_per_minute=8*1024*1024,
                            retained_bytes=128*1024*1024, sample_seconds=60),
                storage=dict(total_bytes=LIMIT, event_bytes_per_instance=256*1024*1024,
                             event_expanded_bytes_per_instance=64*1024**3,
                             live_bytes_per_instance=512*1024*1024, failure_window_ticks=600),
                quiet_window=options.quiet_window, pathfinding='production asynchronous; no -tick-hashes override',
                fullstate_every=options.fullstate_every,
                required_gates=['three_real_boxes', 'matching_content', 'same_commit', 'full_history', 'zero_desync',
                                'zero_unscheduled_holds', 'native_completion', 'bounded_recovery'],
                limitations={'migration': 'NOT COVERED: NetMatchService endpoint publication and NetLockstep direct dialing need the endpoint fix',
                             'team_members': 'NOT COVERED until netcode row 14 lands',
                             'peer_counts': 'NOT COVERED until netcode row 13 lands'})


def dry_run(plan):
    print(json.dumps(plan, indent=2))
    local = next(box for box in plan['boxes'] if box['kind'] == 'windows-local')
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
    with newest.open(encoding='utf-8', errors='replace') as stream:
        last = next(reversed([line.strip() for line in stream if re.search(r'\] S\d+ (start|exit)', line)]), '')
    return f'{newest}: {last}' if re.search(r'\] S3 start\s*$', last) else None


def scratch_bytes(root):
    total = 0
    for directory, names, files in os.walk(root, followlinks=False):
        names[:] = [n for n in names if not Path(directory, n).is_symlink()
                    and not getattr(Path(directory, n), 'is_junction', lambda: False)()]
        for name in files:
            path = Path(directory, name)
            if not path.is_symlink(): total += path.stat().st_size
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


def assert_box_guard(box):
    if box.get('guard_file') and not Path(box['guard_file']).is_file():
        raise RuntimeError(f'Mac inventory guard active: {box["guard_file"]} absent')
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
    stamp_path = repo / 'tools/cross_peers/build.json'
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
    write_json(Path(path).parent / 'preflight.json', result)
    print(f'preflight {box["name"]} files={len(content)} exe={result["executable_sha256"][:16]}')
    return 0


def stage_combat(run, spec):
    from feel_measure import private_settings
    private_settings(run, 60)
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
    from run_sim_test import make_run, seed_settings
    own = Path(spec['own']); own.mkdir(parents=True, exist_ok=False)
    rows = []
    for start in range(1, spec['ticks'] + 1, 240):
        end = min(start + 179, spec['ticks'])
        direction = 'L_RIGHT' if (start // 240) % 2 == 0 else 'L_LEFT'
        rows += [f'player=0 {start} {end} {direction} FIRE AIM=0.9,-0.1',
                 f'player=0 {start+180} {min(start+210, spec["ticks"])} WEAPON_RELOAD'] if start+210 <= spec['ticks'] else [f'player=0 {start} {end} FIRE AIM=-0.9,-0.1']
    (own / 'input.txt').write_text('\n'.join(rows) + '\n', encoding='utf-8')
    write_json(own / 'faults.json', [f for f in spec['faults'] if f['incarnation'] == spec['incarnation']])
    write_json(own / 'barriers.json',spec.get('barriers',[]))
    teams = dict(human_teams=[0,0,1], cpu_teams=[2]) if spec['roster'] in ('mixed','allies') else \
            dict(human_teams=[0,0,0], cpu_teams=[1,2]) if spec['roster'] == 'ai-heavy' else {}
    write_json(own / 'host-options.json', [dict(difficulty=spec['initial_skill'], ai_skill=spec['initial_skill'], fog=False,
                                              scene=spec['scene'], scene_module='Base.rte', **teams),
                                          dict(difficulty=100, ai_skill=100, fog=True, scene='Ketanot Hills', scene_module='Base.rte', **teams)])
    write_json(own / 'bot.json', [dict(round=0, **{'from': 3601, 'to': spec['ticks']})] if spec['ticks'] >= 3601 else [])
    write_json(own / 'probe.json', dict(schema=1, timeout_ms=120000, activate_at_tick=1, activate_phase='Running', repeat_rounds=True, steps=[
        dict(op='assert_window', equals=dict(width=960, height=540)),
        dict(op='assert_buy', input_player=0), dict(op='assert_pie', input_player=0), dict(op='finish')]))
    if box['kind'] == 'posix-ssh':
        os.environ['CCCP_TEST_BINARY'] = spec['executable']
        os.environ['CCCP_POSIX_HOP'] = 'ssh' if sys.platform=='darwin' else 'off'
    settings = {'SessionDirectoryUrl': f'127.0.0.1:{box["directory_port"]}', 'SessionDirectoryCertSha256': pin,
                'SessionDirectoryInstallKey': f'cross-{spec["peer"]}-install', 'NetworkIceEnable': '1',
                'NetworkConnectionMode': 'DirectOnly', 'NetworkHostRelayMode': 'Off'}
    run = make_run(Path(spec['repo']), spec['flags'], own / 'engine', timeout=spec['timeout'], env=spec['env'], runtime=runtime)
    if runtime is None:
        stage_combat(run, spec)
    else:
        (own / 'engine/feel').mkdir(exist_ok=True)
    seed_settings(run, settings)
    return run


class Tail:
    def __init__(self, path):
        self.path, self.offset, self.pending, self.part = Path(path), 0, b'', 0

    def read(self):
        path = self.path if not self.part else Path(str(self.path) + f'.part{self.part}')
        if not path.is_file(): return []
        with path.open('rb') as stream:
            stream.seek(self.offset); block = stream.read(4*1024*1024); self.offset += len(block)
        pieces = (self.pending + block).split(b'\n'); self.pending = pieces.pop()
        rows = [json.loads(line) for line in pieces if line.strip()]
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


def read_capabilities(box, root):
    from run_sim_test import make_run
    assert_box_guard(box)
    if box['kind'] == 'posix-ssh':
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


def run_payload(path):
    from run_sim_test import make_run
    payload = json.loads(Path(path).read_text(encoding='utf-8'))
    root, box = Path(path).parent, payload['box']
    runs, started, completed, readers, progress, fired = {}, {}, set(), {}, {}, set()
    specifications = {s['peer']: s for s in payload['specs']}
    next_sample, verdict = 0, 0
    try:
        assert_box_guard(box)
        write_json(root / 'payload-owned.json', dict(box=box['name'], runner_pid=os.getpid()))
        if box['kind'] == 'windows-local' and len(payload['specs']) != 1:
            raise RuntimeError('this lane permits only one local engine at a time; dry-run supports larger manifests')
        capabilities = read_capabilities(box, root)
        requested = payload['specs'][0]['flags']
        requested = int(requested[requested.index('-net-match-peers') + 1])
        if requested > capabilities['peer_limit']:
            raise RuntimeError(f'admission refused by build capability: requested {requested} peers, limit {capabilities["peer_limit"]}; Main clamps its legacy argument, so the driver refuses to silently shrink the roster')
        for spec in payload['specs']:
            if spec['role'] != 'host':
                session_path = root / 'session.json'
                deadline = time.monotonic() + 180
                while not session_path.is_file() and time.monotonic() < deadline: time.sleep(.2)
                if not session_path.is_file(): raise RuntimeError('session directory publication deadline')
                session = json.loads(session_path.read_text(encoding='utf-8'))['session']
                spec['flags'] = [session if flag == '<published-session-id>' else flag for flag in spec['flags']]
            assert_box_guard(box)
            run = prepare_instance(spec, payload['pin'], box)
            runs[spec['peer']] = run
            run.start(); started[spec['peer']] = time.monotonic()
            readers[spec['peer']] = Tail(Path(spec['own']) / 'events.jsonl')
            progress[spec['peer']] = {}
            write_json(Path(spec['own']) / 'instance.json', spec)
            write_json(Path(spec['own']) / 'started.json', dict(peer=spec['peer'], incarnation=0, engine_pid=engine_pid(run)))
        with (root / 'samples.jsonl').open('w', encoding='utf-8') as samples:
            while len(completed) < len(runs):
                now = time.monotonic()
                if (root / 'stop.json').is_file(): raise RuntimeError('coordinator cancelled this box payload')
                if box['kind'] == 'windows-local' and (reason := inventory_guard()): raise RuntimeError(reason)
                if now >= next_sample:
                    own_pids = [engine_pid(r) for r in runs.values()]
                    load = box_load(own_pids)
                    all_sampled = True
                    for spec in specifications.values():
                        peer = spec['peer']; run = runs[peer]
                        if peer in completed: continue
                        measured = sample_memory(run)
                        all_sampled &= bool(measured and engine_pid(run))
                        row = dict(peer=peer, incarnation=spec['incarnation'], execution=f'process-{spec["incarnation"]}', engine_pid=engine_pid(run),
                                   elapsed_s=now-started[peer], load=load, same_box_instances=len(runs),
                                   **(measured or {}))
                        samples.write(json.dumps(row) + '\n'); samples.flush()
                        retain_checkpoints(run, spec)
                    next_sample = now + (60 if all_sampled else 1)
                    assert_box_guard({**box, 'kind': 'windows-task'} if box['kind'] == 'windows-local' else box)
                for spec in list(specifications.values()):
                    peer = spec['peer']; run = runs[peer]
                    if peer in completed: continue
                    for observed in readers[peer].read():
                        if observed.get('type') == 'progress': progress[peer] = observed
                    readers[peer].compress_consumed()
                    current = progress[peer]
                    due = next((f for f in spec['faults'] if f['action'] == 'crash-restart' and f['incarnation'] == spec['incarnation'] and f['id'] not in fired
                                and current.get('budget_tick', 0) >= f['tick']), None)
                    leaving = next((f for f in spec['faults'] if f['action'] == 'announced-leave-rejoin' and f['incarnation'] == spec['incarnation'] and f['id'] not in fired
                                    and current.get('budget_tick', 0) >= f['tick']), None)
                    if due or (leaving and run.poll() is not None):
                        fault = due or leaving; fired.add(fault['id'])
                        receipt = dict(type='lifecycle', id=fault['id'], peer=peer, incarnation=spec['incarnation'],
                                       action=fault['action'], requested_tick=fault['tick'], actual=current,
                                       observed_wall_ms=time.monotonic()*1000, engine_pid=engine_pid(run))
                        with (root / 'lifecycle.jsonl').open('a', encoding='utf-8') as lifecycle:
                            lifecycle.write(json.dumps(receipt)+'\n')
                        retained = Path(run.cwd)
                        if due and run.poll() is None: run.terminate(reason=f'scheduled crash {fault["id"]}')
                        record = run.finish(); run.close()
                        write_json(Path(spec['own']) / 'record.json', record)
                        retain_checkpoints(run, spec, final=True)
                        seal_evidence(spec['own'])
                        new_spec = restart_spec(spec, current)
                        render = new_spec['flags'].index('-feel-render-settings') + 1
                        new_spec['flags'][render] = str(retained / 'Userdata/FeelRender.ini')
                        assert_box_guard(box)
                        fresh = prepare_instance(new_spec, payload['pin'], box, runtime=retained)
                        runs[peer] = fresh; specifications[peer] = new_spec
                        readers[peer] = Tail(Path(new_spec['own']) / 'events.jsonl')
                        fresh.start(); started[peer] = time.monotonic()
                        write_json(Path(new_spec['own']) / 'instance.json', new_spec)
                        write_json(Path(new_spec['own']) / 'started.json', dict(peer=peer, incarnation=new_spec['incarnation'], engine_pid=engine_pid(fresh)))
                        continue
                    if run.poll() is not None or now - started[peer] > spec['timeout']:
                        record = run.finish(); run.close(); completed.add(peer)
                        write_json(Path(spec['own']) / 'record.json', record)
                        if record.get('exit_code') != 0 or record.get('timed_out'): verdict = 1
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
                seal_evidence(specifications[peer]['own'])
            except Exception as error:
                verdict = 1
                write_json(root / 'seal-error.json', dict(peer=peer, error=str(error)))
        write_json(root / 'done.json', dict(exit_code=verdict, completed=sorted(completed)))
    return verdict


def remote_command(box, args):
    if box['kind'] == 'windows-task':
        return ['ssh', box['ssh'], '& ' + ' '.join(quote_ps(a) for a in args)]
    return ['ssh', box['ssh'], shlex.join(list(map(str, args)))]


def stage_remote(box, local, remote):
    command(['scp', '-q', str(local), f'{box["ssh"]}:{remote}'], timeout=120)


def remote_exists(box, path):
    script = f'if (Test-Path -LiteralPath {quote_ps(path)}) {{ "yes" }}' if box['kind'] == 'windows-task' else f'test -f {shlex.quote(path)} && printf yes'
    return command(['ssh', box['ssh'], script], check=False).strip() == 'yes'


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
    locals_ = [box for box in boxes.values() if box['kind'] == 'windows-local']
    if len(locals_) != 1: raise ValueError('one coordinator box must be windows-local')
    local = locals_[0]
    service, processes, tunnels, handles, preflights, payloads = None, {}, [], [], {}, {}
    findings, launched = [], set()
    try:
        for box in boxes.values():
            box_root = str(PurePosixPath(box['scratch']) / plan['run'])
            payload = dict(box=box, specs=[s for s in plan['specs'] if s['box'] == box['name']], pin='pending')
            local_box = root / 'boxes' / box['name']; local_box.mkdir(parents=True)
            local_payload = local_box / 'payload.json'; write_json(local_payload, payload)
            if box['kind'] == 'windows-local':
                write_json(root / 'payload.json', payload)
                preflight_payload(root / 'payload.json')
                preflights[box['name']] = json.loads((root / 'preflight.json').read_text())
            else:
                if box.get('guard_file') and not remote_exists(box, box['guard_file']):
                    raise RuntimeError(f'{box["name"]}: inventory guard active: {box["guard_file"]} absent')
                mkdir = f'New-Item -ItemType Directory -Path {quote_ps(box_root)} | Out-Null' if box['kind'] == 'windows-task' else f'mkdir {shlex.quote(box_root)}'
                command(['ssh', box['ssh'], mkdir])
                stage_remote(box, local_payload, box_root + '/payload.json')
                command(remote_command(box, [box['python'], box['tree'] + '/tools/cross_peers.py', '--preflight', box_root + '/payload.json']), timeout=180)
                command(['scp', '-q', f'{box["ssh"]}:{box_root}/preflight.json', str(local_box / 'preflight.json')], timeout=120)
                preflights[box['name']] = json.loads((local_box / 'preflight.json').read_text())
            payloads[box['name']] = (payload, local_payload, box_root)
        reference = preflights[local['name']]
        if len({p['machine_id'] for p in preflights.values()}) < 3:
            raise RuntimeError('the manifest resolves to fewer than three real machines')
        for box in boxes.values():
            value = preflights[box['name']]
            if any(value[k] != reference[k] for k in ('content', 'modules', 'fixture')):
                raise RuntimeError(f'{box["name"]}: content/module/fixture manifests differ before launch')
            if box['kind'].startswith('windows') and value['executable_sha256'] != reference['executable_sha256']:
                raise RuntimeError(f'{box["name"]}: Windows executable hash differs')
            if value['build'].get('commit') != reference['head'] or value['build'].get('executable_sha256') != value['executable_sha256']:
                raise RuntimeError(f'{box["name"]}: no build receipt tying this executable to {reference["head"]}')
        plan['preflights'] = preflights
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
            if box['name'] == host_box:
                deadline, session = time.monotonic()+180, None
                while time.monotonic() < deadline:
                    rows = directory.list_sessions(local['directory_port'])
                    if len(rows) == 1:
                        session = rows[0]['session_id']; break
                    if box['name'] in processes and processes[box['name']].poll() is not None: break
                    time.sleep(.5)
                if not session: raise RuntimeError('host published no session before the 180-second deadline')
                write_json(root / 'session.json', dict(session=session))
                plan['session'] = session
                for remote in boxes.values():
                    if remote['kind'] != 'windows-local':
                        stage_remote(remote, root / 'session.json', payloads[remote['name']][2] + '/session.json')
        deadline, finished = time.monotonic()+max(s['timeout'] for s in plan['specs'])+60, set()
        while time.monotonic() < deadline and len(finished) < len(boxes):
            for box in boxes.values():
                if box['name'] in finished: continue
                done = root / 'done.json' if box['kind'] == 'windows-local' else payloads[box['name']][2] + '/done.json'
                if (Path(done).is_file() if box['kind'] == 'windows-local' else remote_exists(box, done)):
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
        for box in boxes.values():
            if box['kind'] == 'windows-local' or box['name'] not in payloads: continue
            try: fetch_box(box, payloads[box['name']][2], root / 'boxes' / box['name'])
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
    parser.add_argument('--out', type=Path, default=SCRATCH / 'dry-run')
    parser.add_argument('--host', default='erol')
    parser.add_argument('--scenario', choices=['match', 'soak', 'chaos', 'endurance'], default='match')
    parser.add_argument('--roster', choices=['three-way', 'allies', 'ai-heavy', 'mixed'], default='three-way')
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
    parser.add_argument('--quiet-window', action='store_true', help='lead-scheduled quiet window; load still suppresses feel gating')
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--payload', type=Path, help=argparse.SUPPRESS)
    parser.add_argument('--preflight', type=Path, help=argparse.SUPPRESS)
    options = parser.parse_args(argv)
    options.ticks = options.ticks or (1201 if options.scenario == 'match' else 36000)
    if options.ticks < 2 or min(options.timeout, options.recovery_deadline_ms, options.capture_budget_ms) <= 0:
        parser.error('tick budget and deadlines must be positive')
    if options.fullstate_every < 0: parser.error('fullstate cadence must be nonnegative')
    return options


def main(argv=None):
    options = parse_args(argv)
    if options.payload: return run_payload(options.payload)
    if options.preflight: return preflight_payload(options.preflight)
    plan = make_plan(options)
    if options.dry_run:
        dry_run(plan)
        return 0
    return run_plan(plan, options.out)


if __name__ == '__main__':
    raise SystemExit(main())
