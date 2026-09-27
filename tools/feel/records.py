"""Retain large text records losslessly after their engine processes have exited."""
from __future__ import annotations

import gzip
import hashlib
import json
from pathlib import Path


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
