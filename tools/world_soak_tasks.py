"""Run the authorized R5 world on Z13 and two EDITH seats through their session tasks."""
from __future__ import annotations

import argparse
from copy import deepcopy
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import time

import cross_peers as cross
import world_mod_cross as world
from acceptance_mod import sha256
from acceptance_runtime import check_storage
from edith.remote_box import RemoteBox, ps_quote, render_payload

HOSTS = {'Z13': 'z13', 'EDITH': 'edith'}


def read_json(remote, path):
    # PowerShell Get-Content denies replacement while its read handle is open.
    # The payload publishes by atomic rename, so readers must share deletion.
    command = "$ErrorActionPreference='Stop'; $path="+ps_quote(path)+"; " + r"""
if (Test-Path -LiteralPath $path) {
    $stream = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
                            ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    try {
        $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::UTF8)
        try { $reader.ReadToEnd() } finally { $reader.Dispose() }
    } finally { $stream.Dispose() }
} else { '<<ABSENT>>' }
"""
    value = remote.ssh(command).strip()
    return None if value == '<<ABSENT>>' else json.loads(value.lstrip('\ufeff'))


def task_profile(box, lane):
    if box.get('name') not in HOSTS or box.get('ssh') != HOSTS[box['name']]:
        raise ValueError('R5 tasks require the named Z13 and EDITH aliases')
    if box.get('kind') != 'windows-task' or box.get('runner') != 'cortex-session1' or box.get('task_script') != 'D:/mx/session1/run.ps1':
        raise ValueError('R5 engines require the existing session tasks')
    if box.get('scratch') != 'D:/mx/'+lane or not box.get('helpers', '').startswith(box['scratch']+'/'):
        raise ValueError('task helper and evidence paths must stay inside this lane')
    if '..' in box['helpers'].replace('\\', '/').split('/'):
        raise ValueError('task helper path leaves its owned root')
    expected = 'D:/Projects/z13-build' if box['name'] == 'Z13' else 'D:/Projects/inventory-build'
    if box.get('tree') != expected or box.get('executable') != expected+'/Cortex Command.exe':
        raise ValueError('R5 executable must use its existing approved path')
    if box.get('exclusive_marker') != 'D:/mx/FEEL-MATRIX-RUNNING':
        raise ValueError('each task must take its real box reservation')
    count = 1 if box['name'] == 'Z13' else 2
    if box.get('peers_per_box') != count:
        raise ValueError('R5 requires one Z13 host and two EDITH seats')
    floor = 6.5 if box['name'] == 'Z13' else 10
    if box.get('launch_floor_gib') != floor:
        raise ValueError('task launch floor differs from the named box policy')


def make_plan(options, profiles):
    if {box.get('name') for box in profiles} != set(HOSTS) or len(profiles) != 2:
        raise ValueError('R5 requires exactly Z13 and EDITH')
    for box in profiles:
        task_profile(box, options.lane)
    base_options = cross.parse_args(['--boxes', str(options.template_boxes), '--lane', options.lane,
                                    '--mac-guard', 'planning-only', '--roster', 'four-way', '--out', str(options.out),
                                    '--ticks', '219601', '--timeout', '4260', '--quiet-window'])
    plan = world.configure_plan(cross.make_plan(base_options), 'world-soak')
    by_name = {box['name']: deepcopy(box) for box in profiles}
    for spec in plan['specs']:
        box = by_name['Z13' if spec['peer'] == 'erol' else 'EDITH']
        old_root = spec['root']
        new_root = box['scratch']+'/'+plan['run']
        def rebase(value):
            if isinstance(value, str):
                return value.replace(old_root+'/', new_root+'/') if value != old_root else new_root
            if isinstance(value, dict): return {key: rebase(item) for key, item in value.items()}
            if isinstance(value, list): return [rebase(item) for item in value]
            return value
        updated = rebase(spec)
        spec.clear(); spec.update(updated)
        spec.update(box=box['name'], repo=box['tree'], executable=box['executable'], task_control=True,
                    under_load_by_design=False)
        port = box['ports'][0]+(5 if spec['peer'] == 'edith-first' else 0)
        spec['port_block'] = [port, port+4]
        if port+4 > box['ports'][-1]:
            raise ValueError('EDITH needs two disjoint five-port instance blocks')
        spec['flags'] = world.flag(spec['flags'], '-net-port', port)
        spec['env'].update(CCCP_HEADLESS='1', CC_RUNNER_IGNORE_FULLSCREEN='1')
    for instance in plan['instances']:
        spec = next(spec for spec in plan['specs'] if spec['peer'] == instance['name'])
        instance.update(box=spec['box'], port_block=spec['port_block'])
    plan.update(boxes=list(by_name.values()), world_host_box='Z13', coordinator_only=dict(box='EROL-PC', engine_instances=0),
                authorization='LEAD-NOTES NOTE 5 / RESUME 1', mac_guard=None, quiet_window=True)
    plan['driver_sources']['world_soak_tasks.py'] = sha256(Path(__file__))
    return plan


