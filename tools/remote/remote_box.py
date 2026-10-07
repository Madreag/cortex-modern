"""A second Windows box reached over ssh, whose engine runs only inside the owner's interactive session through one
scheduled task (REMOTE's cortex-session1). Shared by tools/two_box_match.py and the inventory's split runner.

    python tools/remote/remote_box.py hash --root <dir> --list <file>      sha256 of each listed relative path (JSON)
    python tools/remote/remote_box.py evidence --root <dir> --out <list> [--max-bytes N] [--suffix .json ...]
    python tools/remote/remote_box.py --self-test

The two subcommands run ON the box: `hash` answers a ship-by-hash question, `evidence` writes the list of small files
under a run root (never entering a junction or symlink) for one tar. Everything else runs here and speaks ssh/scp.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import logging
import os
import re
import stat
import subprocess
import sys
import tempfile
import time
import uuid
from pathlib import Path, PureWindowsPath
from typing import Callable, Iterable

LOG = logging.getLogger('remote_box')
SSH_NOISE = re.compile(r'post-quantum|store now, decrypt later|may need to be upgraded|openssh\.com/pq', re.I)
WINDOWS_TAR = 'tar.exe'
PAYLOAD_TEMPLATE = Path(__file__).resolve().parent / 'session_command.ps1'
EVIDENCE_SUFFIXES = ('.json', '.log', '.txt', '.md')
EVIDENCE_MAX_BYTES = 4 << 20
REPARSE = getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0x400)
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


def ps_quote(value: object) -> str:
    """A PowerShell single-quoted literal."""
    return "'" + str(value).replace("'", "''") + "'"


def session_wrapper(floor: float) -> str:
    """The installed native session runner, with its catalog memory floor."""
    floor = format(float(floor), 'g')
    return ("$ErrorActionPreference='Stop'\n"
            "$root=Split-Path -Parent $PSCommandPath\n"
            "$env:CCCP_HEADLESS='1'\n"
            "$free=(Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory/1MB\n"
            f"if($free -lt {floor}) {{\n"
            f"  @{{exit_code=97;reason='native memory floor';free_gb=$free;floor_gb={floor};finished=[DateTimeOffset]::UtcNow.ToUnixTimeSeconds()}} | ConvertTo-Json -Compress | Set-Content -Encoding utf8 -LiteralPath \"$root/last-result.json\"\n"
            "  exit 97\n}\n"
            "& pwsh.exe -NoProfile -NonInteractive -File \"$root/command.ps1\"\n"
            "exit $LASTEXITCODE\n")


def wrapper_refusal(box: dict, slot: dict, *, data: bytes | None = None) -> str | None:
    """Authenticate a preserved wrapper; mentioning command.ps1 is insufficient."""
    if not slot.get('preserve_runner'):
        return None
    reason = f"FOREIGN TASK WRAPPER: {box['name']} {slot['task']} {slot['wrapper']}"
    try:
        if data is None:
            path = Path(slot['wrapper'])
            metadata = path.stat(follow_symlinks=False)
            if (path.is_symlink() or getattr(path, 'is_junction', lambda:False)()
                    or getattr(metadata, 'st_file_attributes', 0) & REPARSE or metadata.st_size > 65536):
                return reason
            data = path.read_bytes()
        if expected := slot.get('wrapper_sha256'):
            return None if hashlib.sha256(data).hexdigest().lower() == expected.lower() else reason
        text = data.decode('utf-8-sig').replace('\r\n', '\n')
        floor = box.get('free_floor_gb')
        if floor is None:
            match = re.search(r'^if\(\$free -lt (\d+(?:\.\d+)?)\) \{$', text, re.M)
            if match is None:
                return reason
            floor = float(match[1])
        return None if text.rstrip('\n') == session_wrapper(floor).rstrip('\n') else reason
    except (OSError, UnicodeError, ValueError):
        return reason


def task_restore_script(slot: dict, token: str, *, abort: bool = False) -> str:
    """Restore only this token's command. The preserved wrapper is never replaced."""
    wrapper, payload, marker = (str(slot[key]).replace('\\', '/') for key in ('wrapper', 'payload', 'owner_marker'))
    stage = str(PureWindowsPath(wrapper).parent / ('.direct-'+token)).replace('\\', '/')
    incoming = str(PureWindowsPath(wrapper).parent / ('.direct-script-'+token+'.ps1')).replace('\\', '/')
    lines = ["$ErrorActionPreference='Stop'", f"$token={ps_quote(token)}", f"$marker={ps_quote(marker)}",
             f"$stage={ps_quote(stage)}", f"$wrapper={ps_quote(wrapper)}", f"$payload={ps_quote(payload)}",
             "if (-not (Test-Path -LiteralPath $marker)) { return }",
             "$owner=Get-Content -Raw -LiteralPath $marker | ConvertFrom-Json",
             "if ($owner.token -ne $token) { throw 'DIRECT TASK OWNER CHANGED: restore refused' }"]
    if abort:
        lines += [f"$task={ps_quote(slot['task'])}",
                  "if ((Get-ScheduledTask -TaskName $task).State -eq 'Running') { Stop-ScheduledTask -TaskName $task }",
                  "$until=[DateTime]::UtcNow.AddSeconds(30)",
                  "while ((Get-ScheduledTask -TaskName $task).State -eq 'Running') { if ([DateTime]::UtcNow -ge $until) { throw 'owned task did not stop; restore deferred' }; Start-Sleep -Milliseconds 200 }"]
    lines += ["$saved=Get-Content -Raw -LiteralPath ($stage+'/saved.json') | ConvertFrom-Json",
              f"if ([IO.Path]::GetFullPath($saved.incoming) -ne [IO.Path]::GetFullPath({ps_quote(incoming)})) {{ throw 'direct payload path changed; restore refused' }}",
              "if ((Get-FileHash -Algorithm SHA256 -LiteralPath $wrapper).Hash -ne $saved.wrapper_sha256) { throw 'FOREIGN TASK WRAPPER: changed during direct payload; restore refused' }",
              "if ((Get-FileHash -Algorithm SHA256 -LiteralPath ($stage+'/wrapper.ps1')).Hash -ne $saved.wrapper_sha256) { throw 'saved wrapper hash differs; restore refused' }",
              "if ((Get-FileHash -Algorithm SHA256 -LiteralPath $payload).Hash -ne $saved.launcher_sha256) { throw 'DIRECT TASK COMMAND CHANGED: restore refused' }",
              "if ($saved.had_command) {",
              "  if ((Get-FileHash -Algorithm SHA256 -LiteralPath ($stage+'/command.ps1')).Hash -ne $saved.command_sha256) { throw 'saved command hash differs; restore refused' }",
              "  [IO.File]::WriteAllBytes(($stage+'/restore.part'), [IO.File]::ReadAllBytes($stage+'/command.ps1'))",
              "  [IO.File]::Move(($stage+'/restore.part'), $payload, $true)",
              "} else { Remove-Item -LiteralPath $payload }",
              "# run.ps1 retained its original bytes throughout, including a forced task abort.",
              "foreach ($name in @('wrapper.ps1','command.ps1','saved.json','restore.ps1')) { $path=$stage+'/'+$name; if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path } }",
              "if (Test-Path -LiteralPath $saved.incoming) { Remove-Item -LiteralPath $saved.incoming }",
              "Remove-Item -LiteralPath $stage",
              "$current=Get-Content -Raw -LiteralPath $marker | ConvertFrom-Json",
              "if ($current.token -ne $token) { throw 'DIRECT TASK OWNER CHANGED: release refused' }",
              "Remove-Item -LiteralPath $marker"]
    return '\n'.join(lines)+'\n'


