"""Retain large text records losslessly once their writers have closed them."""
from __future__ import annotations

import gzip
import hashlib
import json
from pathlib import Path
import re
import zipfile


def inspect_checkpoint(path):
    """Check archive CRCs and named identity; do not claim restoration or admission validity."""
    path = Path(path)
    result = dict(path=str(path), passed=False, scope='CRC, required entries, world/descriptor/manifest identity; not a restore or admission-seal check')
    def properties(text):
        values = {}
        for line in text.splitlines():
            if match := re.match(r'^\s*([A-Za-z]+)\s*=\s*(.*?)\s*$', line):
                if match[1] in values and match[1] in {'SavedTick','MatchId','SessionId','RoundId','ManifestSchema','ConfigHash','ConfigPayload','RestoreSchema'}:
                    raise ValueError('duplicate checkpoint property '+match[1])
                values[match[1]] = match[2]
        return values
    try:
        with path.open('rb') as source:
            digest = hashlib.sha256()
            for block in iter(lambda:source.read(1024*1024),b''): digest.update(block)
            result['sha256'] = digest.hexdigest()
        result['bytes'] = path.stat().st_size
        with zipfile.ZipFile(path) as archive:
            names = archive.namelist()
            required = {'Save.ini','Index.ini','Save Mat.png','Save FG.png','Save BG.png','Restore.ini'}
            if len(names) != len(set(names)) or not required <= set(names): raise ValueError('missing or duplicate archive entries')
            if bad := archive.testzip(): raise ValueError('CRC mismatch: '+bad)
            descriptor = properties(archive.read('Restore.ini').decode('utf-8-sig'))
            save = archive.read('Save.ini').decode('utf-8-sig')
            world_tick = re.search(r'(?m)^\s*SimUpdateCount\s*=\s*(\d+)\s*$', save)
            tick = int(descriptor['SavedTick']); match_id = descriptor['MatchId']
            if tick < 1 or descriptor.get('RestoreSchema') != '1': raise ValueError('unsupported descriptor schema or tick')
            if path.name != f'{match_id}-{tick}.ccsave' or not world_tick or int(world_tick[1]) != tick:
                raise ValueError('archive/world/descriptor tick identity differs')
            if 'RuntimeGlobals = ' not in save or 'LuaStateGraph = ' not in save: raise ValueError('missing runtime or script graph')
            result.update(match_id=match_id,tick=tick,session=descriptor['SessionId'],source_round=int(descriptor['RoundId']),
                          module_hash=descriptor.get('ModuleManifestHash'),world_structure_hash=descriptor.get('WorldStructureHash'))
        manifest_path = path.with_suffix('.ccmanifest')
        manifest = properties(manifest_path.read_text(encoding='utf-8-sig'))
        if any(manifest.get(key) != descriptor.get(key) for key in ('MatchId','SessionId','RoundId','SavedTick')):
            raise ValueError('manifest and descriptor identity differ')
        if manifest.get('ManifestSchema') not in ('2','3') or not manifest.get('ConfigHash') or not manifest.get('ConfigPayload'):
            raise ValueError('manifest lacks config payload/hash or has unsupported schema')
        result.update(passed=True,manifest_path=str(manifest_path),config_hash=manifest['ConfigHash'],
                      manifest_sha256=hashlib.sha256(manifest_path.read_bytes()).hexdigest())
    except (OSError, ValueError, KeyError, zipfile.BadZipFile, RuntimeError) as error:
        result['reason'] = str(error)
    return result


