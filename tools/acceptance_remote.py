"""Run a declared Windows driver through its box's session task and retain its evidence."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import time

from acceptance_collection import read, write
from verdict_artifact import sha256

CREDENTIAL_FLAGS = {'--login-conf', '--turn-conf', '--turn-config'}


def inventory_modules(path):
    sys.path.insert(0, str(Path(path)))
    import run_split
    return run_split, run_split.load_remote_box(Path(__file__).resolve().parents[1])


def remote_commands(commands, local_repo, remote_repo, local_root, remote_root):
    result = []
    pairs = [(str(local_repo), str(remote_repo)), (str(local_root), str(remote_root))]
    pairs += [(old.replace('\\', '/'), new.replace('\\', '/')) for old, new in pairs]
    pairs.sort(key=lambda pair: len(pair[0]), reverse=True)
    for command in commands:
        if CREDENTIAL_FLAGS & set(command):
            raise ValueError('credential readers stay on box-a; only their game peers may be remote')
        argv = list(command)
        if argv and Path(argv[0]).name.lower() in ('python', 'python.exe', 'python3', 'python3.exe'):
            argv[0] = 'python'
        for index, value in enumerate(argv):
            for old, new in pairs:
                value = value.replace(old, new)
            argv[index] = value
        result.append(argv)
    return result


def prepare_box(repo, box, root, inventory_root, source, executable, *, declared_tree=False):
    split, rb = inventory_modules(inventory_root)
    box = box if declared_tree else split.execution_box(box)
    if split.held_reason(box):raise RuntimeError(split.held_reason(box))
    remote = rb.RemoteBox(box.ssh, box.task, box.session_script)
    work = Path(root)/'.windows'/box.name
    work.mkdir(parents=True, exist_ok=True)
    remote_root = split.remote_root_for(box, Path(root))
    payload = remote_root/'.acceptance'
    receipt = work/'prepared.json'
    if receipt.is_file():
        prior = read(receipt)
        if (prior.get('source_sha') != source or prior.get('exe_sha256') != executable or prior.get('repo') != box.repo):
            raise ValueError(f'{box.name}: prepared input identity changed within one collection')
        if remote.sha256(box.exe) != executable:
            raise ValueError(f'{box.name}: prepared executable changed')
        return box, remote, rb, remote_root
    reason = remote.reachable()
    if reason:
        raise RuntimeError(f'{box.name}: {reason}')
    problem = split.environment_problem(rb, remote, box)
    if problem:
        raise RuntimeError(problem)
    remote.wait_task_idle(budget_s=180 * 60, poll_s=30)
    remote.mkdir(payload)
    remote.mkdir(box.repo)
    remote.scp_to(Path(repo)/'tools/remote/remote_box.py', payload/'remote_box.py')
    shipped = split.ship_inputs(rb, remote, Path(repo), box, payload, work, lambda value: print(value, flush=True))
    synced = split.sync_git(rb, remote, Path(repo), box, payload, work, lambda value: print(value, flush=True))
    if shipped['exe_sha256_there'] != executable or synced['head_there'] != source:
        raise ValueError(f'{box.name}: staged source/executable differs from the collection')
    build_receipt = Path(root)/'build-receipt.json'
    if build_receipt.is_file():
        remote.scp_to(build_receipt, remote_root/'build-receipt.json')
    write(receipt, dict(source_sha=source, exe_sha256=executable, box=box.name, repo=box.repo, shipped=shipped, git=synced))
    return box, remote, rb, remote_root


def pack_evidence(root, archive):
    root, archive = Path(root).resolve(), Path(archive).resolve()
    paths = []
    for directory, names, files in os.walk(root, followlinks=False):
        names[:] = [name for name in names if name not in ('runtime', 'Data', '.git')
                    and not (Path(directory)/name).is_symlink() and not getattr(Path(directory)/name, 'is_junction', lambda: False)()]
        for name in files:
            path = Path(directory)/name
            if path == archive or path.suffix == '.tar' or path.is_symlink() or getattr(path, 'is_junction', lambda: False)():
                continue
            paths.append(path)
    with tarfile.open(archive, 'w') as bundle:
        for path in paths:
            bundle.add(path, arcname=path.relative_to(root).as_posix(), recursive=False)
    return {path.relative_to(root).as_posix(): sha256(path) for path in paths}


def sanitize_native(root, digests, *, cleanup=False):
    """A native gate uses salted digests from stdin; no credential input file is created."""
    from relay_secrets import DigestBook,SecretBook,sweep,walk_files
    from relay_scrub import scrub
    from relay_login_sweep import sweep as structural_sweep
    from acceptance_relay_policy import unsafe_reparse_points
    root=Path(root)
    observed=bool(digests.get('items'))
    if not observed and not cleanup:raise ValueError('relay evidence requires a nonempty credential book')
    book=DigestBook(digests) if observed else SecretBook()
    prior_failed=False
    for path in root.rglob('*secret-scan.json'):
        prior=read(path)
        prior_failed |= prior.get('passed') is False or prior.get('native_login_leak') is True
    cleaned=scrub([root],book=book)
    remaining=sweep([root],book.finder())
    structural=structural_sweep([root])
    unscanned=unsafe_reparse_points(root)
    safe=(remaining['files_scanned']>0 and remaining['status']=='CLEAN' and not remaining['files_with_secrets']
          and not cleaned['incomplete'] and cleaned['hits_after']==0
          and structural['status']=='CLEAN' and not structural['files_with_logins'] and not structural['incomplete'] and not unscanned)
    if safe and observed:
        for path in list(walk_files(root)):
            if path.name=='LogConsole.txt' and path.parent.name=='runtime':
                (path.parent.parent/'console.log').write_bytes(path.read_bytes())
    return dict(passed=safe and observed and cleaned['hits_before']==0 and not prior_failed,safe_to_copy=safe and observed,
                cleanup_clean=safe,book_values=len(digests.get('items',[])),unscanned=unscanned,
                files_scanned=remaining['files_scanned'],hits_before=cleaned['hits_before'],hits_after=cleaned['hits_after'],
                native_login_leak=cleaned['hits_before']>0 or prior_failed,sanitizer_receipt=cleaned,structural_scan=structural)


def sanitize_remote(box,remote,rb,root,book, *, cleanup=False):
    try:
        from acceptance_relay_policy import native_book
        digests=native_book(book).digests()
        if not digests['items'] and not cleanup:raise ValueError('relay evidence requires a nonempty credential book')
        action='--relay-cleanup' if cleanup else '--relay-sanitize'
        command=f'{getattr(box,"python","python")} {rb.ps_quote(box.repo+"/tools/acceptance_remote.py")} {action} {rb.ps_quote(Path(root).as_posix())}'
        process=subprocess.run(['ssh','-o','BatchMode=yes',box.ssh,command],input=json.dumps(digests),text=True,
                               capture_output=True,timeout=900,check=True)
        gate=json.loads(process.stdout)
    except BaseException:
        book.native_leak=True
        raise
    if gate.get('passed') is not True:book.native_leak=True
    if gate.get('cleanup_clean' if cleanup else 'safe_to_copy') is not True:
        raise ValueError('native relay sanitizer refused '+('cleanup' if cleanup else 'evidence copy'))
    return gate


def fetch_evidence(box, remote, rb, remote_directory, local_directory, *, book=None):
    remote_directory, local_directory = Path(remote_directory), Path(local_directory)
    gate=sanitize_remote(box,remote,rb,remote_directory,book) if book is not None else None
    local_directory.mkdir(parents=True, exist_ok=True)
    archive = remote_directory/'.acceptance-evidence.tar'
    manifest = remote_directory/'.acceptance-evidence.json'
    remote.ssh(f'python {rb.ps_quote(box.repo + "/tools/acceptance_remote.py")} --pack {rb.ps_quote(remote_directory.as_posix())} '
               f'--archive {rb.ps_quote(archive.as_posix())} --manifest {rb.ps_quote(manifest.as_posix())}', timeout=900)
    local_archive = local_directory/'.acceptance-evidence.tar'
    local_manifest = local_directory/'.acceptance-evidence.json'
    remote.scp_from(archive, local_archive)
    remote.scp_from(manifest, local_manifest)
    expected = read(local_manifest)
    unpack_evidence(local_archive, local_directory, expected)
    if gate is not None:write(local_directory/'native-secret-scan.json',gate)
    return expected


def unpack_evidence(archive, destination, expected):
    local_directory = Path(destination)
    with tarfile.open(archive, 'r') as bundle:
        for member in bundle.getmembers():
            target = (local_directory/member.name).resolve()
            if not member.isfile() or not target.is_relative_to(local_directory.resolve()):
                raise ValueError('remote evidence archive contains a non-file or escaping path')
        bundle.extractall(local_directory, filter='data')
    for relative, digest in expected.items():
        path = (local_directory/relative).resolve()
        if not path.is_relative_to(local_directory.resolve()) or sha256(path) != digest:
            raise ValueError(f'fetched evidence hash differs: {relative}')


def task_payload(box, remote, rb, root, payload, work, *, wait=True):
    root, work = Path(root), Path(work)
    work.mkdir(parents=True, exist_ok=True)
    remote.mkdir(root)
    spec = work/'payload.json'
    write(spec, payload)
    remote.scp_to(spec, root/'payload.json')
    log, done = root/'driver.log', root/'driver.done'
    argv = [box.repo + '/tools/acceptance_remote.py', '--payload', (root/'payload.json').as_posix()]
    import run_split
    script = work/'session.ps1'
    script.write_text(rb.render_payload(box.repo, argv, log, done, run_split.runner_environment(box), box.path_prepend), encoding='utf-8')
    remote.start_task(script, budget_s=180 * 60)
    if not wait:
        return done
    ended = remote.wait_done(done, payload['timeout'] + 120)
    match = re.search(r'\brc=(-?\d+)', ended)
    if not match:
        raise RuntimeError(f'{box.name}: session task has no terminal exit receipt: {ended}')
    return int(match[1])


def run_command(share, command_id, repo, here, commands, log):
    spec = share.rows[command_id]
    split, _ = inventory_modules(here.parent)
    boxes, _ = split.load_manifest(here.parent/'boxes.json')
    box = next(box for box in boxes if box.name == spec['box'])
    if box.kind != 'windows-task':
        raise ValueError(f'{box.name}: the remote row requires its Windows session task')
    box, remote, rb, remote_root = prepare_box(repo, box, share.root, here.parent, share.source, share.schedule['exe_sha256'])
    own = remote_root/share.label/command_id
    local = share.root/share.label/command_id
    payload = dict(kind='driver', box=box.name, source_sha=share.source, collection_id=share.run_id,
        repo=box.repo, executable=box.exe, exe_sha256=share.schedule['exe_sha256'],
        commands=remote_commands(commands, repo, box.repo, share.root, remote_root),
        identity=(own/'identity.json').as_posix(), timeout=max(60, int(spec['minutes']['cap'] * 60)))
    code = task_payload(box, remote, rb, own, payload, share.receipt_dir(command_id)/'remote')
    fetch_evidence(box, remote, rb, own, local)
    native_log = local/'driver.log'
    log.write_bytes(native_log.read_bytes() if native_log.is_file() else f'{box.name}: task exit={code}\n'.encode())
    return code


def execute_payload(path):
    spec = read(path)
    if spec.get('kind') not in ('driver','relay-peer','relay-selftest','capture-peer'):
        raise ValueError('unknown remote driver payload')
    repo = Path(spec['repo'])
    source = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip()
    if source != spec['source_sha'] or sha256(Path(spec['executable'])) != spec['exe_sha256']:
        raise ValueError('remote payload source/executable differs before execution')
    from acceptance_identity import identity
    native = identity(spec['box'], repo, Path(spec['executable']), repo/'tools/cross_peers/build.json', source, spec['collection_id'])
    write(spec['identity'], native)
    if native.get('status') != 'PASS':
        raise ValueError('remote identity refused: ' + '; '.join(native.get('errors', [])))
    if spec['kind'] != 'driver':
        from acceptance_peer_session import execute
        result = execute(spec)
        if sha256(Path(spec['executable'])) != spec['exe_sha256']:
            raise ValueError('remote executable changed while the peer ran')
        return result
    result = 0
    deadline = time.monotonic() + spec['timeout']
    for argv in spec['commands']:
        if CREDENTIAL_FLAGS & set(argv):
            raise ValueError('credential files may only be read by the box-a driver')
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError('declared remote row budget exhausted')
        environment = spec.get('environment', {})
        if set(environment) - {'HOST_ADDRESS'}:
            raise ValueError('remote driver environment is not declared')
        process = subprocess.run(argv, cwd=repo, env=dict(os.environ, CCCP_HEADLESS='1', PYTHONDONTWRITEBYTECODE='1', **environment),
                                 timeout=remaining, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        if process.returncode:
            result = process.returncode
            break
    if sha256(Path(spec['executable'])) != spec['exe_sha256']:
        raise ValueError('remote executable changed while the row ran')
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--payload', type=Path)
    parser.add_argument('--pack', type=Path)
    parser.add_argument('--archive', type=Path)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--broker')
    parser.add_argument('--snapshot', type=Path)
    parser.add_argument('--publish', type=Path)
    parser.add_argument('--relay-sanitize',type=Path)
    parser.add_argument('--relay-cleanup',type=Path)
    options = parser.parse_args(argv)
    if options.relay_sanitize:
        print(json.dumps(sanitize_native(options.relay_sanitize,json.load(sys.stdin))));return 0
    if options.relay_cleanup:
        print(json.dumps(sanitize_native(options.relay_cleanup,json.load(sys.stdin),cleanup=True)));return 0
    if options.broker:
        from acceptance_peer_session import broker
        return broker(options.broker)
    if options.snapshot:
        from acceptance_peer_session import snapshot
        print(json.dumps(snapshot(options.snapshot))); return 0
    if options.publish:
        from acceptance_peer_session import publish
        publish(options.publish,json.load(sys.stdin)); return 0
    if options.payload:
        return execute_payload(options.payload)
    if options.pack and options.archive and options.manifest:
        write(options.manifest, pack_evidence(options.pack, options.archive))
        return 0
    parser.error('pass --payload, or --pack with --archive and --manifest')


if __name__ == '__main__':
    raise SystemExit(main())