def task_install_script(slot: dict, token: str, incoming: str, budget_s: float) -> str:
    """Install through command.ps1 under the existing payload mutex and marker."""
    if not re.fullmatch(r'[A-Za-z0-9-]+', token):
        raise ValueError('direct task token contains path characters')
    wrapper, payload, marker = (str(slot[key]).replace('\\', '/') for key in ('wrapper', 'payload', 'owner_marker'))
    parent = str(PureWindowsPath(wrapper).parent).replace('\\', '/')
    expected_incoming = str(PureWindowsPath(wrapper).parent / ('.direct-script-'+token+'.ps1')).replace('\\', '/')
    if str(PureWindowsPath(incoming)).casefold() != str(PureWindowsPath(expected_incoming)).casefold():
        raise ValueError('direct task payload must be its token file beside the wrapper')
    stage = parent+'/.direct-'+token
    restore = task_restore_script(slot, token)
    launcher = ("$ErrorActionPreference='Stop'\n$directExit=1\ntry {\n"
                f"  $marker={ps_quote(marker)}\n"
                "  $owner=Get-Content -Raw -LiteralPath $marker | ConvertFrom-Json\n"
                f"  if ($owner.token -ne {ps_quote(token)}) {{ throw 'DIRECT TASK OWNER CHANGED: launch refused' }}\n"
                "  $owner.pid=$PID; $owner.machine=[Environment]::MachineName\n"
                "  $owner.process_start=((Get-Process -Id $PID).StartTime.ToFileTimeUtc()-116444736000000000)/10000000.0\n"
                "  [IO.File]::WriteAllText($marker, ($owner | ConvertTo-Json -Compress), [Text.UTF8Encoding]::new($false))\n"
                f"  & pwsh.exe -NoProfile -NonInteractive -File {ps_quote(incoming)}\n"
                "  $directExit=$LASTEXITCODE\n"
                f"}} finally {{ & {ps_quote(stage+'/restore.ps1')} }}\nexit $directExit\n")
    reason = f"FOREIGN TASK WRAPPER: {slot.get('box_name', 'named box')} {slot['task']} {wrapper}"
    verify_incoming = ([] if not slot.get('payload_sha256') else [
        f"if ((Get-FileHash -Algorithm SHA256 -LiteralPath $incoming).Hash -ne {ps_quote(slot['payload_sha256'])}) {{ throw 'direct payload hash differs before launch' }}"])
    return '\n'.join([
        "$ErrorActionPreference='Stop'", f"$task={ps_quote(slot['task'])}", f"$marker={ps_quote(marker)}",
        f"$wrapper={ps_quote(wrapper)}", f"$payload={ps_quote(payload)}", f"$stage={ps_quote(stage)}",
        f"$incoming={ps_quote(incoming)}", f"$token={ps_quote(token)}", f"$lock={ps_quote(parent+'/.payload-submit.lock')}",
        f"$until=[DateTime]::UtcNow.AddSeconds({max(0, float(budget_s))})", "$installed=$false",
        *verify_incoming,
        "while (-not $installed) {",
        "  $held=$false",
        "  try {",
        "    try {",
        "      $stream=[IO.File]::Open($lock, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)",
        "      $held=$true",
        "      try {",
        "        $owner=@{token=$token;label='direct task payload';pid=$PID;machine=[Environment]::MachineName;process_start=((Get-Process -Id $PID).StartTime.ToFileTimeUtc()-116444736000000000)/10000000.0;written_at=[DateTimeOffset]::UtcNow.ToUnixTimeSeconds();lease_expires=[DateTimeOffset]::UtcNow.AddHours(2).ToUnixTimeSeconds()}",
        "        $bytes=[Text.UTF8Encoding]::new($false).GetBytes(($owner | ConvertTo-Json -Compress)); $stream.Write($bytes, 0, $bytes.Length); $stream.Flush()",
        "      } finally { $stream.Dispose() }",
        "    } catch [IO.IOException] { if (-not (Test-Path -LiteralPath $lock)) { throw } }",
        "    if ($held -and (Get-ScheduledTask -TaskName $task).State -eq 'Ready' -and -not (Test-Path -LiteralPath $marker)) {",
        f"      if ((Get-FileHash -Algorithm SHA256 -LiteralPath $wrapper).Hash -ne {ps_quote(slot['wrapper_sha256'])}) {{ throw {ps_quote(reason)} }}",
        "      if ((Get-Item -LiteralPath $wrapper).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'task wrapper is a reparse point' }",
        "      if (Test-Path -LiteralPath $stage) { throw 'direct task staging already exists' }",
        "      [IO.Directory]::CreateDirectory($stage) | Out-Null",
        "      [IO.File]::WriteAllBytes(($stage+'/wrapper.ps1'), [IO.File]::ReadAllBytes($wrapper))",
        "      $had=Test-Path -LiteralPath $payload",
        "      if ($had) {",
        "        if ((Get-Item -LiteralPath $payload).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'task command is a reparse point' }",
        "        [IO.File]::WriteAllBytes(($stage+'/command.ps1'), [IO.File]::ReadAllBytes($payload))",
        "      }",
        f"      [IO.File]::WriteAllText(($stage+'/restore.ps1'), {ps_quote(restore)}, [Text.UTF8Encoding]::new($false))",
        f"      $launcher={ps_quote(launcher)}",
        "      $saved=@{had_command=$had;wrapper_sha256=(Get-FileHash -Algorithm SHA256 -LiteralPath $wrapper).Hash;command_sha256=if($had){(Get-FileHash -Algorithm SHA256 -LiteralPath $payload).Hash}else{$null};incoming=$incoming}",
        "      [IO.File]::WriteAllText(($stage+'/launcher.part'), $launcher, [Text.UTF8Encoding]::new($false))",
        "      $saved.launcher_sha256=(Get-FileHash -Algorithm SHA256 -LiteralPath ($stage+'/launcher.part')).Hash",
        "      [IO.File]::WriteAllText(($stage+'/saved.json'), ($saved | ConvertTo-Json -Compress), [Text.UTF8Encoding]::new($false))",
        "      [IO.File]::WriteAllText($marker, ($owner | ConvertTo-Json -Compress), [Text.UTF8Encoding]::new($false))",
        "      [IO.File]::Move(($stage+'/launcher.part'), $payload, $true)",
        "      $installed=$true",
        "      Start-ScheduledTask -TaskName $task",
        "    }",
        "  } finally {",
        "    if ($held) { $owner=Get-Content -Raw -LiteralPath $lock | ConvertFrom-Json; if ($owner.token -ne $token) { throw 'DIRECT TASK MUTEX OWNER CHANGED' }; Remove-Item -LiteralPath $lock }",
        "  }",
        "  if (-not $installed) { if ([DateTime]::UtcNow -ge $until) { throw ('task payload remains busy: '+$task) }; Start-Sleep -Seconds 2 }",
        "}", "Write-Output 'DIRECT TASK INSTALLED'", ""])


