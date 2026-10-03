"""The lane's own sweep: every file a relay lane keeps, here and on a game box, swept for every relay login we hold - the
Cloudflare key and token, the directory's coturn secret, the retired fixed account - by exact value in every encoded form
and by every login shape (relay_secrets.sweep). A box receives salted digests of the values, never the values; its sweep
runs there and only its counts come back. The receipt holds counts, kinds and paths, never a value.

    python tools/relay_lane_sweep.py --root <dir> [--root ...] [--box edith=<dir> ...] [--scrub] --out <receipt.json>

Exit 0 when every root is CLEAN, 3 when any is INCOMPLETE (a file or directory that could not be read), 1 on a hit.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from relay_secrets import SecretBook, read_turn_config, repository_public, sweep  # noqa: E402

SECRETS_DIR = Path('D:/mx/coturn-20260920')
REPO = HERE.parent
BOOK_SOURCES = (('turn', SECRETS_DIR / 'turn-config-cloudflare.json'), ('turn', SECRETS_DIR / 'directory-coturn.json'),
                ('fixed', SECRETS_DIR / 'turnserver-fixed.conf'))


def lane_book() -> tuple[SecretBook, list[str]]:
    book, read = SecretBook(), []
    for kind, path in BOOK_SOURCES:
        if not path.is_file():
            continue
        if kind == 'turn':
            book.add_turn_config(read_turn_config(path))
        else:
            book.add_fixed_login(path)
        read.append(path.name)
    return book, read


def summary(result: dict) -> dict:
    return dict(status=result['status'], files=result['files_scanned'], hits=len(result['files_with_secrets']),
                incomplete=len(result['incomplete']), kinds=sorted({kind for row in result['files_with_secrets'] for kind in row['kinds']}),
                forms=sorted({form for row in result['files_with_secrets'] for form in row.get('forms', [])}),
                paths=[row['path'] for row in result['files_with_secrets']], incomplete_paths=result['incomplete'][:20],
                public_files=len(result.get('public', [])), public_hits=sum(row['hits'] for row in result.get('public', [])),
                public=result.get('public', []))


def ssh(alias: str, command: str, timeout: int = 1800) -> subprocess.CompletedProcess:
    return subprocess.run(['ssh', '-o', 'BatchMode=yes', alias, command], capture_output=True, text=True, timeout=timeout,
                          creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))


def sweep_box(alias: str, root: str, book: SecretBook, scrub: bool, public_repo: str | None = None) -> dict:
    """The box's sweep: relay_secrets.py and the salted digests copied in, the sweep run there, the digests removed."""
    stamp = time.strftime('%Y%m%d-%H%M%S')
    tool, digests, out = f'{root}/payload/relay_secrets.py', f'{root}/payload/sweep-digests-{stamp}.json', f'{root}/sweep-{stamp}.json'
    with tempfile.TemporaryDirectory() as folder:
        local = Path(folder) / 'digests.json'
        local.write_text(json.dumps(book.digests()), encoding='utf-8')
        ssh(alias, f"New-Item -ItemType Directory -Force '{root}/payload' | Out-Null")
        for source, target in ((HERE / 'relay_secrets.py', tool), (local, digests)):
            copied = subprocess.run(['scp', '-q', '-o', 'BatchMode=yes', str(source), f'{alias}:{target}'], capture_output=True, text=True, timeout=300)
            if copied.returncode:
                return dict(box=alias, root=root, status='INCOMPLETE', error=f'scp exit {copied.returncode}')
    try:
        done = ssh(alias, f"python '{tool}' sweep --root '{root}' --digests '{digests}' {'--scrub ' if scrub else ''}"
                          f"{f'--public-repo {chr(39)}{public_repo}{chr(39)} ' if public_repo else ''}--out '{out}'", timeout=4 * 3600)
    finally:
        ssh(alias, f"Remove-Item -LiteralPath '{digests}' -Force -ErrorAction SilentlyContinue")
    line = next((text for text in reversed(done.stdout.splitlines()) if text.startswith('{')), '{}')
    counts = json.loads(line)
    status = counts.get('status', 'INCOMPLETE') if done.returncode in (0, 1, 3) else 'INCOMPLETE'
    return dict(box=alias, root=root, exit_code=done.returncode, status=status, files=counts.get('files'), hits=counts.get('hits'),
                after=counts.get('after'), incomplete=counts.get('incomplete'), receipt_on_box=out)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--root', type=Path, action='append', default=[])
    parser.add_argument('--box', action='append', default=[], metavar='ALIAS=DIR[@REPO]',
                        help="a box's root; @REPO names that box's repository tree for the published-value check")
    parser.add_argument('--scrub', action='store_true')
    parser.add_argument('--out', type=Path, required=True)
    options = parser.parse_args(argv)
    book, read = lane_book()
    if not len(book):
        print(json.dumps(dict(status='INCOMPLETE', reason='no secret could be read into the book')))
        return 3
    rows = []
    for alias, target in (pair.split('=', 1) for pair in options.box):
        root, _, repo = target.partition('@')
        rows.append(sweep_box(alias, root, book, options.scrub, repo or None))
    public = repository_public(REPO)
    for root in options.root:
        first = sweep([root], book.finder(), scrub=options.scrub, public=public)
        final = sweep([root], book.finder(), public=public) if options.scrub else first
        rows.append(dict(box='here', root=str(root), first=summary(first), **summary(final)))
    result = dict(made=time.strftime('%Y-%m-%d %I:%M:%S %p'), book_sources=read, book_kinds=book.kinds(), book_values=len(book), roots=rows)
    options.out.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps([{key: row.get(key) for key in ('box', 'root', 'status', 'files', 'hits', 'incomplete', 'public_files', 'public_hits')}
                      for row in rows]))
    states = {row['status'] for row in rows}
    return 0 if states == {'CLEAN'} else 1 if 'LEAKED' in states else 3


if __name__ == '__main__':
    raise SystemExit(main())
