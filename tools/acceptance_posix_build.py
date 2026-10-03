"""Build committed acceptance sources in a fresh owned POSIX snapshot."""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import secrets
import shutil
import subprocess
import sys
import tarfile
import time


def digest(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as source:
        for data in iter(lambda: source.read(1024**2), b''): result.update(data)
    return result.hexdigest()


def stamp():
    return dt.datetime.now(dt.timezone(dt.timedelta(hours=-7))).strftime('%Y-%m-%d %I:%M:%S %p MST')


def write(path, value):
    with Path(path).open('x', encoding='utf-8') as stream:
        json.dump(value, stream, indent=2); stream.write('\n')


def retained_bytes(root):
    result, pending = 0, [Path(root)]
    while pending:
        for path in pending.pop().iterdir():
            if path.is_symlink(): continue
            if path.is_dir(): pending.append(path)
            elif path.is_file(): result += path.stat().st_size
    return result


def busy():
    raw = subprocess.check_output(['ps', '-axo', 'pid=,comm='], text=True)
    return [line.strip() for line in raw.splitlines() if re.search(r'CortexCommand|cc1plus|ninja|clang|g\+\+', line)]


def export_commit(cache, commit, destination):
    if not re.fullmatch(r'[0-9a-f]{40}', commit): raise ValueError('full committed source SHA required')
    destination.mkdir(exist_ok=False)
    inputs, links = {}, []
    with subprocess.Popen(['git', '-C', str(cache), 'archive', '--format=tar', commit], stdout=subprocess.PIPE) as process:
        with tarfile.open(fileobj=process.stdout, mode='r|') as archive:
            for member in archive:
                relative = PurePosixPath(member.name)
                if relative.is_absolute() or '..' in relative.parts or member.islnk():
                    raise ValueError('committed snapshot contains an unsupported archive path')
                target = destination/relative
                if member.isdir(): target.mkdir(parents=True, exist_ok=True)
                elif member.issym():
                    if PurePosixPath(member.linkname).is_absolute(): raise ValueError('absolute committed symlink')
                    links.append((member.name, member.linkname))
                elif member.isfile():
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with target.open('xb') as output: shutil.copyfileobj(archive.extractfile(member), output)
                    target.chmod(member.mode)
                    inputs[member.name] = digest(target)
                else: raise ValueError('unsupported committed archive entry')
        if process.wait(timeout=120): raise RuntimeError('git archive failed')
    for name, target_name in links:
        target = destination/name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.symlink_to(target_name)
        if not target.resolve().is_relative_to(destination.resolve()):
            raise ValueError('committed symlink leaves its snapshot')
        inputs[name] = dict(symlink=target_name)
    return inputs


def build_command(arguments, cwd, env, log, scratch):
    with log.open('xb') as stream:
        process = subprocess.Popen(arguments, cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=stream, stderr=subprocess.STDOUT)
        deadline = time.monotonic()+2700
        try:
            while process.poll() is None:
                if time.monotonic() > deadline: raise TimeoutError('native build step exceeded 45 minutes')
                if retained_bytes(scratch) >= 3_750_000_000:
                    raise RuntimeError('native build approached the 4 GB retained scratch guard')
                time.sleep(5)
        except BaseException:
            process.terminate()
            process.wait(timeout=60)
            raise
        if process.returncode: raise RuntimeError(f'native build step exited {process.returncode}; retained log {log.name}')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lane', required=True)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--commit', required=True)
    parser.add_argument('--runtime-data-commit')
    parser.add_argument('--out', type=Path, required=True)
    options = parser.parse_args(argv)
    if platform.system() not in ('Darwin', 'Linux'):
        raise ValueError('this builder never builds on Windows')
    base = Path('/Users/erol/cortex-workers' if platform.system() == 'Darwin' else '/home/erol/cortex-workers')
    scratch = base/options.lane
    if not re.fullmatch(r'[A-Za-z0-9_-]+', options.lane) or not options.out.resolve().is_relative_to(scratch.resolve()) or options.out.resolve() == scratch.resolve():
        raise ValueError('native build output must stay inside its owned lane')
    if retained_bytes(scratch)+700_000_000 >= 4_000_000_000:
        raise RuntimeError('insufficient retained source/build headroom')
    marker = base/'ACCEPTANCE-STREAM-RUNNING'
    token = secrets.token_hex(24)
    claim = dict(token=token, pid=os.getpid(), stream_root=str(options.out), stamp=stamp())
    if marker.exists() or busy(): raise RuntimeError('native box is occupied; no source staged or compiler started')
    write(marker, claim)
    try:
        if busy(): raise RuntimeError('another native build or engine appeared before staging')
        options.out.mkdir(parents=True, exist_ok=False)
        write(options.out/'reservation.json', {key:value for key,value in claim.items() if key != 'token'})
        repo = options.out/'repo'
        inputs = export_commit(options.cache, options.commit, repo)
        write(options.out/'committed-inputs.json', dict(commit=options.commit, files=inputs))
        print(f'{stamp()} committed source exported: {len(inputs)} files', flush=True)
        data_changes = {}
        if options.runtime_data_commit:
            if not re.fullmatch(r'[0-9a-f]{40}', options.runtime_data_commit): raise ValueError('full runtime-content commit required')
            names = subprocess.check_output(['git','-C',str(options.cache),'diff','--name-only', options.commit, options.runtime_data_commit,'--','Data'], text=True).splitlines()
            for name in names:
                raw = subprocess.check_output(['git','-C',str(options.cache),'show',options.runtime_data_commit+':'+name])
                path = repo/name
                if path.exists():
                    backup = options.out/'committed-content-before'/name
                    backup.parent.mkdir(parents=True, exist_ok=True)
                    with backup.open('xb') as output: output.write(path.read_bytes())
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(raw)
                data_changes[name] = hashlib.sha256(raw).hexdigest()
            write(options.out/'runtime-content-overlay.json', dict(commit=options.runtime_data_commit, files=data_changes))
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', CCCP_HEADLESS='1')
        env['PATH'] = '/opt/homebrew/bin:/usr/local/bin:'+str(Path.home()/'.local/bin')+':'+env.get('PATH','')
        setup = ['meson','setup',str(repo/'build-gcc'),str(repo),'--buildtype=release','-Db_lto=false',
                 '-Db_pch=false','-Dcpp_std=c++20','-Dwith_gns=enabled','--wrap-mode=nodownload']
        if platform.system() == 'Darwin':
            deps = '/Users/erol/projects/cccp/deps-audit-20260907'
            env.update(CC='/opt/homebrew/bin/gcc-13', CXX='/opt/homebrew/bin/g++-13',
                       PKG_CONFIG_PATH=deps+'/protobuf/lib/pkgconfig:/opt/homebrew/opt/openssl@3/lib/pkgconfig')
            setup += ['-Dgns_root='+deps+'/gns']
        else:
            env.update(CC='gcc', CXX='g++')
            setup += ['-Dgns_root=/home/erol/deps/gns']
        write(options.out/'commands.json',dict(setup=setup, build=['ninja','-C',str(repo/'build-gcc'),'-j6'],
                                               compiler_environment={key:env[key] for key in ('CC','CXX')}))
        build_command(setup, repo, env, options.out/'setup.log', scratch)
        build_command(['ninja','-C',str(repo/'build-gcc'),'-j6'], repo, env, options.out/'build.log', scratch)
        for name, expected in inputs.items():
            if isinstance(expected, dict):
                if not (repo/name).is_symlink() or os.readlink(repo/name) != expected['symlink']:
                    raise RuntimeError('committed symlink changed while building: '+name)
                continue
            if name in data_changes: expected = data_changes[name]
            if digest(repo/name) != expected: raise RuntimeError('tracked input changed while building: '+name)
        exe = repo/'build-gcc/CortexCommand'
        receipt = dict(commit=options.commit, executable_sha256=digest(exe), configuration='release', build_exit_code=0,
                       build_log=str(options.out/'build.log'), build_log_sha256=digest(options.out/'build.log'),
                       committed_inputs_sha256=digest(options.out/'committed-inputs.json'),
                       runtime_content_commit=options.runtime_data_commit, finished=stamp())
        write(options.out/'build.json',receipt)
        print(json.dumps(receipt), flush=True)
        return 0
    finally:
        if marker.is_file() and json.loads(marker.read_text()).get('token') == token:
            marker.unlink()
            if options.out.is_dir(): write(options.out/'reservation-released.json',dict(released=True, measured=stamp()))


if __name__ == '__main__':
    raise SystemExit(main())
