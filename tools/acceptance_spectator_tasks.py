"""Run the NOTE 12 spectator placement through native tasks and the POSIX runner."""
from __future__ import annotations

import argparse
from contextlib import ExitStack
from copy import deepcopy
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import time

import acceptance_remote_tasks as remote
from acceptance_mod import sha256
from acceptance_runtime import storage_scope, write_json, write_text
from acceptance_spectator import PEERS, SEATED, collect
from acceptance_clock_brackets import PLACEMENT
from acceptance_frozen_tools import helper_archive, receipt as frozen_receipt
from acceptance_native_runtime import acquire_shared_reservation, release_shared_reservation
from acceptance_box_lease import borrow
import cross_peers as cross
from edith.remote_box import RemoteBox, render_payload
import world_mod_cross as world

ROLES = {name: tuple(peer for peer in PEERS if PLACEMENT[peer] == name) for name in ('EROL-PC','EDITH','Linux')}


def validate(box, lane, peers, native=False):
    if box.get('name') not in ROLES:
        raise ValueError('NOTE 12 spectator row uses EROL-PC, EDITH and Linux')
    remote.validate_profile({**box, 'peers_per_box':1}, lane, 'world-join')
    expected = ROLES[box['name']]
    if tuple(peers) != expected or box.get('peers_per_box') != len(expected):
        raise ValueError('spectator roster exceeds or differs from the NOTE 12 placement')
    if native and platform.node().casefold() != box['hostname'].casefold():
        raise ValueError('spectator payload is on the wrong physical machine')


def plan_for(options, profiles):
    by_name = {value['name']:deepcopy(value) for value in profiles if value['name'] in ROLES}
    if set(by_name) != set(ROLES): raise ValueError('the three NOTE 12 row trees are required')
    frozen = frozen_receipt(Path(__file__).parent.parent)
    if frozen is None: raise ValueError('R1 requires the frozen NOTE 11 tool bundle')
    for name, box in by_name.items():
        box['peers_per_box'] = len(ROLES[name])
        box['helpers'] = box['scratch']+'/'+options.out.name+'-helpers'
        if box['kind'] == 'windows-local':
            box['helpers'] = Path(__file__).resolve().parent.parent.as_posix()
            box['payload_root'] = (options.out/'boxes'/box['name']).as_posix()
        if box['kind'] == 'posix-ssh':
            box['acceptance_marker'] = str(Path(box['scratch']).parent/'ACCEPTANCE-STREAM-RUNNING').replace('\\','/')
            box['exclusive_marker'] = box['scratch']+'/FEEL-MATRIX-RUNNING'
        validate(box, options.lane, ROLES[name])
    return dict(run=options.out.name, lane=options.lane, source_sha=options.source_sha,
                boxes=list(by_name.values()), peers=list(PEERS), coordinator_engine_instances=2,
                placement=PLACEMENT,
                driver_sha256=sha256(Path(__file__)), driver_commit=frozen['coordinator_commit'],
                frozen_tools=dict(commit=frozen['frozen_commit'],export=frozen['frozen_export'],frozen_files_modified=0),
                configuration=dict(persistent_world=True, dedicated_host=True, humans=3, cpu_slots=0, world_max_spectators=1),
                throttle=dict(peer='spectator', microseconds=500000, from_tick=6000, until_tick=6060))


def native_preflight(path):
    payload = json.loads(Path(path).read_text())
    box, plan = payload['box'], payload['plan']
    validate(box, plan['lane'], payload['peers'], native=True)
    with borrow(box, Path(path).parent) as held:
        cross.preflight_payload(path)
    destination = Path(path).parent/'preflight.json'
    value = json.loads(destination.read_text())
    build = json.loads(Path(box['build_receipt']).read_text(encoding='utf-8-sig'))
    expected_configuration = 'Final' if box['kind'].startswith('windows-') else 'release'
    if build.get('configuration') != expected_configuration or build.get('build_exit_code') != 0 or \
            build.get('commit') != plan['source_sha'] or build.get('executable_sha256') != value['executable_sha256'] or \
            sha256(Path(build['build_log'])) != build.get('build_log_sha256'):
        raise ValueError('native runtime differs from the compiled engineer tip')
    world.preflight_driver(box, value)
    frozen = frozen_receipt(Path(__file__).parent.parent)
    value.update(build=build, spectator_driver_sha256=sha256(Path(__file__)),
                 frozen_tools=dict(commit=frozen['frozen_commit'],frozen_files_modified=0),
                 preflight_reservation=dict(borrowed=held))
    write_json(destination, value)
    return 0


