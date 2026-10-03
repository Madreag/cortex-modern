"""Run the NOTE 8 four-box rows through their separate native engine trees."""
from __future__ import annotations

import argparse
from copy import deepcopy
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import subprocess
import time

import cross_peers as cross
import world_mod_cross as world
from acceptance_mod import sha256
from acceptance_runtime import ACTIVE_STORAGE, check_storage, storage_scope, write_json, write_text
from edith.remote_box import RemoteBox, render_payload
from world_soak_tasks import helper_archive, read_json as read_windows_json

ROWS = ('mod-match', 'mod-refusal', 'world-join')
ALIASES = {'Z13': 'z13', 'EDITH': 'edith', 'Mac': 'Erol-Mac', 'Linux': '3090'}
PEERS = {'erol': 'Z13', 'edith': 'EDITH', 'mac': 'Mac', 'linux': 'Linux'}


def validate_profile(box, lane, row='mod-match'):
    if row not in ROWS: raise ValueError('unknown four-box acceptance row')
    name = box.get('name')
    if name not in ALIASES or box.get('ssh') != ALIASES[name]:
        raise ValueError('four-box rows require Z13, EDITH, Mac and Linux through their named aliases')
    windows = name in ('Z13', 'EDITH')
    if box.get('kind') != ('windows-task' if windows else 'posix-ssh'):
        raise ValueError('no local engine launch is authorized')
    root = ('D:/mx/' if windows else '/Users/erol/cortex-workers/' if name == 'Mac' else '/home/erol/cortex-workers/')+lane
    helpers = PurePosixPath(box.get('helpers', '').replace('\\', '/'))
    if box.get('scratch') != root or not helpers.is_relative_to(PurePosixPath(root)) or '..' in helpers.parts:
        raise ValueError('helpers and retained evidence must stay inside this lane')
    if box.get('peers_per_box') != 1 or not box.get('exclusive_marker') or not box.get('hostname'):
        raise ValueError('one process, a real host identity and a box reservation are required')
    if windows:
        if row == 'world-join':
            tree = 'D:/Projects/z13-rows-build' if name == 'Z13' else root+'/engine-tip'
        else:
            tree = 'D:/Projects/z13-build' if name == 'Z13' else 'D:/Projects/inventory-build'
        if box.get('tree') != tree or box.get('executable') != tree+'/Cortex Command.exe':
            raise ValueError('Windows runtime differs from the authorized existing path')
        if box.get('runner') != 'cortex-session1' or box.get('task_script') != 'D:/mx/session1/run.ps1':
            raise ValueError('Windows engines require the existing session task')
        if box['exclusive_marker'] != 'D:/mx/FEEL-MATRIX-RUNNING':
            raise ValueError('Windows task must take the physical box reservation')
        if box.get('launch_floor_gib') != (6.5 if name == 'Z13' else 10):
            raise ValueError('named Windows memory floor changed')
    else:
        shared = str(PurePosixPath(root).parent/'ACCEPTANCE-STREAM-RUNNING')
        if box['exclusive_marker'] != shared:
            raise ValueError('POSIX payload must reserve the shared acceptance stream marker')


def make_plan(options, profiles, mod_receipts=None):
    if len(profiles) != 4 or {box.get('name') for box in profiles} != set(ALIASES):
        raise ValueError('exactly the four authorized machines are required')
    for box in profiles:
        validate_profile(box, options.lane, options.row)
    by_name = {box['name']: deepcopy(box) for box in profiles}
    base = cross.parse_args(['--boxes', str(options.template_boxes), '--lane', options.lane,
                             '--mac-guard', by_name['Mac'].get('guard_file', by_name['Mac']['exclusive_marker']), '--roster', 'four-way',
                             '--out', str(options.out), '--ticks', '12001' if options.row == 'world-join' else '1201',
                             '--timeout', '900', '--quiet-window'])
    plan = world.configure_plan(cross.make_plan(base), options.row, mod_receipts)
    for spec in plan['specs']:
        box = by_name[PEERS[spec['peer']]]
        old_root, new_root = spec['root'], box['scratch']+'/'+plan['run']
        def rebase(value):
            if isinstance(value, str):
                return value.replace(old_root+'/', new_root+'/') if value != old_root else new_root
            if isinstance(value, list): return [rebase(item) for item in value]
            if isinstance(value, dict): return {key: rebase(item) for key, item in value.items()}
            return value
        updated = rebase(spec)
        spec.clear(); spec.update(updated)
        spec.update(box=box['name'], repo=box['tree'], executable=box['executable'],
                    task_control=True, under_load_by_design=False, initial_skill=100,
                    module_source=box.get('module_source', box['tree']+'/Data/VoidWanderers.rte'))
        spec['port_block'] = [box['ports'][0], box['ports'][0]+4]
        spec['flags'] = world.flag(spec['flags'], '-net-port', spec['port_block'][0])
        spec['env'].update(box.get('environment', {}))
        spec['env'].update(CCCP_HEADLESS='1', CC_RUNNER_IGNORE_FULLSCREEN='1')
    for instance in plan['instances']:
        spec = next(spec for spec in plan['specs'] if spec['peer'] == instance['name'])
        instance.update(box=spec['box'], port_block=spec['port_block'])
    plan.update(boxes=list(by_name.values()), acceptance_host_box='Z13', world_host_box='Z13',
                source_sha=options.source_sha, expected_source_sha=options.source_sha,
                coordinator_only=dict(box='EROL-PC', engine_instances=0),
                authorization='LEAD-NOTES NOTE 8 / RESUME 3')
    plan['driver_sources']['acceptance_remote_tasks.py'] = sha256(Path(__file__))
    return plan


