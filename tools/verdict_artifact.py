"""Small hashable verdicts for drivers whose primary output is a log."""
import hashlib
import json
from pathlib import Path


def sha256(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write_verdict(path, *, passed, counts, inputs, log, **details):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    hashes = {str(Path(value).resolve()): sha256(value) for value in inputs if Path(value).is_file()}
    document = dict(schema=1, passed=bool(passed), verdict='PASS' if passed else 'FAIL', counts=counts,
                    input_sha256=hashes, log=str(Path(log).resolve()), log_sha256=sha256(log), **details)
    path.write_text(json.dumps(document, indent=2) + '\n', encoding='utf-8')
    return document