def check_preflights(plan):
    values = plan.get('preflights', {})
    if set(values) != set(ROLES): raise ValueError('native preflights missing')
    cross.require_distinct_machines(values)
    reference = values['EROL-PC']
    for box in plan['boxes']:
        value = values[box['name']]
        if value.get('load') or value.get('hostname','').casefold() != box['hostname'].casefold():
            raise ValueError('spectator box is occupied or has the wrong identity')
        if value['build'].get('commit') != plan['source_sha'] or value['build'].get('executable_sha256') != value['executable_sha256']:
            raise ValueError('spectator build identity differs')
        if any(value[key] != reference[key] for key in ('content','modules','fixture','acceptance_driver_sources')):
            raise ValueError('complete content or driver bytes differ')
        if box['kind'].startswith('windows-') and value['executable_sha256'] != reference['executable_sha256']:
            raise ValueError('Windows runtime bytes differ')
        if value.get('frozen_tools') != dict(commit=plan['frozen_tools']['commit'],frozen_files_modified=0):
            raise ValueError('frozen native tool identity differs')
        if value.get('spectator_driver_sha256') != plan['driver_sha256']:
            raise ValueError('spectator driver bytes differ')
        if plan.get('reservation_holders') and value.get('preflight_reservation',{}).get('borrowed') is not True:
            raise ValueError('spectator preflight lacks its physical reservation')


def staged_probe(root, peer):
    directory = root/(peer+'-stage')/'probe'; directory.mkdir(parents=True)
    leaf = 'host' if peer=='host' else 'leave' if peer=='seated-one' else 'join' if peer=='spectator' else None
    if leaf:
        raw = (Path(__file__).parent/'e2e'/f'world-spectator.{leaf}.probe.json').read_text()
        for name in PEERS: raw = raw.replace('{PROBE_DIR_'+name+'}', (root/(name+'-stage')/'probe').as_posix())
        probe = json.loads(raw)
    else:
        probe = dict(schema=1, timeout_ms=180000, activate_phase='Running', steps=[
            dict(op='menu', command='dump_world_ownership initial-seat'),
            dict(op='wait_file', path=(root/'host-stage/probe/done.json').as_posix()), dict(op='finish')])
    if peer in SEATED:
        probe['steps'].insert(0, dict(op='wait',sim_at_least=2))
        initial = next(index for index,step in enumerate(probe['steps']) if step.get('command') == 'dump_world_ownership initial-seat')
        probe['steps'][initial+1:initial+1] = [
            dict(op='wait_file',path=(root/'timing-start.json').as_posix()),
            dict(op='menu',command='dump_world_ownership crawl-start')]
        if peer == 'seated-one':
            departing = next(index for index,step in enumerate(probe['steps']) if step.get('command') == 'dump_world_ownership departing-seat')
            probe['steps'][departing:departing] = [
                dict(op='menu',command='dump_world_ownership crawl-end'),
                dict(op='wait_file',path=(root/'timing-complete.json').as_posix())]
        else:
            ending = next(index for index,step in enumerate(probe['steps']) if step.get('path','').endswith('host-stage/probe/done.json'))
            probe['steps'][ending:ending] = [
                dict(op='wait_file',path=(root/'spectator-stage/probe/crawl-complete.json').as_posix()),
                dict(op='menu',command='dump_world_ownership crawl-end')]
    if peer == 'spectator':
        probe['steps'][1:1] = [dict(op='signal',name='spectator-ready'),
            dict(op='wait_file',path=(root/'timing-begun.json').as_posix()),
            dict(op='menu',command='dump_world_ownership crawl-armed')]
    if not 0 < probe['timeout_ms'] <= 180000: raise ValueError('probe exceeds the native deadline limit')
    write_json(directory/'probe.json', probe)
    return directory/'probe.json'


