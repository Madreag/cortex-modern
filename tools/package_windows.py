"""Packages the Windows build: CortexCommand-mp-<version>-win64.zip with the executable, the DLLs it ships with, Data and the
VERSION.txt, LICENSE and README files, a MANIFEST.json naming every file with its bytes and sha256 inside the zip, and the zip's own
sha256 beside it. The version is VERSION.txt at the tree's root (a file named VERSION there would shadow the C++ <version>
header on a case-insensitive file system: the root is on the include path).

  python tools/package_windows.py [--repo <tree>] [--out <dir>]     (the default out is <tree>/dist)
  python tools/package_windows.py --verify <unpacked dir> --smoke <sp-smoke capture root>
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import re
import struct
import subprocess
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
EXECUTABLE = "Cortex Command.exe"
BUILD_RECEIPT = 'BUILD-RECEIPT.json'
REQUIRED_PAYLOAD = {EXECUTABLE, 'VERSION.txt', 'LICENSE', 'README.md', 'Data/Base.rte/Index.ini', BUILD_RECEIPT}
# The sanitizer runtime belongs to the ASan configuration's builds, never to a release.
EXCLUDED_DLLS = re.compile(r"(?:clang_rt\.(?:asan|lsan|msan|tsan|ubsan).*|vcasan.*)\.dll", re.I)
VC_RUNTIME = re.compile(r"(?:msvcp|msvcr|vcruntime|concrt|vcomp)\d+[a-z0-9_]*\.dll", re.I)
VERSION_FORM = re.compile(r"\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?")
CONFIGURATIONS = {'Final', 'Debug Full', 'Debug Minimal', 'Debug Release'}
RECEIPT_FIELDS = {'commit', 'executable_sha256', 'configuration', 'version'}
RUNTIME_NOTICE = ('Microsoft Visual C++ runtime libraries: copyright Microsoft Corporation; '
                  'redistributed under the Visual Studio license terms. '
                  'https://learn.microsoft.com/cpp/windows/redistributing-visual-cpp-files')


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def package_files(repo: Path) -> list[Path]:
    """Tracked game content and the build's executable and adjacent DLLs."""
    if source_of(repo)['dirty']:
        raise SystemExit('tracked files have uncommitted changes; package a clean source tree')
    tracked = subprocess.run(['git', '-C', str(repo), 'ls-files', '-z'], capture_output=True, check=True).stdout.decode('utf-8')
    files = [repo / EXECUTABLE]
    files += sorted((path for path in repo.iterdir() if path.is_file() and path.suffix.casefold() == '.dll'
                     and not EXCLUDED_DLLS.fullmatch(path.name)), key=lambda path: path.name.casefold())
    files += [repo / name for name in sorted(tracked.split('\0'))
              if name.startswith(('Data/', 'Licences/')) or name in ('VERSION.txt', 'LICENSE', 'README.md')]
    for path in files:
        if not path.is_file():
            raise SystemExit(f'package input absent: {path.relative_to(repo)}')
        for part in (path, *path.relative_to(repo).parents):
            part = part if part.is_absolute() else repo / part
            if part.is_symlink() or getattr(part, 'is_junction', lambda: False)():
                raise SystemExit(f'package input follows a link: {path.relative_to(repo)}')
    return files