def validate_preflights(plan):
    values = plan.get('preflights', {})
    if set(values) != set(ALIASES):
        raise ValueError('one native preflight per named physical box is required')
    cross.require_distinct_machines(values)
    source = plan.get('expected_source_sha')
    if not re.fullmatch(r'[0-9a-f]{40}', str(source)):
        raise ValueError('full expected source commit is required')
    reference = values['Z13']
    for box in plan['boxes']:
        validate_profile(box, plan['lane'], plan['acceptance_row'])
        name, value = box['name'], values[box['name']]
        build = value.get('build', {})
        if value.get('hostname', '').casefold() != box['hostname'].casefold():
            raise ValueError(name+': hostname differs from the bound physical machine')
        if build.get('commit') != source or build.get('executable_sha256') != value.get('executable_sha256'):
            raise ValueError(name+': native build source and measured executable do not agree')
        if not re.fullmatch(r'[0-9a-f]{64}', str(value.get('executable_sha256'))):
            raise ValueError(name+': native executable digest is missing')
        if name == 'EDITH' and value['executable_sha256'] != reference['executable_sha256']:
            raise ValueError('Windows executable bytes differ')
        if any(value.get(key) != reference.get(key) for key in ('content', 'modules', 'fixture')):
            raise ValueError(name+': complete content/module/fixture manifests differ')
        if value.get('load'):
            raise ValueError(name+': another build or engine is active')
        if value.get('acceptance_remote_driver_sha256') != plan['driver_sources']['acceptance_remote_tasks.py']:
            raise ValueError(name+': remote task coordinator bytes differ')
    world.check_driver_preflights(plan, values)
    world.check_mod_preflights(plan, values)


def remote_python(box, code, *arguments):
    return cross.command(cross.remote_command(box, [box['python'], '-c', code, *arguments]), timeout=240)


def read_json(box, path):
    if box['kind'] == 'windows-task':
        return read_windows_json(RemoteBox(box['ssh'], box['runner'], box['task_script']), path)
    raw = remote_python(box, 'from pathlib import Path; import sys; p=Path(sys.argv[1]); print(p.read_text() if p.is_file() else "null")', path)
    return json.loads(raw)


def publish_new(box, local, remote):
    # A hard link exposes a complete immutable control file and retains the
    # transferred candidate. No move, replacement or deletion under scratch.
    candidate = remote+f'.retained-{time.monotonic_ns()}'
    cross.command(['scp', '-q', str(local), box['ssh']+':'+candidate], timeout=120)
    remote_python(box, 'import os,sys; os.link(sys.argv[1],sys.argv[2])', candidate, remote)


