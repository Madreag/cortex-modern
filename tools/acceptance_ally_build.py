"""Stream committed sources to an owned Ally tree and use the lead's build task."""
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


def stamp():
    return dt.datetime.now(dt.timezone(dt.timedelta(hours=-7))).strftime('%Y-%m-%d %I:%M:%S %p MST')


def digest(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as source:
        for chunk in iter(lambda:source.read(1024**2), b''): value.update(chunk)
    return value.hexdigest()


def write(path, value):
    with Path(path).open('x', encoding='utf-8') as stream:
        json.dump(value, stream, indent=2); stream.write('\n')


def quote(value):
    return "'"+str(value).replace("'", "''")+"'"


def ps(command, timeout=60):
    result = subprocess.run(['pwsh','-NoProfile','-NonInteractive','-Command',command],
                            capture_output=True,text=True,timeout=timeout,
                            creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    if result.returncode: raise RuntimeError('Ally task command failed: '+result.stderr[-2000:])
    return result.stdout.strip()


def retained_bytes(root):
    total, pending = 0, [Path(root)]
    while pending:
        for path in pending.pop().iterdir():
            info = path.lstat()
            if path.is_symlink() or getattr(info,'st_file_attributes',0)&1024: continue
            if path.is_dir(): pending.append(path)
            elif path.is_file(): total += info.st_size
    return total


def payload(options):
    if sys.platform != 'win32' or platform.node().casefold() != 'erol-ally7':
        raise ValueError('build payload is authorized only on EROL-ALLY7; no local build')
    scratch = Path('D:/mx')/options.lane
    own = options.out.resolve()
    if not own.is_relative_to(scratch.resolve()) or own == scratch.resolve():
        raise ValueError('Ally source/build output must stay inside this lane')
    initial_bytes = retained_bytes(scratch)
    if initial_bytes+3_400_000_000 >= 4_000_000_000:
        raise RuntimeError('insufficient Ally retained-build headroom')
    if not re.fullmatch(r'[0-9a-f]{40}',options.commit): raise ValueError('full committed source SHA required')
    state = ps('[string](Get-ScheduledTask -TaskName cortex-build).State')
    load = ps("@(Get-CimInstance Win32_Process|Where-Object{$_.Name -match '^(Cortex Command.*|cl|link|MSBuild)\\.exe$'}).Count")
    if state != 'Ready' or load != '0': raise RuntimeError('Ally native build task or engine is occupied')
    marker = Path('D:/mx/FEEL-MATRIX-RUNNING')
    claim = dict(token=secrets.token_hex(24), pid=os.getpid(), stream_root=str(own),stamp=stamp())
    write(marker,claim)
    task_changed = False
    try:
        own.mkdir(parents=True,exist_ok=False)
        write(own/'reservation.json',{key:value for key,value in claim.items() if key!='token'})
        original_task = ps('Export-ScheduledTask -TaskName cortex-build')
        (own/'cortex-build-before.xml').write_text(original_task,encoding='utf-8')
        received = 0
        with tarfile.open(fileobj=sys.stdin.buffer,mode='r|') as archive:
            for member in archive:
                relative = PurePosixPath(member.name)
                if relative.is_absolute() or '..' in relative.parts or not member.isfile():
                    raise ValueError('unsafe committed input archive member')
                received += member.size
                if received > 800_000_000 or initial_bytes+received >= 4_000_000_000:
                    raise RuntimeError('source transfer would exceed retained storage admission')
                path = own/relative
                path.parent.mkdir(parents=True,exist_ok=True)
                with path.open('xb') as output: shutil.copyfileobj(archive.extractfile(member),output)
        manifest = json.loads((own/'committed-inputs.json').read_text(encoding='utf-8'))
        if manifest['commit'] != options.commit: raise RuntimeError('source manifest differs from the requested committed tip')
        for name, entry in manifest['files'].items():
            if digest(own/'repo'/name) != entry['sha256']: raise RuntimeError('shipped source differs: '+name)
        if digest(own/'ally_build_task.ps1') != manifest['build_task_sha256']:
            raise RuntimeError('preserving build-task script differs')
        print(stamp()+' committed Ally source verified; starting cortex-build',flush=True)
        build = own/'build'
        build.mkdir()
        command = "$ErrorActionPreference='Stop'; "
        arguments = '-NoProfile -ExecutionPolicy Bypass -File "'+str(own/'ally_build_task.ps1')+'" -Tree "'+str(own/'repo')+'" -Out "'+str(build)+'" -Target RTEA -MP 8 -VCToolsVersion 14.50.35717'
        command += "$a=New-ScheduledTaskAction -Execute 'C:\\Program Files\\PowerShell\\7\\pwsh.exe' -Argument "+quote(arguments)+'; '
        command += '$p=New-ScheduledTaskPrincipal -UserId ($env:COMPUTERNAME+"\\egerm") -LogonType S4U -RunLevel Highest; '
        command += '$s=New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Minutes 40) -MultipleInstances IgnoreNew; '
        command += 'Register-ScheduledTask -TaskName cortex-build -Action $a -Principal $p -Settings $s -Force | Out-Null; Start-ScheduledTask -TaskName cortex-build'
        ps(command)
        task_changed = True
        deadline = time.monotonic()+2400
        while not (build/'build.exit').is_file():
            if time.monotonic()>deadline: raise TimeoutError('Ally build did not publish its exit within 40 minutes')
            if retained_bytes(scratch)>=3_900_000_000:
                raise RuntimeError('Ally build reached its retained-output stop; task remains owned until it ends')
            time.sleep(5)
        code = int((build/'build.exit').read_text(encoding='utf-8-sig').strip())
        if code: raise RuntimeError('Ally native build exited '+str(code)+'; original log retained')
        for name, entry in manifest['files'].items():
            if digest(own/'repo'/name) != entry['sha256']: raise RuntimeError('committed build input changed: '+name)
        exe = own/'repo/Cortex Command.exe'
        runtime = [exe,*sorted((own/'repo').glob('*.dll')),*sorted((own/'repo').glob('*.pdb'))]
        if not any(path.suffix.lower()=='.pdb' for path in runtime): raise RuntimeError('native build produced no requested PDB')
        result = json.loads((build/'build.json').read_text(encoding='utf-8-sig'))
        if digest(exe) != result['exe_sha256']: raise RuntimeError('native build executable changed after its task receipt')
        receipt = dict(commit=options.commit,executable_sha256=digest(exe),configuration='Final',build_exit_code=0,
                       build_log=str(build/'build.log'),build_log_sha256=digest(build/'build.log'),
                       native_task_result=result,committed_inputs_sha256=digest(own/'committed-inputs.json'),
                       runtime_files={path.name:digest(path) for path in runtime},finished=stamp())
        write(own/'build-receipt.json',receipt)
        print(json.dumps({key:receipt[key] for key in ('commit','executable_sha256','configuration','build_exit_code','finished')}),flush=True)
        return 0
    finally:
        if task_changed:
            # A build task is never replaced while still running. On an error,
            # retain the claim until its native task has ended.
            deadline = time.monotonic()+2400
            while ps('[string](Get-ScheduledTask -TaskName cortex-build).State') == 'Running':
                if time.monotonic()>deadline:
                    raise RuntimeError('Ally build task is still active; reservation retained for inspection')
                time.sleep(5)
            original = own/'cortex-build-before.xml'
            ps('Register-ScheduledTask -TaskName cortex-build -Xml (Get-Content -LiteralPath '+quote(original)+' -Raw) -Force | Out-Null')
        if marker.is_file() and json.loads(marker.read_text()).get('token')==claim['token']:
            marker.unlink()
            if own.is_dir(): write(own/'reservation-released.json',dict(released=True,measured=stamp()))


def source_entries(repo, commit):
    raw = subprocess.check_output(['git','-C',str(repo),'ls-tree','-rlz',commit])
    result = []
    for row in raw.split(b'\0'):
        if not row: continue
        head,name = row.split(b'\t',1)
        mode,kind,oid,size = head.split()
        if kind != b'blob' or mode not in (b'100644',b'100755',b'120000'):
            raise ValueError('committed source includes an unsupported object')
        result.append((name.decode('utf-8'),mode.decode(),oid.decode(),int(size)))
    return result


def coordinator(options):
    from acceptance_runtime import storage_scope,write_json,write_text
    from edith.remote_box import RemoteBox
    scratch = Path('D:/mx')/options.lane
    if not options.out.resolve().is_relative_to(scratch.resolve()) or options.out.resolve()==scratch.resolve():
        raise ValueError('coordinator evidence must stay inside the named scratch')
    if not re.fullmatch(r'[0-9a-f]{40}',options.commit): raise ValueError('full committed source SHA required')
    entries = source_entries(options.repo,options.commit)
    script = options.template.read_text(encoding='utf-8-sig')
    removal = "Remove-Item -LiteralPath (Join-Path $Out 'build.exit') -ErrorAction SilentlyContinue"
    if script.count(removal) != 1: raise ValueError('lead build template changed; preserving adaptation needs review')
    script = script.replace(removal,"if (Test-Path -LiteralPath (Join-Path $Out 'build.exit')) { throw 'Build output already exists; retain it and use a fresh root' }")
    script = script.replace('Get-Date -Format s',"Get-Date -Format 'yyyy-MM-dd hh:mm:ss tt'")
    task = script.encode('utf-8')
    remote_root = 'D:/mx/'+options.lane+'/'+options.out.name
    with storage_scope(scratch,reserve=64*1024**2):
        options.out.mkdir(parents=True,exist_ok=False)
        remote = RemoteBox('ally')
        remote.mkdir('D:/mx/'+options.lane)
        helper = 'D:/mx/'+options.lane+'/'+options.out.name+'-payload.py'
        remote.scp_to(Path(__file__),helper)
        command = ['ssh','ally','python '+quote(helper)+' --payload --lane '+quote(options.lane)+' --commit '+quote(options.commit)+' --out '+quote(remote_root)]
        manifest = dict(commit=options.commit,build_task_sha256=hashlib.sha256(task).hexdigest(),files={},
                        materialization='committed blobs; Git symlinks kept as their blob bytes on Windows')
        with subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
                              creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0)) as process:
            with subprocess.Popen(['git','-C',str(options.repo),'cat-file','--batch'],stdin=subprocess.PIPE,stdout=subprocess.PIPE) as blobs:
                try:
                    with tarfile.open(fileobj=process.stdin,mode='w|') as archive:
                        for name,mode,oid,size in entries:
                            blobs.stdin.write((oid+'\n').encode()); blobs.stdin.flush()
                            header = blobs.stdout.readline().split()
                            if header != [oid.encode(),b'blob',str(size).encode()]: raise RuntimeError('git blob response differs')
                            raw = blobs.stdout.read(size)
                            if len(raw)!=size or blobs.stdout.read(1)!=b'\n': raise RuntimeError('git blob was truncated')
                            member = tarfile.TarInfo('repo/'+name); member.size=size; member.mode=0o644
                            archive.addfile(member,io.BytesIO(raw))
                            manifest['files'][name]=dict(git_blob=oid,git_mode=mode,bytes=size,sha256=hashlib.sha256(raw).hexdigest())
                        write_json(options.out/'committed-inputs.json',manifest)
                        write_text(options.out/'ally_build_task.ps1',script)
                        options.evidence.mkdir(parents=True,exist_ok=False)
                        for path in (options.out/'committed-inputs.json',options.out/'ally_build_task.ps1'):
                            destination=options.evidence/path.name
                            with destination.open('xb') as output: output.write(path.read_bytes())
                            if digest(path)!=digest(destination): raise RuntimeError('pre-build proof copy differs')
                        write_json(options.evidence/'before-build.json',dict(commit=options.commit,blob_count=len(entries),
                                   helper_sha256=digest(Path(__file__)),native_builds_started=0,coordinator_builds=0,measured=stamp()))
                        for name,raw in [('ally_build_task.ps1',task),('committed-inputs.json',(options.out/'committed-inputs.json').read_bytes())]:
                            member=tarfile.TarInfo(name);member.size=len(raw);member.mode=0o644
                            archive.addfile(member,io.BytesIO(raw))
                finally:
                    blobs.stdin.close(); blobs.wait(timeout=60)
            process.stdin.close()
            # The native task's full log stays on the Ally. This bounded receipt
            # stream contains only staging/build state and the final build facts.
            output=process.stdout.read(4*1024**2+1)
            if len(output)>4*1024**2: raise RuntimeError('unexpectedly large native builder response')
            code=process.wait(timeout=3000)
            write_text(options.out/'coordinator.log',output.decode('utf-8',errors='replace'))
            print(output.decode('utf-8',errors='replace'),flush=True)
            return code


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--payload',action='store_true')
    parser.add_argument('--lane',required=True)
    parser.add_argument('--commit',required=True)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--repo',type=Path)
    parser.add_argument('--template',type=Path)
    parser.add_argument('--evidence',type=Path)
    options=parser.parse_args(argv)
    if not re.fullmatch(r'[A-Za-z0-9_-]+',options.lane): parser.error('unsafe lane name')
    if options.payload: return payload(options)
    if not options.repo or not options.template or not options.evidence: parser.error('--repo, --template and --evidence are required')
    return coordinator(options)


if __name__=='__main__':
    raise SystemExit(main())
