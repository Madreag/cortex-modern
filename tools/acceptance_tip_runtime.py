"""Install a verified Ally build only in the NOTE 8 engineer-tip row trees."""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import secrets
import shutil
import stat
import subprocess
import tarfile

from acceptance_runtime import retained_bytes

RUNTIME = ('Cortex Command.exe', 'Cortex Command.pdb', 'fmod.dll',
           'libprotobuf.dll', 'abseil_dll.dll', 'libcrypto-3-x64.dll')
SUPPORT = ('Source/Managers/SettingsMan.h', 'tools/feel/CrossCombat.lua', 'tools/feel/CrossCombat.ini')


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write(path, value):
    with Path(path).open('x', encoding='utf-8') as stream:
        json.dump(value, stream, indent=2); stream.write('\n')


def quote(value):
    return "'"+str(value).replace("'", "''")+"'"


def powershell(code):
    return subprocess.check_output(['pwsh', '-NoProfile', '-NonInteractive', '-Command', code],
                                   text=True, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0)).strip()


def destination(box, lane, hostname=None):
    expected = {'Z13': 'EROL-TABLET', 'EDITH': 'EDITH'}
    if box not in expected or (hostname or platform.node()).casefold() != expected[box].casefold():
        raise ValueError('runtime installer is on the wrong physical box')
    if not lane or any(character not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-' for character in lane):
        raise ValueError('invalid lane')
    return Path('D:/Projects/z13-rows-build' if box == 'Z13' else f'D:/mx/{lane}/engine-tip')


def quiet():
    state = json.loads(powershell("[pscustomobject]@{task=(Get-ScheduledTask -TaskName cortex-session1).State.ToString();"
        "load=@(Get-CimInstance Win32_Process | Where-Object {$_.Name -match '^(Cortex Command.*|cl|link|MSBuild)\\.exe$'} "
        "| Select-Object ProcessId,ExecutablePath)} | ConvertTo-Json -Depth 4 -Compress"))
    if state['task'] != 'Ready' or state['load']:
        raise RuntimeError('physical task or engine/build is occupied; no runtime files written')
    return state


def guarded_exes(box):
    trees = ('D:/Projects/z13-build', 'D:/Projects/z13-dev-build') if box == 'Z13' else ('D:/Projects/inventory-build',)
    return {tree: digest(Path(tree)/'Cortex Command.exe') for tree in trees}


def install(options):
    target = destination(options.box, options.lane)
    scratch = Path('D:/mx')/options.lane
    root = options.out.resolve()
    if not root.is_relative_to(scratch.resolve()) or root == scratch.resolve():
        raise ValueError('shipping evidence must remain inside the lane')
    if root.exists(): raise FileExistsError('select a fresh retained shipping root')
    manifest = json.loads(options.manifest.read_text(encoding='utf-8-sig'))
    if digest(options.archive) != manifest['archive_sha256']:
        raise ValueError('archive digest differs from native Ally package')
    if manifest['build']['executable_sha256'] != manifest['files']['Cortex Command.exe']:
        raise ValueError('native build receipt differs from packaged executable')
    expected = set(RUNTIME+SUPPORT+('build.log',))
    if set(manifest['files']) != expected: raise ValueError('unexpected runtime package member set')
    with tarfile.open(options.archive) as archive:
        members = archive.getmembers()
        if len(members) != len(expected) or {member.name for member in members} != expected or any(not member.isfile() for member in members):
            raise ValueError('runtime package must contain exactly the ten ordinary files')
        for member in members:
            relative = PurePosixPath(member.name)
            if relative.is_absolute() or '..' in relative.parts: raise ValueError('unsafe archive path')
        uncompressed = sum(member.size for member in members)
    old_bytes = sum((target/name).stat().st_size for name in RUNTIME if (target/name).is_file()) if target.exists() else 0
    growth = uncompressed*2+old_bytes+4*1024**2
    if retained_bytes(scratch)+growth >= 4_000_000_000:
        raise RuntimeError('runtime installation lacks retained scratch headroom')
    quiet()
    if target.exists() and (target.is_symlink() or target.lstat().st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT):
        raise ValueError('runtime tree itself must not be a link')
    if options.box == 'EDITH' and target.exists(): raise FileExistsError('EDITH engineer-tip tree must be fresh')
    marker, token = Path('D:/mx/FEEL-MATRIX-RUNNING'), secrets.token_hex(24)
    write(marker, dict(token=token, pid=os.getpid(), stream_root=str(root), purpose='verified row runtime installation'))
    try:
        state = quiet()
        root.mkdir(parents=True)
        before = guarded_exes(options.box)
        write(root/'before.json', dict(guarded_exes=before, native_state=state, scratch_bytes=retained_bytes(scratch), admitted_growth=growth))
        incoming = root/'incoming'; incoming.mkdir()
        with tarfile.open(options.archive) as archive:
            for member in archive:
                path = incoming/member.name; path.parent.mkdir(parents=True, exist_ok=True)
                with path.open('xb') as stream: shutil.copyfileobj(archive.extractfile(member), stream)
                if digest(path) != manifest['files'][member.name]: raise ValueError('extracted runtime hash differs')
        target.mkdir(parents=True, exist_ok=True)
        names = RUNTIME if options.box == 'Z13' else RUNTIME+SUPPORT
        for name in names:
            path, source = target/name, incoming/name
            path.parent.mkdir(parents=True, exist_ok=True)
            if path.is_file():
                backup = root/'before'/name; backup.parent.mkdir(parents=True, exist_ok=True)
                with backup.open('xb') as stream, path.open('rb') as previous: shutil.copyfileobj(previous, stream)
                if digest(backup) != digest(path): raise ValueError('retained previous runtime differs')
            if options.box == 'Z13':
                # Replace the directory entry outside D:/mx, detaching any old
                # hard link without modifying its other runtime-tree entries.
                staged = path.with_name(path.name+'.staged-'+token)
                with staged.open('xb') as stream, source.open('rb') as raw: shutil.copyfileobj(raw, stream)
                os.replace(staged, path)
            else:
                with path.open('xb') as stream, source.open('rb') as raw: shutil.copyfileobj(raw, stream)
            if digest(path) != manifest['files'][name]: raise ValueError('installed runtime hash differs')
        if options.box == 'EDITH':
            powershell('New-Item -ItemType Junction -Path '+quote(target/'Data')+
                       " -Target 'D:/Projects/inventory-build/Data' | Out-Null")
        build = {**manifest['build'], 'build_log':str(incoming/'build.log')}
        write(root/'build.json', build)
        after = guarded_exes(options.box)
        if before != after: raise ValueError('protected runtime executable changed during installation')
        write(root/'installed.json', dict(box=options.box, target=str(target), source_sha=manifest['source_sha'],
              runtime_files={name:digest(target/name) for name in names}, protected_exes_unchanged=True,
              archive_sha256=digest(options.archive), build_receipt=str(root/'build.json'), removed_files=0,
              finished=dt.datetime.now(dt.timezone(dt.timedelta(hours=-7))).strftime('%Y-%m-%d %I:%M:%S %p MST')))
        print(json.dumps(dict(box=options.box, source_sha=build['commit'], executable_sha256=build['executable_sha256'],
                              target=str(target), protected_exes_unchanged=True)))
    finally:
        if marker.is_file() and json.loads(marker.read_text()).get('token') == token:
            marker.unlink()
            if root.is_dir(): write(root/'reservation-released.json', dict(released=True))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--box', choices=('Z13','EDITH'), required=True)
    parser.add_argument('--lane', required=True)
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    return install(parser.parse_args(argv))


if __name__ == '__main__':
    raise SystemExit(main())