def preflight_payload(path):
    payload = json.loads(Path(path).read_text(encoding='utf-8-sig'))
    box = payload['box']
    specs = payload.get('specs', [])
    if len(specs) != 1 or PEERS.get(specs[0].get('peer')) != box['name']:
        raise ValueError('payload differs from the authorized four-box roster')
    validate_profile(box, PurePosixPath(box['scratch']).name, specs[0].get('acceptance_row'))
    if platform.node().casefold() != box['hostname'].casefold() or platform.node().casefold() == 'erol-pc':
        raise ValueError('payload is on the wrong physical host; no engine launched')
    result = cross.preflight_payload(path)
    receipt = Path(path).parent/'preflight.json'
    value = json.loads(receipt.read_text(encoding='utf-8'))
    if box.get('build_receipt'):
        source = Path(box['build_receipt'])
        build = json.loads(source.read_text(encoding='utf-8-sig'))
        configuration = 'Final' if box['kind'] == 'windows-task' else 'release'
        if build.get('build_exit_code') != 0 or build.get('configuration') != configuration or \
                build.get('executable_sha256') != value['executable_sha256'] or \
                not build.get('build_log') or sha256(Path(build['build_log'])) != build.get('build_log_sha256'):
            raise ValueError('native release build receipt or its build log differs')
        value.update(build=build, acceptance_build_receipt=dict(path=str(source), sha256=sha256(source)))
    value['acceptance_remote_driver_sha256'] = sha256(Path(__file__))
    write_json(receipt, value)
    return result


def run_payload(path):
    payload = json.loads(Path(path).read_text(encoding='utf-8-sig'))
    box, specs = payload['box'], payload['specs']
    if len(specs) != 1 or PEERS.get(specs[0].get('peer')) != box['name']:
        raise ValueError('payload differs from the authorized four-box roster')
    validate_profile(box, PurePosixPath(box['scratch']).name, specs[0].get('acceptance_row'))
    if platform.node().casefold() != box['hostname'].casefold() or platform.node().casefold() == 'erol-pc':
        raise ValueError('payload is on the wrong physical host; no engine launched')
    if len(specs) != 1 or PEERS.get(specs[0].get('peer')) != box['name'] or specs[0].get('acceptance_row') not in ROWS:
        raise ValueError('payload differs from the authorized four-box roster')
    from feel import launch_budget
    settings = dict(CC_RUNNER_BOX_NAME=box['name'])
    if 'launch_floor_gib' in box:
        settings['CC_RUNNER_MIN_FREE_GB'] = str(box['launch_floor_gib'])
    previous, claim = {key: os.environ.get(key) for key in settings}, None
    try:
        os.environ.update(settings)
        launch_budget.install_memory_guard()
        claim = cross.acquire_reservation(box, Path(path).parent, 60)
        write_json(Path(path).parent/'reservation.json', {key: value for key, value in claim['record'].items() if key != 'token'})
        return cross.run_payload(path)
    finally:
        if claim:
            released = cross.release_reservation(claim)
            write_json(Path(path).parent/'reservation-released.json', dict(released=released))
        for key, value in previous.items():
            if value is None: os.environ.pop(key, None)
            else: os.environ[key] = value


def stage(plan, root):
    archive = helper_archive(Path(__file__).resolve().parent.parent, root)
    for box in plan['boxes']:
        remote_root = box['scratch']+'/'+plan['run']
        remote_python(box, 'from pathlib import Path; import sys; Path(sys.argv[1]).mkdir(parents=True,exist_ok=False); Path(sys.argv[2]).mkdir(parents=True,exist_ok=False)',
                      remote_root, box['helpers'])
        cross.command(['scp', '-q', str(archive), box['ssh']+':'+box['helpers']+'/helpers.tar'], timeout=180)
        cross.command(cross.remote_command(box, ['tar.exe' if box['kind'] == 'windows-task' else 'tar', '-xf', box['helpers']+'/helpers.tar', '-C', box['helpers']]), timeout=180)
        own = root/'boxes'/box['name']; own.mkdir(parents=True)
        write_json(own/'payload.json', dict(box=box, specs=[spec for spec in plan['specs'] if spec['box'] == box['name']], pin=''))
        publish_new(box, own/'payload.json', remote_root+'/payload.json')
        cross.command(cross.remote_command(box, [box['python'], box['helpers']+'/tools/acceptance_remote_tasks.py', '--preflight', remote_root+'/payload.json']), timeout=240)
        world.fetch_preserved(box, remote_root, own/'preflight-fetch')
        value = json.loads((own/'preflight-fetch/preflight.json').read_text(encoding='utf-8-sig'))
        write_json(own/'preflight.json', value)
        plan.setdefault('preflights', {})[box['name']] = value
        write_json(root/'manifest.json', plan)
    validate_preflights(plan)