class RecordWriter:
    """Bound compressed chunks without discarding a record or following a link."""

    def __init__(self, base, chunk_bytes=4 * 1024 * 1024, total_bytes=256 * 1024 * 1024):
        self.base = Path(base)
        if chunk_bytes <= 0 or total_bytes <= 0:
            raise ValueError('record budgets must be positive')
        self.chunk_bytes, self.total_bytes = chunk_bytes, total_bytes
        self.parts, self.stream, self.size, self.total, self.lines = [], None, 0, 0, 0
        self.index = self.base.with_name(self.base.name + '.index.json')
        if self.index.exists():
            raise FileExistsError(self.index)

    def write(self, row):
        data = (json.dumps(row, allow_nan=False, separators=(',', ':')) + '\n').encode('utf-8')
        if self.total + len(data) > self.total_bytes:
            raise RuntimeError(f'record byte budget exceeded: {self.base}')
        if self.stream is not None and self.size + len(data) > self.chunk_bytes:
            self._seal()
        if self.stream is None:
            self.path = self.base.with_name(f'{self.base.name}.{len(self.parts):05d}.jsonl.gz')
            self.raw = self.path.open('xb')
            self.stream = gzip.GzipFile(filename='', fileobj=self.raw, mode='wb', compresslevel=3, mtime=0)
            self.digest = hashlib.sha256()
            self.size = self.lines = 0
        self.stream.write(data)
        self.digest.update(data)
        self.size += len(data); self.total += len(data); self.lines += 1

    def _seal(self):
        if self.stream is None:
            return
        self.stream.close(); self.raw.close(); self.stream = None
        self.parts.append(dict(path=self.path.name, bytes=self.size, lines=self.lines,
                               sha256=self.digest.hexdigest(), compressed_bytes=self.path.stat().st_size))
        temporary = self.index.with_suffix('.tmp')
        temporary.write_text(json.dumps(dict(version=1, complete=False, parts=self.parts,
                                             uncompressed_bytes=self.total)), encoding='utf-8')
        temporary.replace(self.index)

    def close(self):
        self._seal()
        self.index.write_text(json.dumps(dict(version=1, complete=True, parts=self.parts,
                                             uncompressed_bytes=self.total)), encoding='utf-8')

    def __enter__(self):
        return self

    def __exit__(self, *exception):
        self.close()
        if exception[0] is not None:
            value = json.loads(self.index.read_text(encoding='utf-8')); value['complete'] = False
            self.index.write_text(json.dumps(value), encoding='utf-8')


def read_records(index):
    index = Path(index)
    document = json.loads(index.read_text(encoding='utf-8'))
    for part in document['parts']:
        path = index.parent / part['path']
        if path.is_symlink() or path.parent.resolve() != index.parent.resolve():
            raise ValueError('record chunk leaves its directory')
        digest, size, lines = hashlib.sha256(), 0, 0
        with gzip.open(path, 'rb') as stream:
            for raw in stream:
                digest.update(raw); size += len(raw); lines += 1
                yield json.loads(raw)
        if digest.hexdigest() != part['sha256'] or size != part['bytes'] or lines != part['lines']:
            raise ValueError(f'record chunk digest or count differs: {path}')


def record_path(path):
    path = Path(path)
    packed = path.with_name(path.name + '.gz')
    return packed if not path.is_file() and packed.is_file() else path


def open_record(path, mode='rt', **kwargs):
    path = record_path(path)
    return gzip.open(path, mode, **kwargs) if path.suffix == '.gz' else path.open(mode, **kwargs)


def compress_closed_record(path, root):
    """Pack a writer-sealed file, verify it, then retire only that owned plain file."""
    path, root = Path(path), Path(root).resolve()
    if path.is_symlink() or not path.resolve().is_relative_to(root):
        raise ValueError('record leaves its owning instance')
    size_before = path.stat().st_size
    destination = path.with_name(path.name + '.gz')
    temporary = path.with_name(path.name + '.gz.partial')
    digest, size = hashlib.sha256(), 0
    with path.open('rb') as source, temporary.open('xb') as sink:
        with gzip.GzipFile(filename='', fileobj=sink, mode='wb', compresslevel=3, mtime=0) as packed:
            for block in iter(lambda: source.read(1024*1024), b''):
                digest.update(block); size += len(block); packed.write(block)
    restored, restored_size = hashlib.sha256(), 0
    with gzip.open(temporary, 'rb') as source:
        for block in iter(lambda: source.read(1024*1024), b''):
            restored.update(block); restored_size += len(block)
    if size != size_before or path.stat().st_size != size_before or size != restored_size or digest.digest() != restored.digest():
        raise RuntimeError('sealed record changed or compression did not preserve it')
    if destination.exists(): raise FileExistsError(destination)
    temporary.replace(destination)
    receipt = dict(original=str(path.relative_to(root)), retained=str(destination.relative_to(root)), original_bytes=size,
                   original_sha256=digest.hexdigest(), compressed_bytes=destination.stat().st_size)
    with (root / 'compressed-records.jsonl').open('a', encoding='utf-8') as index:
        index.write(json.dumps(receipt)+'\n')
    path.unlink()
    return receipt


