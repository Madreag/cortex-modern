"""Measure a stream's executable and bind it to its native build receipt before rows start."""
import argparse
from datetime import datetime, timedelta, timezone
import hashlib
import json
from pathlib import Path
import platform
import re
import subprocess


BOXES = ('EROL-PC', 'EDITH', 'Mac', 'Linux', 'Z13', 'ALLY')


def sha256(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def identity(box, repo, executable, receipt, source_sha, collection_id=None):
    errors, build, measured = [], {}, None
    if box not in BOXES: errors.append(f'node/box name maps to no BOXES.md role: {box!r}')
    if not isinstance(source_sha, str) or not re.fullmatch('[0-9a-f]{40}', source_sha): errors.append('source_sha is not a full commit')
    try:
        measured = sha256(executable)
        build = json.loads(Path(receipt).read_text(encoding='utf-8-sig'))
        if build.get('commit') != source_sha or build.get('executable_sha256') != measured:
            errors.append('build receipt commit/hash differs from the requested tip or measured executable')
    except (OSError, ValueError, TypeError) as error:
        errors.append(f'{type(error).__name__}: {error}')
    head = subprocess.run(['git', '-C', str(repo), 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    return dict(schema=1, box=box, node=platform.node(), platform=platform.system(), os=platform.system(),
                source_sha=source_sha, head=head, executable=str(Path(executable).resolve()), executable_sha256=measured,
                build=build, build_receipt=str(Path(receipt).resolve()), collection_id=collection_id,
                status='INCOMPLETE' if errors else 'PASS', errors=errors,
                measured=datetime.now(timezone(timedelta(hours=-7))).strftime('%Y-%m-%d %I:%M:%S %p MST'))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--box', required=True, choices=BOXES)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--build-receipt', type=Path, required=True)
    parser.add_argument('--source-sha', required=True)
    parser.add_argument('--collection-id')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--write-build-receipt', action='store_true', help='record a just-completed successful native build')
    parser.add_argument('--build-exit-code', type=int)
    parser.add_argument('--build-log', type=Path)
    parser.add_argument('--configuration', default='release')
    options = parser.parse_args(argv)
    if options.write_build_receipt:
        actual = subprocess.check_output(['git', '-C', str(options.repo), 'rev-parse', 'HEAD'], text=True).strip()
        if options.build_exit_code != 0 or not options.build_log or not options.build_log.is_file() or actual != options.source_sha:
            parser.error('a successful build log and the exact native source tip are required to write a build receipt')
        if options.build_receipt.exists(): parser.error('the build receipt already exists; retain it and use a new build path')
        options.build_receipt.parent.mkdir(parents=True, exist_ok=True)
        options.build_receipt.write_text(json.dumps(dict(commit=actual, executable_sha256=sha256(options.exe),
            configuration=options.configuration, build_exit_code=0, build_log=str(options.build_log.resolve()),
            build_log_sha256=sha256(options.build_log)), indent=2) + '\n', encoding='utf-8')
    result = identity(options.box, options.repo, options.exe, options.build_receipt, options.source_sha, options.collection_id)
    options.out.parent.mkdir(parents=True, exist_ok=True)
    options.out.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(f'identity {result["box"]}: {result["status"]}; ' + '; '.join(result['errors']))
    return int(bool(result['errors']))


if __name__ == '__main__':
    raise SystemExit(main())
