"""Owned world/mod payload hooks composed with the unchanged frozen cross tools."""
from pathlib import Path
import json
import os
import platform
import secrets
import sys
import time

import cross_peers as cross
import world_mod_cross as acceptance_cross
from cross_peers import (render_cap_hz, write_json, configure_posix_box, digest_file,
    assert_box_guard, read_capabilities, wait_for_payload_release, refuse_mixed_build,
    Tail, engine_pid, box_load, sample_memory, owns_reservation, inventory_guard,
    crash_due, termination_receipt, restart_spec, seal_evidence)


def acquire_shared_reservation(box, root, timeout=60):
    if box['kind'] != 'posix-ssh':
        return None
    marker = Path(box['acceptance_marker'])
    expected = Path(box['scratch']).parent/'ACCEPTANCE-STREAM-RUNNING'
    if marker != expected or Path(box['exclusive_marker']) == marker:
        raise ValueError('POSIX shared stream and cross reservation must both be explicit')
    deadline = time.monotonic()+timeout
    while time.monotonic() < deadline:
        if marker.exists() or cross.box_load():
            time.sleep(.5)
            continue
        token = Path(box['scratch']).name+':'+str(os.getpid())+':'+secrets.token_hex(16)
        try:
            marker.mkdir()
        except FileExistsError:
            continue
        # The frozen POSIX guard's published contract is a directory/owner pair.
        # The cross driver's own JSON reservation is acquired separately below it.
        (marker/'owner').write_text(token, encoding='utf-8')
        previous = os.environ.get('CC_ACCEPTANCE_BOX_OWNER')
        os.environ['CC_ACCEPTANCE_BOX_OWNER'] = token
        claim = dict(marker=str(marker), token=token, previous=previous)
        write_json(Path(root)/'shared-reservation.json', dict(marker=str(marker), runner_pid=os.getpid(),
                   owner_sha256=__import__('hashlib').sha256(token.encode()).hexdigest()))
        if cross.box_load():
            release_shared_reservation(claim)
            time.sleep(.5)
            continue
        return claim
    raise TimeoutError('shared POSIX acceptance stream is occupied; no engine launched')


def release_shared_reservation(claim):
    if claim is None:
        return False
    marker = Path(claim['marker'])
    removed = False
    try:
        if (marker/'owner').read_text(encoding='utf-8') == claim['token'] and \
                {path.name for path in marker.iterdir()} == {'owner'}:
            (marker/'owner').unlink()
            marker.rmdir()
            removed = True
    except OSError:
        pass
    if claim['previous'] is None:
        os.environ.pop('CC_ACCEPTANCE_BOX_OWNER', None)
    else:
        os.environ['CC_ACCEPTANCE_BOX_OWNER'] = claim['previous']
    return removed


def retain_checkpoints(run, spec, *, final=False):
    if not acceptance_cross.is_row(spec):
        raise ValueError('owned payload accepts only the declared acceptance rows')


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


def run_payload(path):
    from run_sim_test import make_run
    from feel.records import CaptureSealer, RecoveryLedger, NativeFaultEffects, LobbyWatch
    payload = json.loads(Path(path).read_text(encoding='utf-8'))
    root, box = Path(path).parent, payload['box']
    if box.get('name') == 'Z13' or platform.node().casefold() != box['hostname'].casefold():
        raise ValueError('native payload is on a retired or different machine')
    if box['kind'] == 'windows-local' and box['name'] != 'EROL-PC':
        raise ValueError('only the NOTE 12 EROL-PC host may run locally')
    if not payload['specs'] or any(not acceptance_cross.is_row(spec) or not spec.get('preserve_evidence') for spec in payload['specs']):
        raise ValueError('native payload requires retained acceptance row evidence')
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
