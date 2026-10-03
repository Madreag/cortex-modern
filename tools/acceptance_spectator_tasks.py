"""Run the five-process spectator row in the two authorized Windows session tasks."""
from __future__ import annotations

import argparse
from copy import deepcopy
import json
import os
from pathlib import Path
import platform
import re
import time

import acceptance_remote_tasks as remote
from acceptance_mod import sha256
from acceptance_runtime import storage_scope, write_json, write_text
from acceptance_spectator import PEERS, SEATED, collect
import cross_peers as cross
from edith.remote_box import RemoteBox, render_payload
import world_mod_cross as world
from world_soak_tasks import helper_archive


def validate(box, lane, peers, native=False):
    if box.get('name') not in ('Z13', 'EDITH'):
        raise ValueError('spectator row uses only Z13 and EDITH')
    remote.validate_profile({**box, 'peers_per_box':1}, lane, 'world-join')
    expected = ('host',) if box['name'] == 'Z13' else (*SEATED, 'spectator')
    if tuple(peers) != expected or box.get('peers_per_box') != len(expected):
        raise ValueError('spectator row requires one dedicated host and four EDITH processes')
    if native and platform.node().casefold() != box['hostname'].casefold():
        raise ValueError('spectator payload is on the wrong physical machine')


def plan_for(options, profiles):
    by_name = {value['name']:deepcopy(value) for value in profiles if value['name'] in ('Z13','EDITH')}
    if set(by_name) != {'Z13','EDITH'}: raise ValueError('both Windows row trees are required')
    for name, box in by_name.items():
        box['peers_per_box'] = 1 if name == 'Z13' else 4
        box['helpers'] = box['scratch']+'/'+options.out.name+'-helpers'
        box['ports'] = [49320,49349]
        validate(box, options.lane, ('host',) if name == 'Z13' else (*SEATED,'spectator'))
    return dict(run=options.out.name, lane=options.lane, source_sha=options.source_sha,
                boxes=list(by_name.values()), peers=list(PEERS), coordinator_engine_instances=0,
                driver_sha256=sha256(Path(__file__)), driver_commit=cross.command(['git','rev-parse','HEAD']).strip(),
                configuration=dict(persistent_world=True, dedicated_host=True, humans=3, cpu_slots=0, world_max_spectators=1),
                throttle=dict(peer='spectator', microseconds=500000, from_tick=6000, until_tick=6060))


def native_preflight(path):
    payload = json.loads(Path(path).read_text())
    box, plan = payload['box'], payload['plan']
    validate(box, plan['lane'], payload['peers'], native=True)
    cross.preflight_payload(path)
    destination = Path(path).parent/'preflight.json'
    value = json.loads(destination.read_text())
    build = json.loads(Path(box['build_receipt']).read_text(encoding='utf-8-sig'))
    if build.get('configuration') != 'Final' or build.get('build_exit_code') != 0 or \
            build.get('commit') != plan['source_sha'] or build.get('executable_sha256') != value['executable_sha256'] or \
            sha256(Path(build['build_log'])) != build.get('build_log_sha256'):
        raise ValueError('native runtime differs from the compiled engineer tip')
    world.preflight_driver(box, value)
    value.update(build=build, spectator_driver_sha256=sha256(Path(__file__)))
    write_json(destination, value)
    return 0