def helper_archive(repo, root):
    archive = root/'helpers.tar'
    names = subprocess.check_output(['git', '-C', str(repo), 'ls-files', '-z', '--', 'tools'], text=True).split('\0')
    with tarfile.open(archive, 'x') as stream:
        for name in names:
            path = repo/name
            if name and path.is_file() and not path.is_symlink():
                stream.add(path, arcname=name, recursive=False)
    return archive


def stage(plan, root):
    repo = Path(__file__).resolve().parent.parent
    check_storage(root.parents[1])
    archive = helper_archive(repo, root)
    for box in plan['boxes']:
        remote = RemoteBox(box['ssh'], box['runner'], box['task_script'])
        remote.wait_task_idle(budget_s=60)
        remote.mkdir(box['helpers'])
        remote.scp_to(archive, box['helpers']+'/helpers.tar')
        remote.ssh('tar.exe -xf '+ps_quote(box['helpers']+'/helpers.tar')+' -C '+ps_quote(box['helpers']), timeout=120)
        remote_root = box['scratch']+'/'+plan['run']
        if remote.read_text(remote_root+'/payload.json') is not None:
            raise FileExistsError('task run already exists; retain it and choose a fresh run name')
        remote.mkdir(remote_root)
        own = root/'boxes'/box['name']; own.mkdir(parents=True)
        payload = dict(box=box, specs=[spec for spec in plan['specs'] if spec['box'] == box['name']], pin='')
        cross.write_json(own/'payload.json', payload)
        remote.scp_to(own/'payload.json', remote_root+'/payload.json')
        remote.ssh('& '+ps_quote(box['python'])+' '+ps_quote(box['helpers']+'/tools/cross_peers.py')+
                   ' --preflight '+ps_quote(remote_root+'/payload.json'), timeout=240)
        # Fetch the retained preflight root by tar, then count it in a separate command.
        world.fetch_preserved(box, remote_root, own/'preflight-fetch')
        preflight = json.loads((own/'preflight-fetch/preflight.json').read_text(encoding='utf-8-sig'))
        cross.write_json(own/'preflight.json', preflight)
        plan.setdefault('preflights', {})[box['name']] = preflight
        cross.write_json(root/'manifest.json', plan)
    validate_preflights(plan)


def validate_preflights(plan):
    values = plan['preflights']
    cross.require_distinct_machines(values)
    reference = values['Z13']
    source = reference.get('build', {}).get('commit')
    if not re.fullmatch(r'[0-9a-f]{40}', str(source)):
        raise ValueError('Z13 has no full native build source receipt')
    for name, value in values.items():
        build = value.get('build', {})
        if build.get('commit') != source or build.get('executable_sha256') != value.get('executable_sha256'):
            raise ValueError(name+': native build and measured executable do not agree')
        if value.get('executable_sha256') != reference.get('executable_sha256'):
            raise ValueError(name+': Windows executable differs from Z13')
        if any(value.get(key) != reference.get(key) for key in ('content', 'modules', 'fixture')):
            raise ValueError(name+': complete content/module/fixture manifests differ')
        if value.get('load'):
            raise ValueError(name+': another build or engine is active')
    world.check_driver_preflights(plan, values)
    plan['source_sha'] = source


def run_payload(path):
    payload = json.loads(Path(path).read_text(encoding='utf-8-sig'))
    box, specs = payload['box'], payload['specs']
    lane = Path(box['scratch']).name
    task_profile(box, lane)
    expected = {'erol'} if box['name'] == 'Z13' else {'edith-first', 'edith'}
    if {spec.get('peer') for spec in specs} != expected or any(spec.get('acceptance_row') != 'world-soak' for spec in specs):
        raise ValueError('task payload differs from the authorized R5 process roster')
    from feel import launch_budget
    settings = dict(CC_RUNNER_BOX_NAME=box['name'], CC_RUNNER_MIN_FREE_GB=str(box['launch_floor_gib']))
    previous, claim = {key:os.environ.get(key) for key in settings}, None
    try:
        os.environ.update(settings)
        launch_budget.install_memory_guard()
        claim = cross.acquire_reservation(box, Path(path).parent, 60)
        cross.write_json(Path(path).parent/'reservation.json', {key:value for key,value in claim['record'].items() if key != 'token'})
        return cross.run_payload(path)
    finally:
        for key, value in previous.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
        if claim:
            released = cross.release_reservation(claim)
            cross.write_json(Path(path).parent/'reservation-released.json', dict(released=released))