def pe_info(path: Path) -> dict:
    """Read normal and delay imports directly from an x64 PE image."""
    data = path.read_bytes()
    def read(form, offset):
        if offset < 0 or offset + struct.calcsize(form) > len(data):
            raise ValueError('truncated PE field')
        return struct.unpack_from(form, data, offset)
    try:
        if data[:2] != b'MZ': raise ValueError('DOS signature absent')
        header, = read('<I', 0x3c)
        if data[header:header+4] != b'PE\0\0': raise ValueError('PE signature absent')
        machine, count = read('<HH', header+4)
        if machine != 0x8664: raise ValueError(f'PE machine={machine:#x}, unsupported architecture')
        size, = read('<H', header+20)
        optional = header+24
        if read('<H', optional)[0] != 0x20b or size < 112: raise ValueError('PE32+ header absent')
        linker = read('<BB', optional+2)
        image_base, = read('<Q', optional+24)
        headers, = read('<I', optional+60)
        directories, = read('<I', optional+108)
        sections = [read('<IIII', optional+size+40*index+8) for index in range(count)]
        def offset(rva):
            if 0 <= rva < min(headers, len(data)): return rva
            for virtual_size, address, raw_size, raw in sections:
                if address <= rva < address+max(virtual_size, raw_size):
                    if rva-address >= raw_size: raise ValueError('import lies in unbacked section')
                    return raw+rva-address
            raise ValueError(f'unmapped import RVA={rva:#x}')
        imports = set()
        for index, stride in ((1, 20), (13, 32)):
            if index >= directories: continue
            if 112+(index+1)*8 > size: raise ValueError('truncated data directories')
            address, length = read('<II', optional+112+index*8)
            if not address: continue
            if length < stride: raise ValueError('truncated import directory')
            for position in range(0, length-stride+1, stride):
                descriptor = read('<'+'I'*(stride//4), offset(address+position))
                if not any(descriptor): break
                name = descriptor[3] if index == 1 else descriptor[1]
                if index == 13 and not descriptor[0] & 1: name -= image_base
                start = offset(name)
                end = data.find(b'\0', start, start+260)
                if end < 0: raise ValueError('unterminated import name')
                name = data[start:end].decode('ascii')
                if not name or re.search(r'[\\/:]', name): raise ValueError('invalid import name')
                imports.add(name.casefold())
            else:
                raise ValueError('import directory has no terminator')
        return dict(linker=linker, imports=sorted(imports))
    except (ValueError, struct.error, UnicodeError) as error:
        raise SystemExit(f'{path.name}: invalid PE: {error}') from error


def visual_studio_installations() -> list[Path]:
    locator = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    if not locator.is_file(): raise SystemExit('vswhere.exe absent; cannot locate the build toolset redistributables')
    result = subprocess.run([str(locator), '-all', '-products', '*', '-requires',
                             'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'],
                            capture_output=True, text=True, check=True)
    return [Path(line) for line in result.stdout.splitlines() if line.strip()]


def redistributable_crt(executable: Path) -> tuple[Path, str]:
    family = pe_info(executable)['linker']
    prefix = '.'.join(map(str, family))+'.'
    candidates = []
    for install in visual_studio_installations():
        if not any((install/'VC/Tools/MSVC').glob(prefix+'*')): continue
        for redist in (install/'VC/Redist/MSVC').glob(prefix+'*'):
            if not re.fullmatch(r'\d+\.\d+\.\d+', redist.name): continue
            for crt in (redist/'x64').glob('Microsoft.VC*.CRT'):
                if crt.is_dir(): candidates.append((tuple(map(int, redist.name.split('.'))), crt))
    if not candidates: raise SystemExit(f'no x64 CRT redistributable for executable linker {prefix[:-1]}')
    version, folder = sorted(candidates, key=lambda item: (item[0], str(item[1])))[-1]
    return folder, '.'.join(map(str, version))


def system_dll(name: str) -> bool:
    if VC_RUNTIME.fullmatch(name) or EXCLUDED_DLLS.fullmatch(name): return False
    if name.startswith(('api-ms-win-', 'ext-ms-win-')):
        if os.name != 'nt': return False
        try:
            ctypes.WinDLL(name, winmode=0x00000800)
            return True
        except OSError:
            return False
    system_root = os.environ.get('SystemRoot')
    return bool(system_root and (Path(system_root)/'System32'/name).is_file())


def dependency_payload(repo: Path, files: list[Path]) -> tuple[list[Path], dict]:
    binaries = {path.name.casefold(): path for path in files if path.parent == repo and path.suffix.casefold() in ('.exe', '.dll')}
    origins, seen = {}, set()
    crt = None
    pending = list(binaries)
    while pending:
        name = pending.pop()
        if name in seen: continue
        if VC_RUNTIME.fullmatch(name):
            if crt is None:
                try: crt = redistributable_crt(repo/EXECUTABLE)
                except SystemExit as error: raise SystemExit(f'unresolved runtime import: {name}; {error}') from error
            runtime = {path.name.casefold(): path for path in crt[0].glob('*.dll')}.get(name)
            if runtime is None: raise SystemExit(f'unresolved runtime import: {name}; absent from CRT redistributable {crt[1]}')
            binaries[name] = runtime
            origins[runtime.name] = dict(kind='msvc-redist', version=crt[1])
        seen.add(name)
        for dependency in pe_info(binaries[name])['imports']:
            if EXCLUDED_DLLS.fullmatch(dependency): raise SystemExit(f'{name}: sanitizer runtime import: {dependency}')
            if dependency in binaries:
                pending.append(dependency)
            elif VC_RUNTIME.fullmatch(dependency):
                binaries[dependency] = repo/dependency
                pending.append(dependency)
            elif not system_dll(dependency):
                raise SystemExit(f'{name}: unresolved import: {dependency}; absent beside the executable')
    content = [path for path in files if not (path.parent == repo and path.suffix.casefold() in ('.exe', '.dll'))]
    return content + sorted(binaries.values(), key=lambda path: path.name.casefold()), origins


def unresolved_imports(files: list[Path]) -> list[str]:
    binaries = {path.name.casefold(): path for path in files if path.suffix.casefold() in ('.exe', '.dll')}
    return [f'{path.name}: unresolved import: {dependency}' for name, path in binaries.items()
            for dependency in pe_info(path)['imports'] if dependency not in binaries and not system_dll(dependency)]


def source_of(repo: Path) -> dict:
    """The commit and tree the package was built from, and whether the working tree differed from them."""
    def git(*args: str) -> str:
        return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True, check=True).stdout.strip()
    return dict(commit=git("rev-parse", "HEAD"), tree=git("rev-parse", "HEAD^{tree}"),
                dirty=bool(git("status", "--porcelain", "--untracked-files=no")))


def package(repo: Path, out: Path, build_receipt: Path | None = None) -> Path:
    version = (repo / "VERSION.txt").read_text(encoding="utf-8").strip()
    if not VERSION_FORM.fullmatch(version):
        raise SystemExit(f"VERSION.txt reads {version!r}, not a version")
    if not (repo / EXECUTABLE).is_file():
        raise SystemExit(f"no {EXECUTABLE} in {repo}: build first")
    missing = [name for name in REQUIRED_PAYLOAD - {BUILD_RECEIPT} if not (repo / name).is_file() or not (repo / name).stat().st_size]
    if missing: raise SystemExit(f'payload files absent/empty: {sorted(missing)}')
    receipt_path = build_receipt or repo / 'tools/cross_peers/build.json'
    if not receipt_path.is_file(): raise SystemExit(f'build receipt absent: {receipt_path}')
    build = json.loads(receipt_path.read_text(encoding='utf-8-sig'))
    source = source_of(repo)
    files = package_files(repo)
    measured = sha256(repo / EXECUTABLE)
    if build.get('commit') != source['commit'] or build.get('executable_sha256') != measured:
        raise SystemExit(f'build receipt commit/hash={build.get("commit")}/{build.get("executable_sha256")}; packaged source/hash={source["commit"]}/{measured}')
    configuration = build.get('configuration')
    if configuration not in CONFIGURATIONS: raise SystemExit(f'unsupported release configuration: {configuration!r}')
    files, origins = dependency_payload(repo, files)
    public_build = dict(commit=source['commit'], executable_sha256=measured, configuration=configuration, version=version)
    out.mkdir(parents=True, exist_ok=True)
    archive = out / f"CortexCommand-mp-{version}-win64.zip"
    entries = []
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as bundle:
        for path in files:
            name = path.relative_to(repo).as_posix() if path.is_relative_to(repo) else path.name
            bundle.write(path, name)
            entries.append(dict(path=name, bytes=path.stat().st_size, sha256=sha256(path)))
            if name in origins: entries[-1]['origin'] = origins[name]
        payload = json.dumps(public_build, indent=2).encode('utf-8')
        bundle.writestr(BUILD_RECEIPT, payload)
        entries.append(dict(path=BUILD_RECEIPT, bytes=len(payload), sha256=hashlib.sha256(payload).hexdigest()))
        manifest = dict(schema=1, version=version, tag=f"v{version}", source=source, executable=EXECUTABLE,
                        build_receipt=BUILD_RECEIPT, files=entries)
        if origins: manifest['runtime_notice'] = RUNTIME_NOTICE
        bundle.writestr("MANIFEST.json", json.dumps(manifest, indent=1))
    if source_of(repo) != source or sha256(repo/EXECUTABLE) != measured:
        archive.unlink()
        raise SystemExit('source or executable changed while packaging')
    (out / f"{archive.name}.sha256").write_text(f"{sha256(archive)}  {archive.name}\n", encoding="utf-8")
    print(f"[package] {archive} files={len(entries)} bytes={archive.stat().st_size} version={version}", flush=True)
    return archive


def unpacked_smoke(unpacked, manifest, root):
    errors = []
    try:
        capture, review = [json.loads((root / leaf).read_text(encoding='utf-8-sig')) for leaf in ('capture.json', 'review.json')]
        if capture.get('scenario') != 'sp-smoke': errors.append(f'smoke scenario={capture.get("scenario")!r}')
        if capture.get('interrupted'): errors.append(f'smoke interrupted: {capture["interrupted"]}')
        exe = capture.get('exe') or {}
        if Path(exe.get('path', '')).resolve() != (unpacked / EXECUTABLE).resolve() or exe.get('sha256') != sha256(unpacked / EXECUTABLE):
            errors.append('smoke executable path/hash differs from the unpacked executable')
        source = capture.get('source') or {}
        if source.get('tip') != manifest.get('source', {}).get('commit') or source.get('package') != manifest.get('tag'):
            errors.append('smoke source/package differs from the unpacked manifest')
        peers = [peer for run in capture.get('runs', []) for peer in run.get('peers', [])]
        smoke_spec = json.loads((Path(__file__).parent / 'e2e/sp-smoke.json').read_text(encoding='utf-8'))
        expected = {(run['name'], peer['name']) for run in smoke_spec['runs'] for peer in run.get('peers', smoke_spec.get('peers', []))}
        actual = [(run.get('name'), peer.get('peer')) for run in capture.get('runs', []) for peer in run.get('peers', [])]
        if set(actual) != expected or len(actual) != len(expected): errors.append(f'smoke required/observed runs={sorted(expected)}/{actual}')
        if not peers or any(peer.get('record', {}).get('exit_code') != 0 or peer.get('record', {}).get('timed_out')
                            or peer.get('record', {}).get('exe_sha256') != exe.get('sha256') for peer in peers):
            errors.append('smoke has absent/failed/mismatched peer launch evidence')
        if any(peer.get('record', {}).get('runner') != 'win32_test_runner.py'
               or Path(peer.get('record', {}).get('exe_path') or '').resolve() != (unpacked / EXECUTABLE).resolve()
               or Path(peer.get('record', {}).get('package_unpacked') or '').resolve() != unpacked.resolve() for peer in peers):
            errors.append('smoke lacks the Windows runner receipt for the actual unpacked executable path')
        items = review.get('checklist') or []
        if not items or review.get('run_findings') or review.get('interrupted') or any(
                item.get('finding') or item.get('blocked_by') or item.get('probe') not in ('pass', 'awaiting-review') for item in items):
            errors.append('smoke review has missing, failed or incomplete required checks')
        for run, name in actual:
            required = {f'recording-rate-{name}', f'recording-stills-{name}'}
            if not required <= {row.get('id') for row in items if row.get('run') == run and row.get('probe') == 'pass'}:
                errors.append(f'{name}: smoke recording health/still checks absent or failed')
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as error:
        errors.append(f'smoke evidence: {type(error).__name__}: {error}')
    return dict(id='unpacked-smoke', required=True, status='FAIL' if errors else 'PASS', errors=errors,
                evidence=str(root), review_state=review.get('verdict') if 'review' in locals() else 'INCOMPLETE')


def verify_evidence(unpacked: Path, smoke: Path | None = None) -> dict:
    errors, names = [], set()
    try:
        manifest = json.loads((unpacked / 'MANIFEST.json').read_text(encoding='utf-8-sig'))
        entries = manifest.get('files')
        if not isinstance(entries, list) or not entries: errors.append('manifest files is empty or absent'); entries = []
        if manifest.get('schema') != 1 or manifest.get('executable') != EXECUTABLE or manifest.get('build_receipt') != BUILD_RECEIPT:
            errors.append('manifest payload schema/executable/build receipt differs')
        version = manifest.get('version')
        if not isinstance(version, str) or not VERSION_FORM.fullmatch(version) or manifest.get('tag') != 'v'+version:
            errors.append(f'version/tag={version!r}/{manifest.get("tag")!r}')
        for entry in entries:
            name = entry.get('path', '')
            path = (unpacked / name).resolve()
            if not name or path == unpacked.resolve() or not path.is_relative_to(unpacked.resolve()) or name.casefold() in {n.casefold() for n in names}:
                errors.append(f'invalid/duplicate payload path={name!r}'); continue
            names.add(name)
            if not path.is_file() or path.stat().st_size != entry.get('bytes') or sha256(path) != entry.get('sha256'):
                errors.append(f'{name}: missing file, size or sha256 mismatch')
            if name in REQUIRED_PAYLOAD and (not path.is_file() or path.stat().st_size == 0): errors.append(f'{name}: required file is empty/absent')
        for name in sorted(REQUIRED_PAYLOAD - names): errors.append(f'required payload missing from manifest: {name}')
        if not (unpacked / 'Data').is_dir(): errors.append('Data directory absent')
        build = json.loads((unpacked / BUILD_RECEIPT).read_text(encoding='utf-8-sig'))
        if set(build) != RECEIPT_FIELDS or build.get('configuration') not in CONFIGURATIONS or build.get('version') != version:
            errors.append('packaged build receipt has private, missing or invalid fields')
        source = (manifest.get('source') or {}).get('commit')
        if not isinstance(source, str) or not re.fullmatch(r'[0-9a-f]{40}', source) or build.get('commit') != source:
            errors.append(f'build commit={build.get("commit")!r}, source commit={source!r}')
        if not (unpacked / EXECUTABLE).is_file() or build.get('executable_sha256') != sha256(unpacked / EXECUTABLE):
            errors.append('packaged executable hash differs from build receipt')
        binaries = [unpacked/name for name in names if '/' not in name and Path(name).suffix.casefold() in ('.exe', '.dll')
                    and (unpacked/name).is_file()]
        try: errors.extend(unresolved_imports(binaries))
        except SystemExit as error: errors.append(str(error))
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as error:
        errors.append(f'package evidence: {type(error).__name__}: {error}')
    manifest = manifest if 'manifest' in locals() and isinstance(manifest, dict) else {}
    collection = [dict(id='payload-and-build', required=True, status='FAIL' if errors else 'PASS', errors=errors),
                  unpacked_smoke(unpacked, manifest, smoke or unpacked / 'smoke')]
    return dict(passed=all(row['status'] == 'PASS' for row in collection), collection=collection, files=len(names))


def verify(unpacked: Path, smoke: Path | None = None) -> int:
    result = verify_evidence(unpacked, smoke)
    (unpacked / 'verification.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(f'[package] verify {unpacked} {json.dumps(result)}', flush=True)
    return 0 if result['passed'] else 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=Path, default=REPO)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--verify", type=Path, help="check an unpacked package against its manifest")
    parser.add_argument('--build-receipt', type=Path, help='the build\'s recorded source commit and executable hash')
    parser.add_argument('--smoke', type=Path, help='sp-smoke capture/review run from the unpacked executable; required for --verify')
    options = parser.parse_args(argv)
    if options.verify:
        return verify(options.verify.resolve(), options.smoke.resolve() if options.smoke else None)
    repo = options.repo.resolve()
    package(repo, (options.out or repo / "dist").resolve(), options.build_receipt)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
