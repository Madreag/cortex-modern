"""Release contents come from tracked files and the executable's imports."""
import contextlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import package_windows as package


def pe(path, imports=(), delayed=(), linker=(14, 50), machine=0x8664):
    """A small PE with real import descriptors, never executed."""
    data = bytearray(4096)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 0x3c, 0x80)
    data[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<HHIIIHH', data, 0x84, machine, 1, 0, 0, 0, 240, 0)
    struct.pack_into('<HBB', data, 0x98, 0x20b, *linker)
    struct.pack_into('<I', data, 0x98 + 60, 512)
    struct.pack_into('<I', data, 0x98 + 108, 16)
    struct.pack_into('<8sIIIIIIHHI', data, 0x188, b'.rdata', 3584, 0x1000, 3584, 512, 0, 0, 0, 0, 0)
    cursor = 2048
    for names, index, offset, stride in ((imports, 1, 512, 20), (delayed, 13, 1024, 32)):
        if names:
            struct.pack_into('<II', data, 0x98 + 112 + index * 8, offset + 0xe00, (len(names)+1)*stride)
        for number, name in enumerate(names):
            encoded = name.encode('ascii') + b'\0'
            data[cursor:cursor+len(encoded)] = encoded
            if index == 1:
                struct.pack_into('<IIIII', data, offset+number*stride, 1, 0, 0, cursor+0xe00, 1)
            else:
                struct.pack_into('<IIIIIIII', data, offset+number*stride, 1, cursor+0xe00, 0, 1, 1, 0, 0, 0)
            cursor += len(encoded)
    path.write_bytes(data)


def git(root, *args):
    return subprocess.run(['git', '-C', str(root), *args], check=True, capture_output=True).stdout


def fixture(root, runtime=True):
    repo = root/'repo'; repo.mkdir()
    for name in ('VERSION.txt', 'LICENSE', 'README.md', 'Data/Base.rte/Index.ini', 'Licences/notice.txt'):
        path = repo/name; path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('1.0.0' if name == 'VERSION.txt' else 'public content', encoding='utf-8')
    git(repo, 'init', '-q'); git(repo, 'add', '.')
    git(repo, '-c', 'commit.gpgsign=false', 'commit', '-qm', 'Create package inputs')
    pe(repo/package.EXECUTABLE, ('support.dll',) if runtime else ())
    pe(repo/'support.dll', ('msvcp140.dll', 'vcruntime140.dll') if runtime else (),
       ('msvcp140_atomic_wait.dll',) if runtime else ())
    pe(repo/'clang_rt.asan_dynamic-x86_64.dll')
    install = root/'installation'
    crt = install/'VC/Redist/MSVC/14.50.35710/x64/Microsoft.VC145.CRT'; crt.mkdir(parents=True)
    (install/'VC/Tools/MSVC/14.50.35717').mkdir(parents=True)
    for name in ('msvcp140.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll'):
        pe(crt/name, ('vcruntime140_1.dll',) if name == 'msvcp140.dll' else ())
    receipt = dict(commit=git(repo, 'rev-parse', 'HEAD').decode().strip(),
                   executable_sha256=package.sha256(repo/package.EXECUTABLE), configuration='Final',
                   build_log=str(root/'private-build.log'),
                   lead_build_receipt=dict(label='private-build', repo='private-tree'))
    receipt_path = repo/'tools/cross_peers/build.json'; receipt_path.parent.mkdir(parents=True)
    receipt_path.write_text(json.dumps(receipt), encoding='utf-8')
    return repo, install, crt


class PackageContents(unittest.TestCase):
    def archive(self, root, repo, install):
        with patch.object(package, 'visual_studio_installations', return_value=[install], create=True), contextlib.redirect_stdout(io.StringIO()):
            return package.package(repo, root/'out')

    def test_package_uses_only_tracked_content(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            built = os.environ.get('CC_PACKAGE_CONTENTS_REPO')
            if built:
                repo = Path(built)
            else:
                repo, _, _ = fixture(root)
                mod = repo/'Data/Installed.rte/Index.ini'; mod.parent.mkdir(); mod.write_text('local mod')
                (repo/'Data/save.log').write_text('local save')
            tracked = set(git(repo, 'ls-files', '-z').decode().split('\0'))
            content = {path.relative_to(repo).as_posix() for path in package.package_files(repo)
                       if not (path.parent == repo and path.suffix.lower() in ('.exe', '.dll'))}
            extras = sorted(content - tracked)
            self.assertFalse(bool(extras), f'package contains {len(extras)} untracked files: {extras[:3]}')
            wanted = {name for name in tracked if name.startswith(('Data/', 'Licences/')) or name in ('LICENSE', 'README.md', 'VERSION.txt')}
            self.assertEqual(content, wanted, f'tracked package content missing: {sorted(wanted-content)[:3]}')

    def test_package_resolves_transitive_runtime_imports(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); repo, install, crt = fixture(root)
            with zipfile.ZipFile(self.archive(root, repo, install)) as archive:
                missing = sorted(path.name for path in crt.glob('*.dll') if path.name not in archive.namelist())
                self.assertFalse(missing, f'package has unresolved runtime imports: {missing}')
                manifest = json.loads(archive.read('MANIFEST.json'))
                origins = {entry['path']: entry.get('origin') for entry in manifest['files']}
                for dll in crt.glob('*.dll'):
                    self.assertEqual(origins[dll.name], dict(kind='msvc-redist', version='14.50.35710'))
                self.assertNotIn('clang_rt.asan_dynamic-x86_64.dll', archive.namelist())

    def test_package_receipt_has_only_public_build_fields(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); repo, install, _ = fixture(root, runtime=False)
            with zipfile.ZipFile(self.archive(root, repo, install)) as archive:
                receipt = json.loads(archive.read(package.BUILD_RECEIPT))
                private = sorted(set(receipt)-{'commit', 'executable_sha256', 'configuration', 'version'})
                self.assertFalse(private, f'packaged receipt exposes internal fields: {private}')
                self.assertEqual(receipt['version'], '1.0.0')
                for name in (package.BUILD_RECEIPT, 'MANIFEST.json'):
                    self.assertNotRegex(archive.read(name).decode(), r'(?i)[a-z]:[\\/]|builder|private-build|private-tree')

    def test_tracked_changes_are_refused_before_writing(self):
        for staged in (False, True):
            with self.subTest(staged=staged), tempfile.TemporaryDirectory() as folder:
                root = Path(folder); repo, install, _ = fixture(root, runtime=False)
                (repo/'README.md').write_text('uncommitted content')
                if staged: git(repo, 'add', 'README.md')
                with self.assertRaisesRegex(SystemExit, 'tracked files.*uncommitted'):
                    self.archive(root, repo, install)
                self.assertFalse((root/'out').exists())

    def test_build_commit_and_executable_hash_must_match(self):
        for field in ('commit', 'executable_sha256'):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as folder:
                root = Path(folder); repo, install, _ = fixture(root, runtime=False)
                path = repo/'tools/cross_peers/build.json'; receipt = json.loads(path.read_text())
                receipt[field] = '0'*len(receipt[field]); path.write_text(json.dumps(receipt))
                with self.assertRaisesRegex(SystemExit, 'build receipt commit/hash'):
                    self.archive(root, repo, install)
                self.assertFalse((root/'out').exists())

    def test_missing_transitive_runtime_is_refused_by_name(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); repo, install, crt = fixture(root)
            (crt/'vcruntime140_1.dll').unlink()
            with self.assertRaisesRegex(SystemExit, 'unresolved runtime import: vcruntime140_1.dll'):
                self.archive(root, repo, install)
            self.assertFalse((root/'out').exists())

    def test_unknown_application_import_is_refused_by_name(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); repo, install, _ = fixture(root, runtime=False)
            pe(repo/'support.dll', ('absent-dependency.dll',))
            with self.assertRaisesRegex(SystemExit, 'support.dll: unresolved import: absent-dependency.dll'):
                self.archive(root, repo, install)

    def test_sanitizer_import_is_refused_even_if_it_is_beside_the_executable(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); repo, install, _ = fixture(root, runtime=False)
            pe(repo/'support.dll', ('clang_rt.asan_dynamic-x86_64.dll',))
            with self.assertRaisesRegex(SystemExit, 'sanitizer runtime import'):
                self.archive(root, repo, install)

    def test_runtime_comes_from_the_matching_toolset_family(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); repo, install, crt = fixture(root)
            newer = install/'VC/Redist/MSVC/14.60.99999/x64/Microsoft.VC150.CRT'; newer.mkdir(parents=True)
            (install/'VC/Tools/MSVC/14.60.99999').mkdir(parents=True)
            pe(newer/'msvcp140.dll', ('wrong-family.dll',))
            with patch.object(package, 'visual_studio_installations', return_value=[install]):
                self.assertEqual(package.redistributable_crt(repo/package.EXECUTABLE), (crt, '14.50.35710'))

    def test_absent_matching_toolset_names_the_runtime_import(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); repo, _, _ = fixture(root)
            with patch.object(package, 'visual_studio_installations', return_value=[]):
                with self.assertRaisesRegex(SystemExit, 'unresolved runtime import: .*; no x64 CRT redistributable'):
                    package.package(repo, root/'out')

    def test_import_reader_rejects_truncated_and_other_architectures(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'input.dll'
            path.write_bytes(b'MZ')
            with self.assertRaisesRegex(SystemExit, 'invalid PE: truncated'):
                package.pe_info(path)
            pe(path, machine=0x14c)
            with self.assertRaisesRegex(SystemExit, 'unsupported architecture'):
                package.pe_info(path)

    def test_existing_runtime_is_replaced_by_the_redistributable(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); repo, install, crt = fixture(root)
            pe(repo/'msvcp140.dll', ('wrong-local-runtime.dll',))
            with zipfile.ZipFile(self.archive(root, repo, install)) as archive:
                self.assertEqual(archive.read('msvcp140.dll'), (crt/'msvcp140.dll').read_bytes())

    def test_system_imports_do_not_copy_system_libraries(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); repo, install, _ = fixture(root, runtime=False)
            pe(repo/'support.dll', ('kernel32.dll',))
            with patch.object(package, 'system_dll', side_effect=lambda name: name == 'kernel32.dll'):
                with zipfile.ZipFile(self.archive(root, repo, install)) as archive:
                    self.assertNotIn('kernel32.dll', archive.namelist())


if __name__ == '__main__':
    unittest.main()