def launch(plan, root):
    """Only task payloads launch engines; this coordinator starts no local engine."""
    validate_preflights(plan)
    remotes = {box['name']: RemoteBox(box['ssh'], box['runner'], box['task_script']) for box in plan['boxes']}
    by_name = {box['name']: box for box in plan['boxes']}
    remote_roots = {name:box['scratch']+'/'+plan['run'] for name,box in by_name.items()}
    if not world.public_directory_available(Path(__file__).resolve().parent.parent):
        raise RuntimeError('public directory is unavailable; a lane-owned fallback must be staged before this run')
    plan['driver_findings'] = []
    cross.write_json(root/'manifest.json', plan)
    started, published, late_released = [], False, False
    try:
        for name in ('EDITH', 'Z13'):
            box, remote, own = by_name[name], remotes[name], root/'boxes'/name
            remote_root = remote_roots[name]
            script = render_payload(box['tree'], [box['helpers']+'/tools/world_soak_tasks.py', '--payload', remote_root+'/payload.json'],
                                    remote_root+'/task.log', remote_root+'/task.done',
                                    env=dict(CCCP_HEADLESS='1', CC_RUNNER_IGNORE_FULLSCREEN='1'), path_prepend=box.get('path_prepend'))
            (own/'task.ps1').write_text(script, encoding='utf-8')
            remote.start_task(own/'task.ps1', budget_s=60)
            started.append(name)
        deadline = time.monotonic()+180
        while time.monotonic() < deadline:
            ready = {name:read_json(remotes[name], remote_roots[name]+'/launch-ready.json') for name in started}
            if all(ready.values()): break
            for name in started:
                if remotes[name].read_text(remote_roots[name]+'/task.done') is not None:
                    raise RuntimeError(name+': task stopped before launch readiness')
            time.sleep(2)
        else:
            raise TimeoutError('both task payloads did not become ready')
        cross.write_json(root/'launch-go.json', dict(boxes=sorted(started), ready=ready))
        for name in started:
            cross.stage_remote(by_name[name], root/'launch-go.json', remote_roots[name]+'/launch-go.json')
        deadline, next_status = time.monotonic()+4500, 0
        while time.monotonic() < deadline:
            control = read_json(remotes['Z13'], remote_roots['Z13']+'/host-control.json') or {}
            if control.get('session') and not published:
                cross.write_json(root/'session.json', dict(session=control['session']))
                cross.stage_remote(by_name['EDITH'], root/'session.json', remote_roots['EDITH']+'/session.json')
                published = True
            elapsed = control.get('host_elapsed_s')
            if published and not late_released and isinstance(elapsed, (int, float)) and elapsed >= 3000:
                cross.stage_remote(by_name['EDITH'], root/'session.json', remote_roots['EDITH']+'/session-late.json')
                cross.write_json(root/'late-join-released.json', {key:control[key] for key in ('host_tick', 'host_elapsed_s', 'payload_monotonic_s')})
                late_released = True
            done = {name: read_json(remotes[name], remote_roots[name]+'/done.json') for name in started}
            for name, outcome in done.items():
                if outcome is not None:
                    cross.require_payload_success(by_name[name], outcome)
            if all(done.values()): break
            if time.monotonic() >= next_status:
                print(f'R5 host_tick={control.get("host_tick")} elapsed_s={elapsed} late_released={late_released}', flush=True)
                next_status = time.monotonic()+30
            check_storage(root.parents[1])
            time.sleep(2)
        else:
            raise TimeoutError('R5 task coordinator deadline')
    except Exception as error:
        plan['driver_findings'].append(str(error))
        cross.write_json(root/'stop.json', dict(reason=str(error)))
        for name in started:
            cross.stage_remote(by_name[name], root/'stop.json', remote_roots[name]+'/stop.json')
    finally:
        cross.write_json(root/'manifest.json', plan)
        for name in started:
            outcome = remotes[name].wait_done(remote_roots[name]+'/task.done', 120, slice_cap_s=30)
            (root/'boxes'/name/'task-outcome.txt').write_text(outcome+'\n', encoding='utf-8')
        for name in started:
            world.fetch_preserved(by_name[name], remote_roots[name], root/'boxes'/name,
                                  compress_records=True, stream_transfer=True)
    from acceptance_cross_report import build_report
    return build_report(root)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--payload', type=Path)
    parser.add_argument('--lane')
    parser.add_argument('--profiles', type=Path)
    parser.add_argument('--template-boxes', type=Path)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--stage-only', action='store_true')
    parser.add_argument('--launch-staged', action='store_true')
    options = parser.parse_args(argv)
    if options.payload:
        return run_payload(options.payload)
    if not options.out or not options.lane:
        parser.error('--out and --lane are required')
    owned = Path('D:/mx')/options.lane
    if not re.fullmatch(r'[A-Za-z0-9_-]+', options.lane) or not options.out.resolve().is_relative_to(owned.resolve()) or options.out.resolve() == owned.resolve():
        parser.error('output must be a fresh run inside the named lane scratch')
    if options.launch_staged:
        plan = json.loads((options.out/'manifest.json').read_text(encoding='utf-8'))
        return 0 if launch(plan, options.out)['passed'] else 1
    if not options.profiles or not options.template_boxes:
        parser.error('--profiles and --template-boxes are required')
    options.out.mkdir(parents=True, exist_ok=False)
    plan = make_plan(options, json.loads(options.profiles.read_text(encoding='utf-8-sig')))
    cross.write_json(options.out/'manifest.json', plan)
    stage(plan, options.out)
    cross.write_json(options.out/'manifest.json', plan)
    if options.stage_only:
        print('R5 tasks staged and native preflights verified; no engine launched')
        return 0
    return 0 if launch(plan, options.out)['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