def check_preflights(plan):
    values = plan.get('preflights', {})
    if set(values) != {'Z13','EDITH'}: raise ValueError('native preflights missing')
    cross.require_distinct_machines(values)
    reference = values['Z13']
    for box in plan['boxes']:
        value = values[box['name']]
        if value.get('load') or value.get('hostname','').casefold() != box['hostname'].casefold():
            raise ValueError('spectator box is occupied or has the wrong identity')
        if value['build'].get('commit') != plan['source_sha'] or value['build'].get('executable_sha256') != value['executable_sha256']:
            raise ValueError('spectator build identity differs')
        if any(value[key] != reference[key] for key in ('executable_sha256','content','modules','fixture','acceptance_driver_sources')):
            raise ValueError('Windows runtime or complete content differs')
        if value.get('spectator_driver_sha256') != plan['driver_sha256']:
            raise ValueError('spectator driver bytes differ')


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
    flags = ['-seed','42','-max-ticks','18001','-net-match-service-e2e','-net-port',str(49320+5*PEERS.index(peer)),
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
               CC_TEST_CROSS_EVENT_RAW_LIMIT=str(256*1024**2), CC_RUNNER_BOX_NAME=box['name'],
               CC_RUNNER_MIN_FREE_GB=str(box['launch_floor_gib']))
    if peer=='host': env['CC_TEST_WORLD_MAX_SPECTATORS']='1'
    if peer=='spectator': env.update(CCCP_TEST_SIM_COST_US='500000',CCCP_TEST_SIM_COST_FROM_TICK='6000',CCCP_TEST_SIM_COST_UNTIL_TICK='6060')
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
    os.environ.update(CC_RUNNER_BOX_NAME=box['name'], CC_RUNNER_MIN_FREE_GB=str(box['launch_floor_gib']))
    claim, runs, complete, errors = None, {}, {}, []
    try:
        claim = cross.acquire_reservation(box, root, 60)
        write_json(root/'reservation.json', {key:value for key,value in claim['record'].items() if key!='token'})
        caps = cross.read_capabilities(box, root)
        if caps['peer_limit'] < 4: raise RuntimeError('native build refuses the four-seat protocol capacity')
        write_json(root/'launch-ready.json',dict(box=box['name'],capabilities=caps))
        cross.wait_for_payload_release(root,360)
        deadline, session, next_status = time.monotonic()+640, None, 0
        def start(peer):
            cross.assert_box_guard(box)
            run = prepare_peer(box,plan,root,peer,session)
            runs[peer]=run
            run.start()
            if sha256(Path(run.argv[0])) != plan['preflights'][box['name']]['executable_sha256']:
                raise RuntimeError('native executable changed during launch')
        if box['name']=='Z13': start('host')
        while len(complete) < len(payload['peers']):
            if time.monotonic()>deadline: raise TimeoutError('spectator native payload deadline')
            if (root/'stop.json').is_file(): raise RuntimeError('coordinator cancelled the spectator row')
            if not cross.owns_reservation(box): raise RuntimeError('physical reservation was lost')
            if box['name']=='EDITH':
                session_file=root/'session.json'
                if session is None and session_file.is_file():
                    session=json.loads(session_file.read_text())['session']; start('seated-one')
                initial=root/'seated-one-stage/probe/initial-seat.ownership.json'
                if 'seated-two' not in runs and initial.is_file():
                    if json.loads(initial.read_text())['ownership'].get('seat') != 1:
                        raise RuntimeError('first native seated participant did not own stable seat 1')
                    start('seated-two'); start('seated-three')
                all_seated=all((root/(peer+'-stage')/'probe/initial-seat.ownership.json').is_file() for peer in SEATED)
                if all_seated and 'spectator' not in runs and (root/'watcher-go.json').is_file(): start('spectator')
            if time.monotonic()>=next_status:
                status=dict(box=box['name'],started=list(runs),completed=list(complete),
                            live_ticks={peer:world.latest_tick(root/(peer+'-live.jsonl')) for peer in runs})
                if 'host' in runs:
                    log=root/'host/stdout.log'
                    text=log.read_text(encoding='utf-8',errors='replace') if log.is_file() else ''
                    sessions=re.findall(r'(?m)^\[net-directory\] registered session_id=(\S+) heartbeat_s=\d+',text)
                    if sessions: status['session']=sessions[-1]
                write_json(root/'control.json',status)
                if cross.box_load([cross.engine_pid(run) for peer,run in runs.items() if peer not in complete]):
                    raise RuntimeError('another native workload appeared during the spectator row')
                cross.assert_box_guard(box)
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
        if claim: write_json(root/'reservation-released.json',dict(released=cross.release_reservation(claim)))
        write_json(root/'done.json',dict(box=box['name'],passed=not errors and len(complete)==len(payload['peers']),errors=errors))
    return int(bool(errors))


def stage(plan, root):
    archive=helper_archive(Path(__file__).parent.parent,root)
    for box in plan['boxes']:
        remote_root=box['scratch']+'/'+plan['run']
        remote.remote_python(box,'from pathlib import Path; import sys; Path(sys.argv[1]).mkdir(exist_ok=False); Path(sys.argv[2]).mkdir(exist_ok=False)',remote_root,box['helpers'])
        cross.command(['scp','-q',str(archive),box['ssh']+':'+box['helpers']+'/helpers.tar'],timeout=180)
        cross.command(cross.remote_command(box,['tar.exe','-xf',box['helpers']+'/helpers.tar','-C',box['helpers']]),timeout=180)
        own=root/'boxes'/box['name']; own.mkdir(parents=True)
        peers=('host',) if box['name']=='Z13' else (*SEATED,'spectator')
        write_json(own/'payload.json',dict(box=box,plan=plan,peers=peers,specs=[dict(acceptance_row='spectator')]))
        remote.publish_new(box,own/'payload.json',remote_root+'/payload.json')
        cross.command(cross.remote_command(box,[box['python'],box['helpers']+'/tools/acceptance_spectator_tasks.py','--preflight',remote_root+'/payload.json']),timeout=240)
        world.fetch_preserved(box,remote_root,own/'preflight-fetch')
        value=json.loads((own/'preflight-fetch/preflight.json').read_text())
        write_json(own/'preflight.json',value)
        plan.setdefault('preflights',{})[box['name']]=value
        write_json(root/'spectator-remote.json',plan)
    check_preflights(plan)
    # The measured preflights travel in a separate immutable file; the original
    # payload and its transferred hard-link candidate remain retained.
    for box in plan['boxes']:
        remote.publish_new(box,root/'spectator-remote.json',box['scratch']+'/'+plan['run']+'/verified-plan.json')