class CaptureSealer:
    """Pack completed captures while an engine runs, without touching active dumps.

    ActivityMan emits fullstate-scope after FullStateHashLine returns and closes
    every section file. Repeated keys with several queued paths are ambiguous;
    those captures remain raw until the owning process exits.
    """
    def __init__(self, own):
        self.own=Path(own).resolve()
        self.log=self.own/'engine/stdout.log'
        self.offset=0; self.pending=b''; self.contexts={}; self.sealed=set()

    def poll(self):
        if not self.log.is_file(): return
        with self.log.open('rb') as stream:
            stream.seek(self.offset); block=stream.read(4*1024*1024); self.offset+=len(block)
        lines=(self.pending+block).split(b'\n'); self.pending=lines.pop()
        for raw in lines:
            line=raw.decode('utf-8',errors='replace').strip()
            if match:=re.match(r'^\[fullstate-context\] tick=(\d+) round=(\d+) label=(\S+) path=(.+)$',line):
                key=match.group(1,2,3)
                path=Path(match[4]).resolve()/match[1]
                if not path.is_relative_to(self.own/'fullstate'):
                    raise ValueError('capture context leaves its owning instance')
                self.contexts.setdefault(key,[]).append(path)
            elif match:=re.match(r'^\[fullstate-scope\] tick=(\d+) round=(\d+) label=(\S+) per_peer=',line):
                paths=self.contexts.get(match.group(1,2,3),[])
                if len(paths)!=1 or paths[0] in self.sealed: continue
                path=paths[0]
                if not path.is_dir(): continue
                for section in sorted(path.glob('*.txt')):
                    compress_closed_record(section,self.own)
                self.sealed.add(path)


def compress_case_records(root):
    root = Path(root).resolve()
    paths = [*root.glob('*_controller.jsonl'), *root.glob('*.simdump.txt'), *root.glob('*/feel/raw.jsonl')]
    records = []
    for path in paths:
        if path.is_symlink() or not path.resolve().is_relative_to(root):
            raise ValueError(f'record leaves its run directory: {path}')
        packed = path.with_name(path.name + '.gz')
        digest, size = hashlib.sha256(), 0
        with path.open('rb') as source, packed.open('xb') as target:
            with gzip.GzipFile(filename='', fileobj=target, mode='wb', compresslevel=3, mtime=0) as stream:
                while block := source.read(1024 * 1024):
                    digest.update(block)
                    size += len(block)
                    stream.write(block)
        check, restored_size = hashlib.sha256(), 0
        with gzip.open(packed, 'rb') as stream:
            while block := stream.read(1024 * 1024):
                check.update(block)
                restored_size += len(block)
        if restored_size != size or check.digest() != digest.digest():
            raise RuntimeError(f'compressed record did not reproduce its original bytes: {path}')
        records.append(dict(original=str(path), retained=str(packed), original_bytes=size,
                            original_sha256=digest.hexdigest(), compressed_bytes=packed.stat().st_size))
        path.unlink()
    if records:
        (root / 'compressed-records.json').write_text(json.dumps(records, indent=2) + '\n', encoding='utf-8')
    return records
