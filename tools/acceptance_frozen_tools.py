"""Compose owned row adapters with an immutable, explicitly authorized tool export."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tarfile

from acceptance_runtime import retained_open, storage_scope, write_json

# A run whose bundle carries an export older than its coordinator names that export's commit here.
AUTHORIZED_ENV = 'CC_ACCEPTANCE_FROZEN_COMMIT'


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def authorized_commit(head, authorized=None):
    """The export commit a bundle may carry: the one named, else the bundle's own head."""
    return authorized or os.environ.get(AUTHORIZED_ENV) or head


def receipt(root, *, verify=True, authorized=None):
    root = Path(root)
    path = root/'acceptance-tools.json'
    if not path.is_file():
        return None
    value = json.loads(path.read_text(encoding='utf-8'))
    expected = authorized_commit(value.get('coordinator_commit'), authorized)
    if not expected or value.get('frozen_commit') != expected:
        raise ValueError(f'tool bundle exports {value.get("frozen_commit")} but the authorized export is {expected}')
    if verify:
        for entry in value['files']:
            name = Path(entry['path'])
            if name.is_absolute() or '..' in name.parts or digest(root/name) != entry['sha256']:
                raise ValueError('frozen/owned tool bundle bytes differ: '+str(name))
    return value


@contextmanager
def driver_git_context(root):
    """Record the owned coordinator commit separately from the frozen tool commit."""
    value = receipt(root)
    if value is None:
        yield None
        return
    repo = Path(value['coordinator_source_repo'])
    git_dir = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', '--absolute-git-dir'], text=True).strip()
    previous = {key: os.environ.get(key) for key in ('GIT_DIR', 'GIT_WORK_TREE')}
    try:
        os.environ.update(GIT_DIR=git_dir, GIT_WORK_TREE=str(repo))
        yield value
    finally:
        for key, old in previous.items():
            if old is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = old


def helper_archive(repo, root):
    repo, root = Path(repo), Path(root)
    value = receipt(repo)
    if value is None:
        from world_soak_tasks import helper_archive as ordinary_archive
        return ordinary_archive(repo, root)
    archive = root/'helpers.tar'
    with retained_open(archive) as output:
        with tarfile.open(fileobj=output, mode='w') as stream:
            for name in [entry['path'] for entry in value['files']] + ['acceptance-tools.json']:
                stream.add(repo/name, arcname=name, recursive=False)
    return archive


def compose(repo, export, out, authorized=None):
    repo, export, out = Path(repo).resolve(), Path(export).resolve(), Path(out).resolve()
    frozen = json.loads((export/'export.json').read_text(encoding='utf-8'))
    current = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip()
    authorized = authorized_commit(current, authorized)
    if frozen.get('source_commit') != authorized:
        raise ValueError(f'the export is of {frozen.get("source_commit")} but the authorized export is {authorized}')
    added = subprocess.check_output(['git', '-C', str(repo), 'diff', '--name-only', '--diff-filter=A', '-z',
                                     authorized, current, '--', 'tools'], text=True).split('\0')
    original_names = {entry['path'] for entry in frozen['files']}
    if original_names.intersection(name for name in added if name):
        raise ValueError('owned adapters cannot replace a frozen export file')
    files = []
    out.mkdir(parents=True, exist_ok=False)
    def keep(name, data, origin):
        target = out/name
        target.parent.mkdir(parents=True, exist_ok=True)
        with retained_open(target) as stream:
            stream.write(data)
        files.append(dict(path=name, bytes=len(data), sha256=hashlib.sha256(data).hexdigest(), origin=origin))
    for entry in frozen['files']:
        source = export/entry['path']
        if digest(source) != entry['sha256']:
            raise ValueError('original frozen export changed: '+entry['path'])
        keep(entry['path'], source.read_bytes(), 'frozen')
    for name in added:
        if name:
            keep(name, subprocess.check_output(['git', '-C', str(repo), 'show', current+':'+name]), 'owned')
    # This retained committed header supplies the public-directory default only.
    # Native build/content identity still comes from each separate engine tree.
    header = 'Source/Managers/SettingsMan.h'
    keep(header, subprocess.check_output(['git', '-C', str(repo), 'show', current+':'+header]), 'driver-config')
    value = dict(frozen_commit=authorized, frozen_export=str(export),
                 coordinator_commit=current, coordinator_source_repo=str(repo), files=files,
                 frozen_files_modified=0, authorization='the export of '+authorized)
    write_json(out/'acceptance-tools.json', value)
    receipt(out, authorized=authorized)
    return value


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--export', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--scratch', type=Path, required=True)
    parser.add_argument('--frozen-commit', help='the commit the export was made from (default: the repo HEAD)')
    args = parser.parse_args(argv)
    if not args.out.resolve().is_relative_to(args.scratch.resolve()):
        parser.error('the composed bundle must stay inside this lane scratch')
    with storage_scope(args.scratch, reserve=1024**2) as budget:
        value = compose(args.repo, args.export, args.out, args.frozen_commit)
        print(json.dumps(dict(frozen_commit=value['frozen_commit'], coordinator_commit=value['coordinator_commit'],
                              files=len(value['files']), retained_bytes=budget.used)))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