class RemoteBox:
    """ssh/scp to one box plus its single session task. With dry_run every call is printed, nothing is sent."""

    def __init__(self, alias: str, task: str = 'cortex-session1', session_script: str | None = None,
                 dry_run: bool = False, say: Callable[[str], None] | None = None,
                 *, pool_registry: Path | str | None = None, preserve_runner: bool | None = None) -> None:
        self.alias = alias
        self.task = task
        self.session_script = session_script or os.environ.get('CCCP_SESSION_SCRIPT', 'session/run.ps1')
        self.dry_run = dry_run
        self.say = say or LOG.info
        self.pool_registry = pool_registry
        self.preserve_runner = preserve_runner
        self._direct_payload: tuple[dict, str] | None = None

    def catalog_slot(self) -> dict | None:
        """Read only the named box's task facts, without any placement or queue."""
        registry = self.pool_registry or os.environ.get('CORTEX_POOL_REGISTRY')
        if not registry:
            dispatcher = os.environ.get('CORTEX_POOL_DISPATCHER')
            if not dispatcher:
                try:
                    import box_facts
                    dispatcher = box_facts.pool_dispatcher()
                except (ImportError, AttributeError):
                    pass
            if dispatcher:
                registry = Path(dispatcher).with_name('boxes.json')
        if not registry:
            try:
                from inventory_location import lead_script
                path = lead_script('boxes.json')
                if path.is_file():
                    registry = path
            except ImportError:
                pass
        if not registry:
            return None
        data = json.loads(Path(registry).read_text(encoding='utf-8-sig'))
        for box in data['boxes']:
            names = [box.get('name'), box.get('ssh'), box.get('hostname'), *box.get('aliases', [])]
            if self.alias.casefold() not in {str(name).casefold() for name in names if name}:
                continue
            for slot in box.get('task_slots', []):
                if slot['task'] == self.task:
                    return dict(slot, box_name=box['name'], free_floor_gb=box.get('free_floor_gb'))
        raise RuntimeError(f'named task is not registered: {self.alias} {self.task}')

    def run_local(self, argv: Iterable[object], timeout: float = 120, check: bool = True, what: str | None = None) -> str:
        argv = [str(part) for part in argv]
        if self.dry_run:
            self.say('dry-run: ' + ' '.join(argv))
            return ''
        done = subprocess.run(argv, capture_output=True, text=True, timeout=timeout, stdin=subprocess.DEVNULL,
                              creationflags=NO_WINDOW, encoding='utf-8', errors='replace')
        errors = '\n'.join(line for line in done.stderr.splitlines() if not SSH_NOISE.search(line))
        if check and done.returncode:
            raise RuntimeError(f'{what or argv[0]} failed ({done.returncode}): {errors[-600:]}')
        return done.stdout

    def ssh(self, command: str, timeout: float = 120, check: bool = True) -> str:
        return self.run_local(['ssh', self.alias, command], timeout, check, what=f'ssh {self.alias} {command[:80]!r}')

    def scp_to(self, local: Path | str, remote: Path | str, timeout: float = 300) -> None:
        self.run_local(['scp', '-q', str(local), f'{self.alias}:{Path(remote).as_posix()}'], timeout,
                       what=f'scp {Path(local).name}')

    def scp_from(self, remote: Path | str, local: Path | str, timeout: float = 900) -> None:
        self.run_local(['scp', '-q', f'{self.alias}:{Path(remote).as_posix()}', str(local)], timeout,
                       what=f'scp {Path(remote).name}')

    def mkdir(self, path: Path | str) -> None:
        self.ssh(f"New-Item -ItemType Directory -Force -Path {ps_quote(Path(path).as_posix())} | Out-Null")

    def reachable(self, timeout: float = 30) -> str | None:
        """None when the box answers, else the reason it does not."""
        try:
            out = self.run_local(['ssh', '-o', 'ConnectTimeout=15', self.alias, "Write-Output 'ok'"], timeout, what='ssh probe')
        except (RuntimeError, subprocess.TimeoutExpired, OSError) as error:
            return f'{type(error).__name__}: {str(error)[-300:]}'
        return None if self.dry_run or out.strip().endswith('ok') else f'ssh probe answered {out.strip()[-120:]!r}'

    def sha256(self, path: Path | str) -> str:
        return self.ssh(f"(Get-FileHash -Algorithm SHA256 -LiteralPath {ps_quote(Path(path).as_posix())}).Hash").strip().lower()

    def task_state(self) -> str:
        return self.ssh(f'(Get-ScheduledTask -TaskName {self.task}).State').strip() if not self.dry_run else 'Ready'

    def wait_task_idle(self, budget_s: float = 900, poll_s: float = 15) -> str:
        deadline = time.monotonic() + budget_s
        while True:
            state = self.task_state()
            if state != 'Running':
                return state
            if time.monotonic() > deadline:
                raise RuntimeError(f'{self.task} is still running another payload after {budget_s:.0f} s')
            self.say(f'{self.task} runs another payload; waiting')
            time.sleep(poll_s)

    def start_task(self, local_script: Path, budget_s: float = 900) -> None:
        """Preserved tasks use command.ps1; their run.ps1 is saved and never overwritten.

        The old constructor and direct tasks remain supported. Explicit catalog
        preserve_runner wins; without a catalog, an existing command.ps1 runner
        is protected. Normal completion and abort restore the previous command.
        """
        deadline = time.monotonic()+budget_s
        self.wait_task_idle(budget_s)
        slot = self.catalog_slot()
        preserve = (slot or {}).get('preserve_runner', self.preserve_runner)
        if self.dry_run or preserve is False:
            self.scp_to(local_script, self.session_script)
            self.ssh(f'Start-ScheduledTask -TaskName {self.task}')
            return
        slot = slot or dict(task=self.task, wrapper=self.session_script,
                            payload=str(PureWindowsPath(self.session_script).parent/'command.ps1'),
                            owner_marker=str(PureWindowsPath(self.session_script).parent/'payload-owner.json'),
                            box_name=self.alias)
        wrapper = ps_quote(slot['wrapper'])
        meta = json.loads(self.ssh(f"$p={wrapper}; @{{wrapper=if(Test-Path -LiteralPath $p){{[Convert]::ToBase64String([IO.File]::ReadAllBytes($p))}}else{{$null}}; command=Test-Path -LiteralPath {ps_quote(slot['payload'])}}} | ConvertTo-Json -Compress"))
        import base64
        data = base64.b64decode(meta['wrapper'] or '')
        identity = dict(name=slot['box_name'], free_floor_gb=slot.get('free_floor_gb'))
        slot['preserve_runner'] = True
        reason = wrapper_refusal(identity, slot, data=data)
        if preserve is None and not meta['command'] and reason:
            self.scp_to(local_script, self.session_script)
            self.ssh(f'Start-ScheduledTask -TaskName {self.task}')
            return
        if reason:
            raise RuntimeError(reason)
        slot['wrapper_sha256'] = hashlib.sha256(data).hexdigest()
        slot['payload_sha256'] = sha256_file(Path(local_script))
        token = uuid.uuid4().hex
        incoming = str(PureWindowsPath(slot['wrapper']).parent/('.direct-script-'+token+'.ps1')).replace('\\', '/')
        self.scp_to(local_script, incoming)
        self._direct_payload = slot, token
        try:
            remaining = max(0, deadline-time.monotonic())
            self.ssh(task_install_script(slot, token, incoming, remaining), timeout=remaining+60)
        except BaseException as error:
            try:
                self.abort_task()
            except Exception as cleanup:
                # Preserve the launch's exact refusal and retain failed cleanup.
                error.add_note(f'owned direct task cleanup: {type(cleanup).__name__}: {cleanup}')
            raise

    def abort_task(self) -> None:
        """Abort/restore only the direct payload token started by this instance."""
        if self._direct_payload is not None:
            slot, token = self._direct_payload
            self.ssh(task_restore_script(slot, token, abort=True), timeout=60)
            self._direct_payload = None

    def wait_done(self, done: Path | str, budget_s: float, slice_cap_s: int = 540) -> str:
        """Waits on a payload's done file; each ssh call stays under ten minutes."""
        if self.dry_run:
            return 'done rc=0 (dry-run)'
        done = Path(done).as_posix()
        deadline = time.monotonic() + budget_s
        while time.monotonic() < deadline:
            slice_s = int(max(10, min(slice_cap_s, deadline - time.monotonic())))
            text = self.ssh(f"$d={ps_quote(done)}; $t=0; while (-not (Test-Path -LiteralPath $d) -and $t -lt {slice_s}) "
                            f"{{ Start-Sleep 2; $t += 2 }}; if (Test-Path -LiteralPath $d) {{ Get-Content -LiteralPath $d }} "
                            f"else {{ 'WAITING' }}", timeout=slice_s + 60).strip()
            if text != 'WAITING':
                return text
        return 'TIMEOUT'

    def read_text(self, path: Path | str, timeout: float = 120) -> str | None:
        """A small remote text file's content, or None when it is absent."""
        path = ps_quote(Path(path).as_posix())
        text = self.ssh(f"if (Test-Path -LiteralPath {path}) {{ Get-Content -Raw -Encoding utf8 -LiteralPath {path} }} "
                        f"else {{ '<<ABSENT>>' }}", timeout=timeout)
        return None if text.strip() == '<<ABSENT>>' else text

    def fetch_tar(self, root: Path | str, names_like: list[str], excludes: list[str], local_root: Path | None = None,
                  tar_name: str = 'fetch-remote.tar') -> None:
        """Packs the top-level entries of <root> matching names_like (never a runtime, whose Data is a junction) and
        unpacks them under local_root (default: the same path here)."""
        root = Path(root)
        local_root = Path(local_root or root)
        tar = (root / tar_name).as_posix()
        likes = ' -or '.join(f"$_ -like {ps_quote(pattern)}" for pattern in names_like)
        exclude = ' '.join(f"--exclude {ps_quote(pattern)}" for pattern in excludes)
        self.ssh(f"Set-Location -LiteralPath {ps_quote(root.as_posix())}; $n = @(Get-ChildItem -Name | Where-Object {{ {likes} }}); "
                 f"tar.exe -cf {ps_quote(tar)} {exclude} @n", timeout=600)
        self.scp_from(tar, local_root / tar_name)
        self.ssh(f"Remove-Item -LiteralPath {ps_quote(tar)}")
        if not self.dry_run:
            self.run_local([WINDOWS_TAR, '-xf', local_root / tar_name, '-C', local_root], 600, what='tar -x')
            (local_root / tar_name).unlink()

    def fetch_list(self, root: Path | str, list_file: Path | str, local_root: Path, tar_name: str) -> int:
        """Packs the files named (relative to root) in a remote list file and unpacks them under local_root."""
        tar = (Path(root) / tar_name).as_posix()
        self.ssh(f"tar.exe -cf {ps_quote(tar)} -C {ps_quote(Path(root).as_posix())} -T {ps_quote(Path(list_file).as_posix())}",
                 timeout=900)
        local_root.mkdir(parents=True, exist_ok=True)
        self.scp_from(tar, local_root / tar_name, timeout=1800)
        self.ssh(f"Remove-Item -LiteralPath {ps_quote(tar)}")
        if self.dry_run:
            return 0
        self.run_local([WINDOWS_TAR, '-xf', local_root / tar_name, '-C', local_root], 900, what='tar -x')
        size = (local_root / tar_name).stat().st_size
        (local_root / tar_name).unlink()
        return size


