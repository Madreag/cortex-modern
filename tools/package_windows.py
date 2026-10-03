"""Packages the Windows build: CortexCommand-mp-<version>-win64.zip with the executable, the DLLs it ships with, Data and the
VERSION.txt, LICENSE and README files, a MANIFEST.json naming every file with its bytes and sha256 inside the zip, and the zip's own
sha256 beside it. The version is VERSION.txt at the tree's root (a file named VERSION there would shadow the C++ <version>
header on a case-insensitive file system: the root is on the include path).

  python tools/package_windows.py [--repo <tree>] [--out <dir>]     (the default out is <tree>/dist)
  python tools/package_windows.py --verify <unpacked dir> --smoke <sp-smoke capture root>
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
EXECUTABLE = "Cortex Command.exe"
BUILD_RECEIPT = 'BUILD-RECEIPT.json'
REQUIRED_PAYLOAD = {EXECUTABLE, 'VERSION.txt', 'LICENSE', 'README.md', 'Data/Base.rte/Index.ini', BUILD_RECEIPT}
# The sanitizer runtime belongs to the ASan configuration's builds, never to a release.
EXCLUDED_DLLS = re.compile(r"clang_rt\.asan.*\.dll", re.I)
VERSION_FORM = re.compile(r"\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def package_files(repo: Path) -> list[Path]:
    """The executable, the DLLs beside it, every file under Data (no link followed) and the tree's VERSION.txt, LICENSE, README."""
    files = [repo / EXECUTABLE]
    files += sorted(path for path in repo.glob("*.dll") if not EXCLUDED_DLLS.fullmatch(path.name))
    for folder, directories, names in os.walk(repo / "Data", followlinks=False):
        directories[:] = sorted(name for name in directories if not (Path(folder) / name).is_symlink())
        files += [Path(folder) / name for name in sorted(names)]
    files += [repo / name for name in ("VERSION.txt", "LICENSE", "README.md") if (repo / name).is_file()]
    return files


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
    measured = sha256(repo / EXECUTABLE)
    if build.get('commit') != source['commit'] or build.get('executable_sha256') != measured:
        raise SystemExit(f'build receipt commit/hash={build.get("commit")}/{build.get("executable_sha256")}; packaged source/hash={source["commit"]}/{measured}')
    out.mkdir(parents=True, exist_ok=True)
    archive = out / f"CortexCommand-mp-{version}-win64.zip"
    entries = []
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as bundle:
        for path in package_files(repo):
            name = path.relative_to(repo).as_posix()
            bundle.write(path, name)
            entries.append(dict(path=name, bytes=path.stat().st_size, sha256=sha256(path)))
        payload = json.dumps(build, indent=2).encode('utf-8')
        bundle.writestr(BUILD_RECEIPT, payload)
        entries.append(dict(path=BUILD_RECEIPT, bytes=len(payload), sha256=hashlib.sha256(payload).hexdigest()))
        manifest = dict(schema=1, version=version, tag=f"v{version}", source=source, executable=EXECUTABLE,
                        build_receipt=BUILD_RECEIPT, files=entries)
        bundle.writestr("MANIFEST.json", json.dumps(manifest, indent=1))
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
        source = (manifest.get('source') or {}).get('commit')
        if not isinstance(source, str) or not re.fullmatch(r'[0-9a-f]{40}', source) or build.get('commit') != source:
            errors.append(f'build commit={build.get("commit")!r}, source commit={source!r}')
        if not (unpacked / EXECUTABLE).is_file() or build.get('executable_sha256') != sha256(unpacked / EXECUTABLE):
            errors.append('packaged executable hash differs from build receipt')
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