def prepare_peer(box, plan, root, peer, session):
    from run_sim_test import make_run, seed_settings
    from feel_measure import private_settings
    probe = staged_probe(root, peer)
    stage = probe.parent.parent
    write_text(stage/'input.txt', '\n'.join(
        f'player=0 {tick} {tick+119} '+('L_LEFT' if (tick//240)%2 else 'L_RIGHT')+' FIRE AIM=0.9,-0.1'
        for tick in range(1,18000,240))+'\n')
    port = box['ports'][0]+5*ROLES[box['name']].index(peer)
    if port+4 > box['ports'][-1]: raise ValueError('native role exceeds its assigned port block')
    flags = ['-seed','42','-max-ticks','18001','-net-match-service-e2e','-net-port',str(port),
             '-net-match-ticks','18000','-net-match-peers','4','-net-match-humans','3','-net-match-cpu-slots','0',
             '-net-match-mode','pvp-skirmish','-net-match-service-preset','Persistent World',
             '-net-match-service-module','Base.rte','-net-match-service-scene','Grasslands','-net-match-service-scene-module','Base.rte',
             '-net-match-auto-delay','-net-local-prediction','on','-net-ice','on','-net-player-name',peer,
             '-net-live-tick-hashes',str(root/(peer+'-live.jsonl')),'-net-fullstate-hash-every','60',
             '-net-match-report',str(root/(peer+'-match-report.json')),'-input-script',str(stage/'input.txt'),
             '-net-reconnect-ticket',str(stage/'participant.ticket'),'-net-autosave-seconds','0']
    flags += ['-net-host','-net-persistent-world','-net-world-fresh'] if peer=='host' else ['-net-join-session',session]
    env = dict(CCCP_HEADLESS='1', CC_RUNNER_IGNORE_FULLSCREEN='1', CC_TEST_NET_UI_SCRIPT=str(probe),
               CC_TEST_CROSS_RUN=plan['run'], CC_TEST_CROSS_INSTANCE=peer, CC_TEST_CROSS_EXECUTION='process-0',
               CC_TEST_CROSS_INCARNATION='0', CC_TEST_CROSS_RECORDS=str(root/(peer+'-events.jsonl')),
               CC_TEST_CROSS_EVENT_RAW_LIMIT=str(256*1024**2), CC_RUNNER_BOX_NAME=box['name'])
    env.update(box.get('environment',{}))
    if 'launch_floor_gib' in box: env['CC_RUNNER_MIN_FREE_GB']=str(box['launch_floor_gib'])
    if peer=='host': env['CC_TEST_WORLD_MAX_SPECTATORS']='1'
    if peer=='spectator': env.update(CCCP_TEST_SIM_COST_US='500000',CCCP_TEST_SIM_COST_FROM_TICK='6000',CCCP_TEST_SIM_COST_UNTIL_TICK='6060')
    if box['kind'] == 'posix-ssh':
        cross.configure_posix_box(box)
        os.environ.update(CCCP_TEST_BINARY=box['executable'],CCCP_POSIX_HOP='ssh' if sys.platform=='darwin' else 'off')
    run = make_run(Path(box['tree']), flags, root/peer, timeout=600, env=env)
    private_settings(run,60)
    seed_settings(run, dict(SessionDirectoryUrl=world.builtin_directory(box['tree']), SessionDirectoryCertSha256='',
                   SessionDirectoryInstallKey=plan['run']+'-'+peer, NetworkPortMapEnable='0', NetworkIceEnable='1',
                   NetworkConnectionMode='DirectOnly', NetworkHostRelayMode='Off', NetworkDisplayName=peer))
    render = Path(run.cwd)/'Userdata/FeelRender.ini'; write_text(render,'RenderCapHz = 60\n')
    run.argv += ['-feel-render-settings',str(render)]
    return run


def run_payload(path, payload=None):
    payload = json.loads(Path(path).read_text()) if payload is None else payload
    box, plan, root = payload['box'], payload['plan'], Path(path).parent
    validate(box, plan['lane'], payload['peers'], native=True)
    from feel.launch_budget import install_memory_guard
    install_memory_guard()
    settings = dict(CC_RUNNER_BOX_NAME=box['name'])
    if 'launch_floor_gib' in box: settings['CC_RUNNER_MIN_FREE_GB']=str(box['launch_floor_gib'])
    if box['kind'] == 'windows-local':
        settings.update(CC_RUNNER_AFFINITY_MASK=box['affinity_mask'],
                        CC_RUNNER_JOB_MEMORY_GB=str(box['engine_memory_gb']),
                        CCCP_HEADLESS='1', CC_RUNNER_IGNORE_FULLSCREEN='1')
    previous = {key:os.environ.get(key) for key in settings}
    os.environ.update(settings)
    claim, shared_claim, runs, complete, errors = None, None, {}, {}, []
    lease_context = ExitStack()
    try:
        frozen_receipt(Path(__file__).parent.parent)
        borrowed = lease_context.enter_context(borrow(box, root))
        if not borrowed: shared_claim = acquire_shared_reservation(box, root, 60)
        claim = cross.acquire_reservation(box, root, 60)
        write_json(root/'reservation.json', {key:value for key,value in claim['record'].items() if key!='token'})
        caps = cross.read_capabilities(box, root)
        if caps['peer_limit'] < 4: raise RuntimeError('native build refuses the four-seat protocol capacity')
        write_json(root/'launch-ready.json',dict(box=box['name'],capabilities=caps))
        cross.wait_for_payload_release(root,360)
        deadline, session, next_status = time.monotonic()+640, None, 0
        def guard_owned():
            own_pids = [cross.engine_pid(run) for peer,run in runs.items() if peer not in complete]
            if cross.box_load(own_pids):
                raise RuntimeError('another native workload appeared during the spectator row')
            # The frozen local guard intentionally rejects any existing engine
            # at this executable. For the second declared peer, retain every
            # other guard after explicitly accounting for our owned processes.
            if box['kind'] == 'windows-local' and own_pids:
                if not cross.owns_reservation(box): raise RuntimeError('physical reservation was lost')
                if reason := cross.inventory_guard(): raise RuntimeError(reason)
                cross.assert_box_guard({**box, 'kind':'windows-task'})
            else:
                cross.assert_box_guard(box)
        def start(peer):
            guard_owned()
            if len(runs) >= box['peers_per_box']:
                raise RuntimeError('declared native process capacity exhausted')
            run = prepare_peer(box,plan,root,peer,session)
            runs[peer]=run
            guard_owned()
            run.start()
            if sha256(Path(run.argv[0])) != plan['preflights'][box['name']]['executable_sha256']:
                raise RuntimeError('native executable changed during launch')
        if 'host' in payload['peers']: start('host')
        while len(complete) < len(payload['peers']):
            if time.monotonic()>deadline: raise TimeoutError('spectator native payload deadline')
            if (root/'stop.json').is_file(): raise RuntimeError('coordinator cancelled the spectator row')
            if not cross.owns_reservation(box): raise RuntimeError('physical reservation was lost')
            session_file=root/'session.json'
            if session is None and session_file.is_file(): session=json.loads(session_file.read_text())['session']
            if session is not None:
                if box['name']=='EROL-PC' and 'seated-one' not in runs: start('seated-one')
                initial=root/'seated-one-stage/probe/initial-seat.ownership.json'
                if box['name']=='EDITH' and 'seated-two' not in runs and initial.is_file():
                    if json.loads(initial.read_text())['ownership'].get('seat') != 1:
                        raise RuntimeError('first native seated participant did not own stable seat 1')
                    start('seated-two'); start('seated-three')
                if box['name']=='Linux' and 'spectator' not in runs and (root/'watcher-go.json').is_file(): start('spectator')
            if time.monotonic()>=next_status:
                status=dict(box=box['name'],started=list(runs),completed=list(complete),
                            live_ticks={peer:world.latest_tick(root/(peer+'-live.jsonl')) for peer in runs})
                if 'host' in runs:
                    log=root/'host/stdout.log'
                    text=log.read_text(encoding='utf-8',errors='replace') if log.is_file() else ''
                    sessions=re.findall(r'(?m)^\[net-directory\] registered session_id=(\S+) heartbeat_s=\d+',text)
                    if sessions: status['session']=sessions[-1]
                write_json(root/'control.json',status)
                guard_owned()
                next_status=time.monotonic()+2
            for peer,run in runs.items():
                if peer not in complete and run.poll() is not None:
                    complete[peer]=run.finish()
                    write_json(root/peer/'result.json',complete[peer])
                    if complete[peer].get('exit_code') != 0 or complete[peer].get('timed_out'):
                        raise RuntimeError(peer+': native runner did not complete successfully')
            time.sleep(.2)
    except Exception as error:
        errors.append(str(error))
    finally:
        for peer,run in runs.items():
            if peer not in complete:
                if run.process:
                    run.close()
                write_json(root/peer/'result.json',run.record)
        if claim: write_json(root/'reservation-released.json',dict(released=cross.release_reservation(claim),borrowed=bool(claim.get('borrowed'))))
        if shared_claim: write_json(root/'shared-reservation-released.json',dict(released=release_shared_reservation(shared_claim)))
        lease_context.close()
        for key,value in previous.items():
            if value is None: os.environ.pop(key,None)
            else: os.environ[key]=value
        write_json(root/'done.json',dict(box=box['name'],passed=not errors and len(complete)==len(payload['peers']),errors=errors))
    return int(bool(errors))


def stage(plan, root):
    archive=helper_archive(Path(__file__).parent.parent,root)
    for box in plan['boxes']:
        remote_root=remote.native_root(plan,box)
        if box['kind'] != 'windows-local':
            remote.remote_python(box,'from pathlib import Path; import sys; Path(sys.argv[1]).mkdir(exist_ok=False); Path(sys.argv[2]).mkdir(exist_ok=False)',remote_root,box['helpers'])
            cross.command(['scp','-q',str(archive),box['ssh']+':'+box['helpers']+'/helpers.tar'],timeout=180)
            cross.command(remote.native_command(box,['tar.exe' if box['kind']=='windows-task' else 'tar','-xf',box['helpers']+'/helpers.tar','-C',box['helpers']]),timeout=180)
        own=root/'boxes'/box['name']; own.mkdir(parents=True)
        peers=ROLES[box['name']]
        write_json(own/'payload.json',dict(box=box,plan=plan,peers=peers,specs=[dict(acceptance_row='spectator')]))
        remote.publish_new(box,own/'payload.json',remote_root+'/payload.json')
    leases={}
    try:
        remote.start_leases(plan,root,leases)
        for box in plan['boxes']:
            remote_root=remote.native_root(plan,box)
            own=root/'boxes'/box['name']
            cross.command(remote.native_command(box,[box['python'],box['helpers']+'/tools/acceptance_spectator_tasks.py','--preflight',remote_root+'/payload.json']),timeout=240)
            if box['kind'] == 'windows-local':
                value=remote.read_json(box,remote_root+'/preflight.json')
            else:
                world.fetch_preserved(box,remote_root,own/'preflight-fetch')
                value=json.loads((own/'preflight-fetch/preflight.json').read_text())
            write_json(own/'preflight.json',value)
            plan.setdefault('preflights',{})[box['name']]=value
            write_json(root/'spectator-remote.json',plan)
        check_preflights(plan)
        for box in plan['boxes']:
            remote.publish_new(box,root/'spectator-remote.json',remote.native_root(plan,box)+'/verified-plan.json')
        return leases
    except BaseException:
        failures=remote.stop_leases(plan,root,leases)
        write_json(root/'lease-cleanup.json',dict(errors=failures))
        raise


def launch(plan, root, evidence, leases=None):
    check_preflights(plan)
    cross.coordinator(plan)
    if not world.public_directory_available(Path(__file__).parent.parent):
        raise RuntimeError('public directory unavailable; native row not launched')
    proof=evidence/plan['run']/'before-launch'; proof.mkdir(parents=True,exist_ok=False)
    from shutil import copyfile
    for source in [root/'spectator-remote.json',*[root/'boxes'/box['name']/leaf for box in plan['boxes'] for leaf in ('preflight.json','payload.json')]]:
        target=proof/source.relative_to(root); target.parent.mkdir(parents=True,exist_ok=True); copyfile(source,target)
        if sha256(source)!=sha256(target): raise ValueError('pre-run evidence copy differs')
    boxes={box['name']:box for box in plan['boxes']}; roots={name:remote.native_root(plan,box) for name,box in boxes.items()}
    started,errors,relayed,processes=[],[],set(),{}
    clock_events=[]
    write_json(root/'clock-order.json',clock_events)
    def document(name, relative):
        code=('from pathlib import Path; import hashlib,json,sys; p=Path(sys.argv[1]); '
              'raw=p.read_bytes() if p.is_file() else None; '
              'value=None\n'
              'if raw is not None:\n'
              ' try: value=dict(document=json.loads(raw),sha256=hashlib.sha256(raw).hexdigest())\n'
              ' except json.JSONDecodeError: pass\n'
              'print(json.dumps(value))')
        return json.loads(remote.remote_python(boxes[name],code,roots[name]+'/'+relative))
    def publish(name, relative, value):
        local=root/'relayed'/name/relative
        local.parent.mkdir(parents=True,exist_ok=True); write_json(local,value)
        remote.remote_python(boxes[name],'from pathlib import Path; import sys; Path(sys.argv[1]).parent.mkdir(parents=True,exist_ok=True)',roots[name]+'/'+relative)
        remote.publish_new(boxes[name],local,roots[name]+'/'+relative)
    def clock_event(kind,peer,receipt,**extra):
        clock_events.append(dict(sequence=len(clock_events)+1,kind=kind,peer=peer,sha256=receipt['sha256'],**extra))
        write_json(root/'clock-order.json',clock_events)
    try:
        for name in ('Linux','EDITH','EROL-PC'):
            box=boxes[name]; own=root/'boxes'/name
            driver=box['helpers']+'/tools/acceptance_spectator_tasks.py'
            payload=roots[name]+'/payload.json'
            if box['kind']=='windows-task':
                script=render_payload(box['tree'],[driver,'--payload',payload],roots[name]+'/task.log',roots[name]+'/task.done',
                    env=dict(CCCP_HEADLESS='1',CC_RUNNER_IGNORE_FULLSCREEN='1',PYTHONDONTWRITEBYTECODE='1'),path_prepend=box.get('path_prepend'))
                write_text(own/'task.ps1',script)
                RemoteBox(box['ssh'],box['runner'],box['task_script']).start_task(own/'task.ps1',budget_s=60)
            else:
                bootstrap=('import sys,runpy; from pathlib import Path; log=open(sys.argv[3],"x"); '
                    'sys.stdout=log; sys.stderr=log; sys.dont_write_bytecode=True; '
                    'script,payload=sys.argv[1:3]; sys.path.insert(0,str(Path(script).parent)); '
                    'sys.argv=[script,"--payload",payload]; runpy.run_path(script,run_name="__main__")')
                processes[name]=subprocess.Popen(remote.native_command(box,[box['python'],'-c',bootstrap,driver,payload,roots[name]+'/payload.log']),
                    stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            started.append(name)
        deadline=time.monotonic()+180
        while time.monotonic()<deadline:
            if all(remote.read_json(box,roots[name]+'/launch-ready.json') for name,box in boxes.items()): break
            for name,box in boxes.items():
                done=remote.read_json(box,roots[name]+'/done.json')
                if done is not None and not done['passed']: raise RuntimeError(name+': '+str(done['errors']))
                if name in processes and processes[name].poll() is not None: raise RuntimeError(name+': native payload exited before readiness')
            time.sleep(2)
        else: raise TimeoutError('native spectator launch readiness')
        write_json(root/'launch-go.json',dict(ready=True))
        for name,box in boxes.items(): remote.publish_new(box,root/'launch-go.json',roots[name]+'/launch-go.json')
        deadline,session,watcher,next_status=time.monotonic()+720,False,False,0
        initials,begin,end={}, {}, {}
        timing_start=timing_begun=armed=closed=timing_done=False
        while time.monotonic()<deadline:
            control=remote.read_json(boxes['EROL-PC'],roots['EROL-PC']+'/control.json') or {}
            if control.get('session') and not session:
                for name in boxes: publish(name,'session.json',dict(session=control['session']))
                session=True
            for peer in SEATED:
                if peer in initials: continue
                relative=peer+'-stage/probe/initial-seat.ownership.json'
                value=document(PLACEMENT[peer],relative)
                if value is None: continue
                if peer=='seated-one':
                    if value['document'].get('ownership',{}).get('seat') != 1: raise RuntimeError('first native seat differs from stable seat 1')
                    publish('EDITH',relative,value['document'])
                initials[peer]=value
            tick=control.get('live_ticks',{}).get('host')
            if session and len(initials)==3 and not watcher and isinstance(tick,int) and tick>=1200:
                go=dict(native_host_tick=tick,initial_seats={peer:value['sha256'] for peer,value in initials.items()})
                write_json(root/'watcher-go.json',go); publish('Linux','watcher-go.json',go); watcher=True
            if watcher and not timing_start:
                ready=document('Linux','spectator-stage/probe/spectator-ready.json')
                if ready is not None:
                    for name in ('EROL-PC','EDITH'): publish(name,'timing-start.json',dict(watcher_ready_sha256=ready['sha256']))
                    timing_start=True
            if timing_start:
                for peer in SEATED:
                    if peer in begin: continue
                    value=document(PLACEMENT[peer],peer+'-stage/probe/crawl-start.ownership.json')
                    if value is not None:
                        begin[peer]=value; clock_event('crawl-start',peer,value)
            if len(begin)==3 and not timing_begun:
                publish('Linux','timing-begun.json',dict(seated={peer:value['sha256'] for peer,value in begin.items()}))
                timing_begun=True
            if timing_begun and not armed:
                value=document('Linux','spectator-stage/probe/crawl-armed.ownership.json')
                if value is not None: clock_event('crawl-armed','spectator',value); armed=True
            if armed and not closed:
                value=document('Linux','spectator-stage/probe/crawl-complete.ownership.json')
                signal=document('Linux','spectator-stage/probe/crawl-complete.json')
                if value is not None and signal is not None:
                    clock_event('crawl-closed','spectator',value,signal_sha256=signal['sha256'])
                    for name in ('EROL-PC','EDITH'): publish(name,'spectator-stage/probe/crawl-complete.json',signal['document'])
                    closed=True
            if closed:
                for peer in SEATED:
                    if peer in end: continue
                    value=document(PLACEMENT[peer],peer+'-stage/probe/crawl-end.ownership.json')
                    if value is not None: end[peer]=value; clock_event('crawl-end',peer,value)
            if len(end)==3 and not timing_done:
                publish('EROL-PC','timing-complete.json',dict(seated={peer:value['sha256'] for peer,value in end.items()}))
                timing_done=True
            for source,dest,peer,signal in [('EROL-PC','Linux','host','host-released'),
                    ('Linux','EROL-PC','spectator','promoted-input'),('EROL-PC','EDITH','host','done')]:
                key=(source,dest,peer,signal)
                if key in relayed: continue
                relative=peer+'-stage/probe/'+signal+'.json'
                value=document(source,relative)
                if value is not None: publish(dest,relative,value['document']); relayed.add(key)
            done={name:remote.read_json(box,roots[name]+'/done.json') for name,box in boxes.items()}
            for name,value in done.items():
                if value is not None and not value['passed']: raise RuntimeError(name+': '+str(value['errors']))
            if all(done.values()): break
            if time.monotonic()>=next_status:
                print(f'spectator native_host_tick={tick} watcher_released={watcher} clock_receipts={len(clock_events)}',flush=True)
                next_status=time.monotonic()+30
            time.sleep(1)
        else: raise TimeoutError('spectator coordinator deadline')
    except Exception as error:
        errors.append(str(error)); write_json(root/'stop.json',dict(reason=str(error)))
        for name in started:
            try: remote.publish_new(boxes[name],root/'stop.json',roots[name]+'/stop.json')
            except Exception as secondary: errors.append(name+': stop publication failed: '+str(secondary))
    finally:
        for name in started:
            box=boxes[name]
            try:
                if box['kind']=='windows-task':
                    outcome=RemoteBox(box['ssh'],box['runner'],box['task_script']).wait_done(roots[name]+'/task.done',120,slice_cap_s=30)
                    write_text(root/'boxes'/name/'task-outcome.txt',outcome+'\n')
                else:
                    _,failure=processes[name].communicate(timeout=120)
                    write_text(root/'boxes'/name/'ssh-outcome.txt',f'exit_code={processes[name].returncode}\n'+failure.decode('utf-8',errors='replace'))
            except Exception as error: errors.append(name+': final native collection: '+str(error))
        errors.extend(remote.stop_leases(plan,root,leases or {}))
        for name in started:
            try:
                if boxes[name]['kind'] != 'windows-local':
                    world.fetch_preserved(boxes[name],roots[name],root/'boxes'/name,compress_records=True,stream_transfer=True)
            except Exception as error: errors.append(name+': final native collection: '+str(error))
    result=collect(root)
    result['failures'].extend(errors); result['passed']=not result['failures']
    write_json(root/'spectator-report.json',result)
    return 0 if result['passed'] else 1


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--preflight',type=Path); parser.add_argument('--payload',type=Path)
    parser.add_argument('--lane'); parser.add_argument('--profiles',type=Path); parser.add_argument('--source-sha')
    parser.add_argument('--out',type=Path); parser.add_argument('--evidence',type=Path); parser.add_argument('--stage-only',action='store_true')
    options=parser.parse_args(argv)
    if options.preflight: return native_preflight(options.preflight)
    if options.payload:
        payload=json.loads(options.payload.read_text()); payload['plan']=json.loads((options.payload.parent/'verified-plan.json').read_text())
        # Use the verified plan in memory; preserve the staged payload unchanged.
        return run_payload(options.payload,payload)
    if not all((options.lane,options.profiles,options.source_sha,options.out,options.evidence)): parser.error('lane, profiles, source SHA, output and evidence are required')
    scratch=Path('D:/mx')/options.lane
    if not options.out.resolve().is_relative_to(scratch.resolve()) or options.out.resolve()==scratch.resolve(): parser.error('output must stay in the lane')
    if not re.fullmatch('[0-9a-f]{40}',options.source_sha): parser.error('full source SHA required')
    with storage_scope(scratch,reserve=64*1024**2):
        options.out.mkdir(parents=True,exist_ok=False)
        plan=plan_for(options,json.loads(options.profiles.read_text(encoding='utf-8-sig')))
        leases=stage(plan,options.out)
        try:
            if options.stage_only: return 0
            return launch(plan,options.out,options.evidence,leases)
        finally:
            if not (options.out/'lease-stop.json').exists():
                failures=remote.stop_leases(plan,options.out,leases)
                if failures: raise RuntimeError('spectator native reservation cleanup failed: '+str(failures))


if __name__=='__main__': raise SystemExit(main())