class LocalBox(RemoteBox):
    """This box as a game box: the same calls answered here, the payload started as a hidden process of this session (an ssh
    session to this box has no desktop for the runner's private one)."""

    def ssh(self, command: str, timeout: float = 120, check: bool = True) -> str:
        return self.run_local(['pwsh', '-NoProfile', '-NonInteractive', '-Command', command], timeout, check, what=f'pwsh {command[:80]!r}')

    def scp_to(self, local: Path | str, remote: Path | str, timeout: float = 300) -> None:
        if not self.dry_run and Path(local).resolve() != Path(remote).resolve():
            Path(remote).parent.mkdir(parents=True, exist_ok=True)
            Path(remote).write_bytes(Path(local).read_bytes())

    def scp_from(self, remote: Path | str, local: Path | str, timeout: float = 900) -> None:
        self.scp_to(remote, local)

    def mkdir(self, path: Path | str) -> None:
        if not self.dry_run:
            Path(path).mkdir(parents=True, exist_ok=True)

    def reachable(self, timeout: float = 30) -> str | None:
        return None

    def sha256(self, path: Path | str) -> str:
        return sha256_file(Path(path))

    def task_state(self) -> str:
        return 'Ready'

    def start_task(self, local_script: Path, budget_s: float = 900) -> None:
        if self.dry_run:
            self.say(f'dry-run: pwsh -File {local_script}')
            return
        subprocess.Popen(['pwsh', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', str(local_script)],
                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=NO_WINDOW)

    def wait_done(self, done: Path | str, budget_s: float, slice_cap_s: int = 540) -> str:
        if self.dry_run:
            return 'done rc=0 (dry-run)'
        deadline = time.monotonic() + budget_s
        while time.monotonic() < deadline:
            if Path(done).is_file():
                time.sleep(1)
                return Path(done).read_text(encoding='ascii', errors='replace').strip()
            time.sleep(2)
        return 'TIMEOUT'

    def read_text(self, path: Path | str, timeout: float = 120) -> str | None:
        return Path(path).read_text(encoding='utf-8-sig') if Path(path).is_file() else None

    def fetch_list(self, root: Path | str, list_file: Path | str, local_root: Path, tar_name: str) -> int:
        """The run root is this box's own: the listed files are already where the driver reads them."""
        if Path(root).resolve() != Path(local_root).resolve():
            raise RuntimeError(f'a local box fetches only into its own run root, not {local_root}')
        return 0


def render_payload(cwd: Path | str, argv: list[str], log: Path | str, done: Path | str,
                   env: dict[str, str] | None = None, path_prepend: list[str] | None = None,
                   template: Path = PAYLOAD_TEMPLATE) -> str:
    """tools/remote/session_command.ps1 with one command line (python and its arguments) filled in; path_prepend puts
    directories ahead of the session's PATH (the tools the drivers call by name: date, ffmpeg)."""
    env = dict({'CCCP_HEADLESS': '1', 'PYTHONDONTWRITEBYTECODE': '1', 'PYTHONUNBUFFERED': '1'}, **(env or {}))
    fields = {
        'ENV': '\n'.join(f'$env:{name} = {ps_quote(value)}' for name, value in env.items()),
        'PATH': (f"$env:PATH = {ps_quote(';'.join(str(PureWindowsPath(d)) for d in path_prepend) + ';')} + $env:PATH"
                 if path_prepend else ''),
        'CWD': ps_quote(cwd), 'LOG': ps_quote(Path(log).as_posix()), 'DONE': ps_quote(Path(done).as_posix()),
        'ARGV': '@(' + ', '.join(ps_quote(arg) for arg in argv) + ')',
    }
    script = template.read_text(encoding='utf-8')
    for key, value in fields.items():
        script = script.replace('{{' + key + '}}', value)
    left = re.findall(r'\{\{\w+\}\}', script)
    if left:
        raise ValueError(f'payload template fields left unfilled: {left}')
    return script


# --- on the box ------------------------------------------------------------------------------------------------------

def is_reparse(entry: os.DirEntry) -> bool:
    if entry.is_symlink():
        return True
    if hasattr(entry, 'is_junction') and entry.is_junction():
        return True
    try:
        return bool(getattr(entry.stat(follow_symlinks=False), 'st_file_attributes', 0) & REPARSE)
    except OSError:
        return True


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, 'rb') as handle:
        for block in iter(lambda: handle.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def hash_list(root: Path, names: list[str]) -> dict[str, str | None]:
    return {name: (sha256_file(root / name) if (root / name).is_file() else None) for name in names}


def small_files(root: Path, suffixes: tuple[str, ...] = EVIDENCE_SUFFIXES, max_bytes: int = EVIDENCE_MAX_BYTES,
                skip_dirs: tuple[str, ...] = ('runtime', 'frames', '__pycache__')) -> list[str]:
    """Relative paths of the small evidence files under root; junctions, symlinks and runtime trees never entered."""
    found: list[str] = []
    stack = [root]
    while stack:
        directory = stack.pop()
        try:
            entries = list(os.scandir(directory))
        except OSError:
            continue
        for entry in entries:
            if is_reparse(entry):
                continue
            if entry.is_dir(follow_symlinks=False):
                if entry.name not in skip_dirs:
                    stack.append(Path(entry.path))
            elif entry.name.lower().endswith(suffixes) and entry.stat(follow_symlinks=False).st_size <= max_bytes:
                found.append(Path(entry.path).relative_to(root).as_posix())
    return sorted(found)


# --- self-test -------------------------------------------------------------------------------------------------------

def self_test() -> int:
    failures: list[str] = []

    def expect(name: str, ok: bool) -> None:
        print(f"[remote_box self-test] {'PASS' if ok else 'FAIL'} {name}")
        if not ok:
            failures.append(name)

    expect('ps_quote doubles a single quote', ps_quote("a'b") == "'a''b'")
    script = render_payload('work/tree', ['tools/x.py', "--out", "scratch/l a/o'k"], 'scratch/l/log.txt',
                            'scratch/l/done.txt', {'INVENTORY_NO_FULLSTATE': '1'}, ['tools/git/bin'])
    expect('payload fields all filled', '{{' not in script)
    expect('payload argv quoted', "@('tools/x.py', '--out', 'scratch/l a/o''k')" in script)
    expect('payload sets the headless and inventory environment',
           "$env:CCCP_HEADLESS = '1'" in script and "$env:INVENTORY_NO_FULLSTATE = '1'" in script)
    expect('payload never starts the engine itself', 'Cortex Command' not in script)
    expect('payload puts the listed directories ahead of PATH',
           "$env:PATH = 'tools\\git\\bin;' + $env:PATH" in script)
    expect('ssh noise filtered', bool(SSH_NOISE.search('** WARNING: connection is not using a post-quantum key exchange')))
    said: list[str] = []
    box = RemoteBox('remote', dry_run=True, say=said.append)
    box.ssh('Write-Output 1')
    expect('dry-run prints and sends nothing', bool(said) and said[-1].startswith('dry-run: ssh remote'))
    expect('dry-run task is Ready', box.task_state() == 'Ready')
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        (root / 'S2/a/run/runtime').mkdir(parents=True)
        (root / 'S2/a/run/runtime/big.json').write_text('{}', encoding='utf-8')
        (root / 'S2/a/result.json').write_text('{"pass": true}', encoding='utf-8')
        (root / 'S2/a/run/frame.png').write_bytes(b'\0' * 16)
        (root / 'S2/a/run/huge.log').write_bytes(b'x' * 64)
        listed = small_files(root / 'S2', max_bytes=32)
        expect('evidence keeps small json, skips runtime, media and oversize', listed == ['a/result.json'])
        hashes = hash_list(root / 'S2', ['a/result.json', 'missing.txt'])
        expect('hash of a present file and None for an absent one',
               hashes['a/result.json'] == hashlib.sha256(b'{"pass": true}').hexdigest() and hashes['missing.txt'] is None)
    print(f"[remote_box self-test] {'PASS' if not failures else 'FAIL'} {len(failures)} failure(s)")
    return 1 if failures else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--self-test', action='store_true')
    sub = parser.add_subparsers(dest='command')
    hashing = sub.add_parser('hash')
    hashing.add_argument('--root', type=Path, required=True)
    hashing.add_argument('--list', type=Path, required=True, help='one relative path per line')
    evidence = sub.add_parser('evidence')
    evidence.add_argument('--root', type=Path, required=True)
    evidence.add_argument('--out', type=Path, required=True)
    evidence.add_argument('--max-bytes', type=int, default=EVIDENCE_MAX_BYTES)
    evidence.add_argument('--suffix', nargs='+', default=list(EVIDENCE_SUFFIXES))
    options = parser.parse_args(argv)
    if options.self_test:
        return self_test()
    if options.command == 'hash':
        names = [line.strip() for line in options.list.read_text(encoding='utf-8').splitlines() if line.strip()]
        print(json.dumps(hash_list(options.root, names)))
        return 0
    if options.command == 'evidence':
        files = small_files(options.root, tuple(s.lower() for s in options.suffix), options.max_bytes)
        options.out.write_text('\n'.join(files) + '\n', encoding='utf-8')
        print(json.dumps({'files': len(files), 'list': str(options.out)}))
        return 0
    parser.print_help()
    return 2


if __name__ == '__main__':
    sys.exit(main())