def preserve_before_launch(plan, root, evidence):
    validate_preflights(plan)
    destination = evidence/plan['run']/f'before-launch-{time.monotonic_ns()}'
    destination.mkdir(parents=True, exist_ok=False)
    files = [root/'manifest.json', *[root/'boxes'/name/'preflight.json' for name in ALIASES],
             *[root/'boxes'/name/'payload.json' for name in ALIASES]]
    index = []
    for path in files:
        if path.stat().st_size > 8*1024**2:
            raise ValueError('pre-run proof is too large for small evidence retention')
        target = destination/path.relative_to(root)
        target.parent.mkdir(parents=True, exist_ok=True)
        raw = path.read_bytes()
        with target.open('xb') as output: output.write(raw)
        if sha256(path) != sha256(target): raise ValueError('pre-run proof copy differs')
        index.append(dict(path=path.relative_to(root).as_posix(), bytes=len(raw), sha256=sha256(target)))
    write_json(destination/'before-run-proof.json', dict(run=plan['run'], files=index, engine_launches_before_copy=0))


def launch(plan, root, evidence):
    validate_preflights(plan)
    if not world.public_directory_available(Path(__file__).resolve().parent.parent):
        raise RuntimeError('public directory is unavailable; a lane-owned fallback must be prepared before launch')
    for box in plan['boxes']:
        remote_root = box['scratch']+'/'+plan['run']
        if any(read_json(box, remote_root+'/'+leaf) is not None
               for leaf in ('reservation.json', 'payload-owned.json', 'done.json')):
            raise FileExistsError('a remote payload already used this run; retain it and select a fresh run name')
    preserve_before_launch(plan, root, evidence)
    boxes = {box['name']: box for box in plan['boxes']}
    roots = {name: box['scratch']+'/'+plan['run'] for name, box in boxes.items()}
    started, processes = [], {}
    published = late_released = False
    plan['driver_findings'] = []
    try:
        for name in ('Linux', 'Mac', 'EDITH', 'Z13'):
            box, remote_root, own = boxes[name], roots[name], root/'boxes'/name
            args = [box['helpers']+'/tools/acceptance_remote_tasks.py', '--payload', remote_root+'/payload.json']
            if box['kind'] == 'windows-task':
                remote = RemoteBox(box['ssh'], box['runner'], box['task_script'])
                script = render_payload(box['tree'], args, remote_root+'/task.log', remote_root+'/task.done',
                                        env=dict(CCCP_HEADLESS='1', CC_RUNNER_IGNORE_FULLSCREEN='1', PYTHONDONTWRITEBYTECODE='1'), path_prepend=box.get('path_prepend'))
                write_text(own/'task.ps1', script)
                remote.start_task(own/'task.ps1', budget_s=60)
            else:
                bootstrap = ('import sys,runpy; from pathlib import Path; '
                             'log=open(sys.argv[3],"x"); sys.stdout=log; sys.stderr=log; '
                             'script,payload=sys.argv[1:3]; sys.path.insert(0,str(Path(script).parent)); '
                             'sys.argv=[script,"--payload",payload]; runpy.run_path(script,run_name="__main__")')
                processes[name] = subprocess.Popen(cross.remote_command(box, [box['python'], '-c', bootstrap,
                                  args[0], args[2], remote_root+'/payload.log']),
                                  stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                  creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            started.append(name)
        deadline = time.monotonic()+180
        while time.monotonic() < deadline:
            ready = {name: read_json(boxes[name], roots[name]+'/launch-ready.json') for name in started}
            if all(ready.values()): break
            for name in started:
                done = read_json(boxes[name], roots[name]+'/done.json')
                if done is not None: cross.require_payload_success(boxes[name], done)
                if name in processes and processes[name].poll() is not None:
                    raise RuntimeError(name+': payload exited before launch readiness')
            time.sleep(2)
        else: raise TimeoutError('four remote payloads did not become ready')
        write_json(root/'launch-go.json', dict(boxes=sorted(started), ready=ready))
        for name in started: publish_new(boxes[name], root/'launch-go.json', roots[name]+'/launch-go.json')
        deadline, next_status = time.monotonic()+plan['deadlines']['launch_s']+180, 0
        while time.monotonic() < deadline:
            control = read_json(boxes['Z13'], roots['Z13']+'/host-control.json') or {}
            if control.get('session') and not published:
                write_json(root/'session.json', dict(session=control['session']))
                for name in ('EDITH', 'Mac', 'Linux'):
                    publish_new(boxes[name], root/'session.json', roots[name]+'/session.json')
                published = True
            late = plan.get('late_join')
            if published and late and not late_released and isinstance(control.get('host_tick'), int) and control['host_tick'] >= late['host_tick']:
                name = PEERS[late['peer']]
                publish_new(boxes[name], root/'session.json', roots[name]+'/session-late.json')
                write_json(root/'late-join-released.json', {key: control[key] for key in ('host_tick', 'host_elapsed_s', 'payload_monotonic_s')})
                late_released = True
            done = {name: read_json(boxes[name], roots[name]+'/done.json') for name in started}
            for name, outcome in done.items():
                if outcome is not None: cross.require_payload_success(boxes[name], outcome)
            if all(done.values()): break
            if time.monotonic() >= next_status:
                print(f'{plan["acceptance_row"]} host_tick={control.get("host_tick")} late_released={late_released}', flush=True)
                next_status = time.monotonic()+30
            check_storage(Path('D:/mx')/plan['lane'])
            time.sleep(2)
        else: raise TimeoutError('four-box coordinator deadline')
    except Exception as error:
        plan['driver_findings'].append(str(error))
        write_json(root/'stop.json', dict(reason=str(error)))
        for name in started:
            try:
                publish_new(boxes[name], root/'stop.json', roots[name]+'/stop.json')
            except Exception as cancellation_error:
                plan['driver_findings'].append(name+': stop publication failed: '+str(cancellation_error))
    finally:
        write_json(root/'manifest.json', plan)
        for name in started:
            box = boxes[name]
            if box['kind'] == 'windows-task':
                outcome = RemoteBox(box['ssh'], box['runner'], box['task_script']).wait_done(roots[name]+'/task.done', 120, slice_cap_s=30)
                write_text(root/'boxes'/name/'task-outcome.txt', outcome+'\n')
            else:
                _, errors = processes[name].communicate(timeout=120)
                write_text(root/'boxes'/name/'ssh-outcome.txt',
                           f'exit_code={processes[name].returncode}\n'+errors.decode('utf-8', errors='replace'))
        budget = ACTIVE_STORAGE.get()
        reserve = budget.reserve if budget is not None else 0
        try:
            if budget is not None:
                budget.reserve += 64*1024**2
                budget.admit(0)
            for name in started:
                world.fetch_preserved(boxes[name], roots[name], root/'boxes'/name, compress_records=True, stream_transfer=True)
        finally:
            if budget is not None: budget.reserve = reserve
    from acceptance_cross_report import build_report
    return build_report(root)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--payload', type=Path)
    parser.add_argument('--preflight', type=Path)
    parser.add_argument('--lane')
    parser.add_argument('--profiles', type=Path)
    parser.add_argument('--template-boxes', type=Path)
    parser.add_argument('--mod-receipts', type=Path)
    parser.add_argument('--source-sha')
    parser.add_argument('--row', choices=ROWS)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--evidence', type=Path)
    parser.add_argument('--stage-only', action='store_true')
    parser.add_argument('--launch-staged', action='store_true')
    options = parser.parse_args(argv)
    if options.payload: return run_payload(options.payload)
    if options.preflight: return preflight_payload(options.preflight)
    if not options.lane or not options.out or not options.evidence: parser.error('--lane, --out and --evidence are required')
    owned = Path('D:/mx')/options.lane
    if not re.fullmatch(r'[A-Za-z0-9_-]+', options.lane) or not options.out.resolve().is_relative_to(owned.resolve()) or options.out.resolve() == owned.resolve():
        parser.error('output must be a fresh run inside the named scratch root')
    with storage_scope(owned, reserve=1024**2):
        if options.launch_staged:
            plan = json.loads((options.out/'manifest.json').read_text(encoding='utf-8'))
        else:
            if not options.profiles or not options.template_boxes or not options.row or not options.source_sha:
                parser.error('--profiles, --template-boxes, --row and --source-sha are required')
            options.out.mkdir(parents=True, exist_ok=False)
            profiles = json.loads(options.profiles.read_text(encoding='utf-8-sig'))
            receipts = json.loads(options.mod_receipts.read_text(encoding='utf-8-sig')) if options.mod_receipts else None
            plan = make_plan(options, profiles, receipts)
            write_json(options.out/'manifest.json', plan)
            stage(plan, options.out)
        if options.stage_only:
            preserve_before_launch(plan, options.out, options.evidence)
            print('Four-box native preflights verified and small evidence copied; no engine launched')
            return 0
        return 0 if launch(plan, options.out, options.evidence)['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
