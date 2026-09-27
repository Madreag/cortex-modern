"""A second Windows box reached over ssh, whose engine runs only inside the owner's interactive session through one
scheduled task (EDITH's cortex-session1). Shared by tools/edith_cross.py and the inventory's split runner.

    python tools/edith/remote_box.py hash --root <dir> --list <file>      sha256 of each listed relative path (JSON)
    python tools/edith/remote_box.py evidence --root <dir> --out <list> [--max-bytes N] [--suffix .json ...]
    python tools/edith/remote_box.py --self-test

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
from pathlib import Path
from typing import Callable, Iterable

LOG = logging.getLogger('remote_box')
SSH_NOISE = re.compile(r'post-quantum|store now, decrypt later|may need to be upgraded|openssh\.com/pq', re.I)
WINDOWS_TAR = 'C:/Windows/System32/tar.exe'
PAYLOAD_TEMPLATE = Path(__file__).resolve().parent / 'session_command.ps1'
EVIDENCE_SUFFIXES = ('.json', '.log', '.txt', '.md')
EVIDENCE_MAX_BYTES = 4 << 20
REPARSE = getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0x400)
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


def ps_quote(value: object) -> str:
    """A PowerShell single-quoted literal."""
    return "'" + str(value).replace("'", "''") + "'"


class RemoteBox:
    """ssh/scp to one box plus its single session task. With dry_run every call is printed, nothing is sent."""

    def __init__(self, alias: str, task: str = 'cortex-session1', session_script: str = 'D:/mx/session1/run.ps1',
                 dry_run: bool = False, say: Callable[[str], None] | None = None) -> None:
        self.alias = alias
        self.task = task
        self.session_script = session_script
        self.dry_run = dry_run
        self.say = say or LOG.info

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
        """Waits for the task to be free, installs the rendered payload as the task's script and starts it."""
        self.wait_task_idle(budget_s)
        self.scp_to(local_script, self.session_script)
        self.ssh(f'Start-ScheduledTask -TaskName {self.task}')

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
                  tar_name: str = 'fetch-edith.tar') -> None:
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


def render_payload(cwd: Path | str, argv: list[str], log: Path | str, done: Path | str,
                   env: dict[str, str] | None = None, template: Path = PAYLOAD_TEMPLATE) -> str:
    """tools/edith/session_command.ps1 with one command line (python and its arguments) filled in."""
    env = dict({'CCCP_HEADLESS': '1', 'PYTHONDONTWRITEBYTECODE': '1', 'PYTHONUNBUFFERED': '1'}, **(env or {}))
    fields = {
        'ENV': '\n'.join(f'$env:{name} = {ps_quote(value)}' for name, value in env.items()),
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
    script = render_payload('D:\\Projects\\t', ['tools/x.py', "--out", "D:/mx/l a/o'k"], 'D:/mx/l/log.txt',
                            'D:/mx/l/done.txt', {'INVENTORY_NO_FULLSTATE': '1'})
    expect('payload fields all filled', '{{' not in script)
    expect('payload argv quoted', "@('tools/x.py', '--out', 'D:/mx/l a/o''k')" in script)
    expect('payload sets the headless and inventory environment',
           "$env:CCCP_HEADLESS = '1'" in script and "$env:INVENTORY_NO_FULLSTATE = '1'" in script)
    expect('payload never starts the engine itself', 'Cortex Command' not in script)
    expect('ssh noise filtered', bool(SSH_NOISE.search('** WARNING: connection is not using a post-quantum key exchange')))
    said: list[str] = []
    box = RemoteBox('edith', dry_run=True, say=said.append)
    box.ssh('Write-Output 1')
    expect('dry-run prints and sends nothing', bool(said) and said[-1].startswith('dry-run: ssh edith'))
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
