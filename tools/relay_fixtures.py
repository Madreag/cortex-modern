"""The fixture registry the relay sweep excuses shape hits by: every login-shaped value a tracked file defines, as the
SHA-256 of the value and of its defining line, with the file:line and the reason it is synthetic. No value is written.

    python tools/relay_fixtures.py build [--repo <tree>] [--out tools/relay_fixtures.json]
    python tools/relay_fixtures.py check [--repo <tree>] [--registry tools/relay_fixtures.json]

build reads every file git tracks at HEAD (the working copy must match HEAD, the registry itself aside) and records each
shape hit of relay_secrets' patterns; a file with a hit and no reason in REASONS below stops the build, so every entry is
a reviewed decision, never an inference. check rebuilds in memory and exits 1 when the committed registry differs.
"""
from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from relay_secrets import FIXTURES, Finder, logical_line  # noqa: E402

# Each tracked file that holds a login-shaped value, and why that value is synthetic (first match wins).
REASONS = [
    ('tools/acceptance_relay_pair.py', 'relay driver: a keyword-argument line naming settings keys, holding no login value'),
    ('tools/test_relay_build_inputs.py', 'relay retention unit tests: deliberately synthetic sample logins'),
    ('tools/session_directory/test_session_directory.py', "the directory's unit tests: sample relay logins built for those tests"),
    ('tools/e2e/*.menu.txt', 'menu script templates: {TOKEN} placeholders the harness fills at run time'),
    ('tools/e2e/*.json', 'e2e scenario templates and negative fixtures: placeholders and deliberately invalid sample logins'),
    ('tools/test_menu_readback.py', 'the menu readback driver: sample relay logins typed into the boxes under test'),
    ('tools/feel/relay_join.py', 'the relay join driver: a keyword-argument line naming the settings keys, holding no value'),
    ('external/sources/*', 'third-party source: an assignment or format string that only looks like a login'),
]


def git(repo: Path, *args: str) -> str:
    return subprocess.run(['git', '-C', str(repo), *args], capture_output=True, text=True, check=True, timeout=600).stdout


def reason_for(path: str) -> str | None:
    return next((why for pattern, why in REASONS if fnmatch.fnmatch(path, pattern)), None)


def collect(repo: Path) -> tuple[list[dict], list[str]]:
    finder = Finder([], None)
    registry = FIXTURES.relative_to(repo.resolve()).as_posix() if FIXTURES.is_relative_to(repo.resolve()) else None
    dirty = [line[3:] for line in git(repo, 'status', '--porcelain', '--untracked-files=no').splitlines() if line[3:] != registry]
    if dirty:
        raise SystemExit(f'tracked files differ from HEAD: {dirty[:5]}')
    entries, unexplained = [], []
    for name in git(repo, 'ls-files', '-z').split('\0'):
        if not name or name == registry:
            continue
        path = repo / name
        try:
            data = path.read_bytes()
        except OSError:
            continue
        for start, end, kind, _, _ in finder.hits(data):
            why = reason_for(name)
            if why is None:
                unexplained.append(name)
                continue
            value, line = data[start:end], logical_line(data, start, end)
            entries.append(dict(value_sha256=hashlib.sha256(value).hexdigest(), length=len(value), kind=kind,
                                defined_at=f'{name}:{data.count(b"\n", 0, start) + 1}', line_sha256=hashlib.sha256(line).hexdigest(), why=why))
    unique = {(entry['value_sha256'], entry['line_sha256'], entry['defined_at']): entry for entry in entries}
    return sorted(unique.values(), key=lambda entry: (entry['defined_at'], entry['kind'])), sorted(set(unexplained))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=('build', 'check'))
    parser.add_argument('--repo', type=Path, default=HERE.parent)
    parser.add_argument('--out', type=Path, default=FIXTURES)
    parser.add_argument('--registry', type=Path, default=FIXTURES)
    options = parser.parse_args(argv)
    entries, unexplained = collect(options.repo)
    if unexplained:
        print(json.dumps(dict(status='REFUSED', reason='tracked files with login-shaped values and no reason', files=unexplained), indent=1))
        return 2
    document = dict(schema=1, note='SHA-256 of each synthetic value and of its defining line; no value is stored', fixtures=entries)
    if options.command == 'build':
        options.out.write_text(json.dumps(document, indent=1) + '\n', encoding='utf-8')
        print(json.dumps(dict(status='BUILT', entries=len(entries), files=len({entry['defined_at'].rsplit(':', 1)[0] for entry in entries}))))
        return 0
    committed = json.loads(options.registry.read_text(encoding='utf-8')) if options.registry.is_file() else {}
    current = committed.get('fixtures') == entries
    print(json.dumps(dict(status='CURRENT' if current else 'STALE', entries=len(entries), committed=len(committed.get('fixtures', [])))))
    return 0 if current else 1


if __name__ == '__main__':
    raise SystemExit(main())