def launch(plan, root, evidence):
    check_preflights(plan)
    if not world.public_directory_available(Path(__file__).parent.parent):
        raise RuntimeError('public directory unavailable; native row not launched')
    proof=evidence/plan['run']/'before-launch'; proof.mkdir(parents=True,exist_ok=False)
    from shutil import copyfile
    for source in [root/'spectator-remote.json',*[root/'boxes'/box['name']/leaf for box in plan['boxes'] for leaf in ('preflight.json','payload.json')]]:
        target=proof/source.relative_to(root); target.parent.mkdir(parents=True,exist_ok=True); copyfile(source,target)
        if sha256(source)!=sha256(target): raise ValueError('pre-run evidence copy differs')
    boxes={box['name']:box for box in plan['boxes']}; roots={name:box['scratch']+'/'+plan['run'] for name,box in boxes.items()}
    started,errors,relayed=[],[],set()
    try:
        for name in ('EDITH','Z13'):
            box=boxes[name]; own=root/'boxes'/name
            script=render_payload(box['tree'],[box['helpers']+'/tools/acceptance_spectator_tasks.py','--payload',roots[name]+'/payload.json'],
                                  roots[name]+'/task.log',roots[name]+'/task.done',env=dict(CCCP_HEADLESS='1',CC_RUNNER_IGNORE_FULLSCREEN='1'),path_prepend=box.get('path_prepend'))
            write_text(own/'task.ps1',script)
            RemoteBox(box['ssh'],box['runner'],box['task_script']).start_task(own/'task.ps1',budget_s=60)
            started.append(name)
        deadline=time.monotonic()+180
        while time.monotonic()<deadline:
            if all(remote.read_json(box,roots[name]+'/launch-ready.json') for name,box in boxes.items()): break
            for name,box in boxes.items():
                done=remote.read_json(box,roots[name]+'/done.json')
                if done is not None and not done['passed']: raise RuntimeError(name+': '+str(done['errors']))
            time.sleep(2)
        else: raise TimeoutError('native spectator launch readiness')
        write_json(root/'launch-go.json',dict(ready=True))
        for name,box in boxes.items(): remote.publish_new(box,root/'launch-go.json',roots[name]+'/launch-go.json')
        deadline,session,watcher,next_status=time.monotonic()+720,False,False,0
        signals=[('EDITH','Z13','spectator','crawl-complete'),('EDITH','Z13','seated-one','departed'),
                 ('Z13','EDITH','host','host-released'),('EDITH','Z13','spectator','promoted-input'),('Z13','EDITH','host','done')]
        while time.monotonic()<deadline:
            control=remote.read_json(boxes['Z13'],roots['Z13']+'/control.json') or {}
            if control.get('session') and not session:
                write_json(root/'session.json',dict(session=control['session']))
                remote.publish_new(boxes['EDITH'],root/'session.json',roots['EDITH']+'/session.json'); session=True
            tick=control.get('live_ticks',{}).get('host')
            if session and not watcher and isinstance(tick,int) and tick>=1200:
                write_json(root/'watcher-go.json',dict(native_host_tick=tick))
                remote.publish_new(boxes['EDITH'],root/'watcher-go.json',roots['EDITH']+'/watcher-go.json'); watcher=True
            for source,dest,peer,signal in signals:
                key=(peer,signal)
                if key in relayed: continue
                relative=peer+'-stage/probe/'+signal+'.json'
                value=remote.read_json(boxes[source],roots[source]+'/'+relative)
                if value is None: continue
                local=root/'relayed'/relative; local.parent.mkdir(parents=True,exist_ok=True); write_json(local,value)
                remote.remote_python(boxes[dest],'from pathlib import Path; import sys; Path(sys.argv[1]).parent.mkdir(parents=True,exist_ok=True)',roots[dest]+'/'+relative)
                remote.publish_new(boxes[dest],local,roots[dest]+'/'+relative); relayed.add(key)
            done={name:remote.read_json(box,roots[name]+'/done.json') for name,box in boxes.items()}
            for name,value in done.items():
                if value is not None and not value['passed']: raise RuntimeError(name+': '+str(value['errors']))
            if all(done.values()): break
            if time.monotonic()>=next_status:
                print(f'spectator native_host_tick={tick} watcher_released={watcher} native_signals={len(relayed)}',flush=True)
                next_status=time.monotonic()+30
            time.sleep(2)
        else: raise TimeoutError('spectator coordinator deadline')
    except Exception as error:
        errors.append(str(error)); write_json(root/'stop.json',dict(reason=str(error)))
        for name in started:
            try: remote.publish_new(boxes[name],root/'stop.json',roots[name]+'/stop.json')
            except Exception as secondary: errors.append(name+': stop publication failed: '+str(secondary))
    finally:
        for name in started:
            box=boxes[name]
            outcome=RemoteBox(box['ssh'],box['runner'],box['task_script']).wait_done(roots[name]+'/task.done',120,slice_cap_s=30)
            write_text(root/'boxes'/name/'task-outcome.txt',outcome+'\n')
            world.fetch_preserved(box,roots[name],root/'boxes'/name,compress_records=True,stream_transfer=True)
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
        stage(plan,options.out)
        if options.stage_only: return 0
        return launch(plan,options.out,options.evidence)


if __name__=='__main__': raise SystemExit(main())
