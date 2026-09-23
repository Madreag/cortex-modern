"""Retain large text records losslessly after their engine processes have exited."""
from __future__ import annotations

import gzip
import hashlib
import json
from pathlib import Path


def record_path(path):
    path = Path(path)
    packed = path.with_name(path.name + '.gz')
    return packed if not path.is_file() and packed.is_file() else path


def open_record(path, mode='rt', **kwargs):
    path = record_path(path)
    return gzip.open(path, mode, **kwargs) if path.suffix == '.gz' else path.open(mode, **kwargs)


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
