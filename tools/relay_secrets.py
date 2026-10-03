"""Relay logins kept in memory, and the gate that proves no retained file holds one.

    python tools/relay_secrets.py sweep --root <dir> [--root <dir> ...] [--digests <book.json>] [--scrub] --out <receipt.json>

The Cloudflare key file, a coturn secret and every login a run mints or observes are read into a SecretBook; the book
never prints, logs or writes a value. A box that must be swept without the values gets a DigestBook: a per-run salt and
the salted SHA-256 of each value, so no value travels.

sweep() reads every regular file under the roots (a junction, symlink or other reparse point is neither entered nor read;
nothing is skipped for its size or extension) and looks at each in every form a login can take there: the raw bytes, the
JSON-escaped and UTF-16 forms, hex-encoded payloads (a resume manifest's ConfigPayload) decoded, and the members of
gzip, zip and tar archives opened. A hit is the exact book (any length; a short value only at a token boundary), the
digest book (tokens of a booked length hashed with the salt) or a login's own shape: a JSON username/credential field,
an INI relay login key, a coturn REST username (expiry:24-hex tag), a 64-hex string after a login key. A file that
cannot be read, a directory that cannot be listed or an archive that cannot be opened makes the sweep INCOMPLETE, never
clean. With scrub, a hit is overwritten in place with the same number of 'x' (in its hex form for a hex payload) and an
archive holding one is replaced by a receipt; the file is swept again and must come back clean. Reports name the file,
the form and the kind, never a value.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
import os
import re
import secrets as random_source
import stat
import subprocess
import sys
import tarfile
import zipfile
import zlib
from pathlib import Path
from typing import Any, Iterable, Iterator

REPARSE = getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0x400)
SHORT = 8  # a value shorter than this matches only between token boundaries
TOKEN_CHARS = rb'A-Za-z0-9+/=_:.\-!#$%&*@^~'
TOKENS_WIDE = re.compile(rb'[' + TOKEN_CHARS + rb']+')
TOKENS_NARROW = re.compile(rb'[A-Za-z0-9+/_.\-!#$%&*@^~]+')
BOUNDARY = re.compile(rb'[A-Za-z0-9+/_\-]')
HEX_RUN = re.compile(rb'(?:[0-9a-fA-F]{2}){32,}')
MAX_DECODED = 1 << 30
MAX_DEPTH = 3
PLACEHOLDER = re.compile(rb'^(?:x+|redacted|<[^>]*>|REPLACE_WITH[A-Z_]*|\*+)?$')
PATTERNS = {
    # A JSON login field, and the same field escaped inside another JSON string.
    'json-login-field': re.compile(rb'"(username|credential)"\s*:\s*"((?:[^"\\]|\\.)*)"'),
    'escaped-json-login-field': re.compile(rb'\\"(username|credential)\\"\s*:\s*\\"((?:[^"\\]|\\\\(?:\\\\|\\"))*)\\"'),
    # The same settings keys as JSON (a harness spec or receipt carrying a peer's settings), plain or escaped.
    'json-relay-setting': re.compile(rb'(?i)"(NetworkTurnUser|NetworkTurnPass|NetworkPlayerTurnUser|NetworkPlayerTurnPass)"\s*:\s*"((?:[^"\\]|\\.)*)"'),
    'escaped-json-relay-setting': re.compile(
        rb'(?i)\\"(NetworkTurnUser|NetworkTurnPass|NetworkPlayerTurnUser|NetworkPlayerTurnPass)\\"\s*:\s*\\"((?:[^"\\]|\\\\(?:\\\\|\\"))*)\\"'),
    # A relay login written into an INI (Settings.ini) by the game or a harness.
    'ini-relay-login': re.compile(rb'(?mi)^[ \t]*(NetworkTurnUser|NetworkTurnPass|NetworkPlayerTurnUser|NetworkPlayerTurnPass)[ \t]*=[ \t]*([^\r\n]*?)[ \t]*\r?$'),
    # coturn's REST username as the directory mints it: expiry seconds, a colon, a 24-hex tag.
    'coturn-rest-username': re.compile(rb'(?<![0-9])(1[0-9]{9}:[0-9a-f]{24})(?![0-9a-f])'),
    # A 64-hex value right after a login key (Cloudflare's minted username and credential are 64 hex characters).
    'hex64-after-login-key': re.compile(rb'(?i)(username|credential|turnuser|turnpass|password|user)[^A-Za-z0-9]{1,8}([0-9a-f]{64})(?![0-9a-f])'),
    # A coturn account or REST secret line.
    'coturn-secret-line': re.compile(rb'(?mi)^[ \t]*(user|static-auth-secret)[ \t]*=[ \t]*(\S+)'),
}


def read_turn_config(path: Path | str) -> dict[str, Any]:
    """The directory's TURN backend file, read by path; the caller hands the dict to the directory and the book."""
    config = json.loads(Path(path).read_text(encoding='utf-8'))
    if not isinstance(config, dict):
        raise ValueError(f'{Path(path).name}: not a TURN backend object')
    return config


def read_fixed_login(conf: Path | str) -> tuple[str, str]:
    """The first user=<name>:<password> line of a coturn config: for the book only, never for a run."""
    for line in Path(conf).read_text(encoding='utf-8', errors='replace').splitlines():
        match = re.match(r'\s*user\s*=\s*([^:\s]+):(\S+)', line)
        if match:
            return match[1], match[2]
    raise ValueError(f'{Path(conf).name} has no user=<name>:<password> line')


def variants(value: bytes) -> set[bytes]:
    """The forms a value takes in a file: itself, JSON-escaped once and twice, UTF-16, hex."""
    text = value.decode('utf-8', 'surrogateescape')
    forms = {value}
    once = json.dumps(text)[1:-1]
    forms.add(once.encode('utf-8', 'surrogateescape'))
    forms.add(json.dumps(once)[1:-1].encode('utf-8', 'surrogateescape'))
    forms.add(text.encode('utf-16-le', 'surrogatepass'))
    forms.add(value.hex().encode())
    forms.add(value.hex().upper().encode())
    return forms


class SecretBook:
    """Named secrets held in memory; nothing it reports carries a value."""

    def __init__(self) -> None:
        self._values: dict[bytes, str] = {}

    def add(self, kind: str, value: Any) -> None:
        if isinstance(value, str) and value and not PLACEHOLDER.match(value.encode('utf-8', 'replace')):
            self._values.setdefault(value.encode('utf-8'), kind)

    def add_turn_config(self, config: dict[str, Any]) -> None:
        for key in ('api_token', 'turn_key_id', 'static_auth_secret'):
            self.add(f'backend-{key}', config.get(key))

    def add_offer(self, offer: dict[str, Any], kind: str = 'minted') -> None:
        for server in offer.get('iceServers', []) if isinstance(offer, dict) else []:
            self.add(f'{kind}-username', server.get('username'))
            self.add(f'{kind}-credential', server.get('credential'))

    def add_fixed_login(self, conf: Path | str) -> None:
        user, password = read_fixed_login(conf)
        self.add('fixed-username', user)
        self.add('fixed-password', password)

    def kinds(self) -> list[str]:
        return sorted(set(self._values.values()))

    def __len__(self) -> int:
        return len(self._values)

    def usernames(self, kind_suffix: str = '-username') -> list[str]:
        return [value.decode() for value, kind in self._values.items() if kind.endswith(kind_suffix)]

    def digests(self, salt: bytes | None = None) -> dict[str, Any]:
        """The book as a box may carry it: a fresh salt and each value's salted SHA-256 and length, never the value."""
        salt = salt or random_source.token_bytes(16)
        return dict(salt=salt.hex(), items=[dict(kind=kind, length=len(value), sha256=hashlib.sha256(salt + value).hexdigest())
                                            for value, kind in self._values.items()])

    def finder(self) -> 'Finder':
        return Finder([(form, len(form) < SHORT, kind) for value, kind in self._values.items() for form in variants(value)], None)

    def scan(self, roots: Iterable[Path | str], scrub: bool = False) -> dict[str, Any]:
        return sweep(roots, self.finder(), scrub=scrub)


class DigestBook:
    """A book a box can carry: salted digests of the values; a token of a booked length is hashed and compared."""

    def __init__(self, document: dict[str, Any]) -> None:
        self.salt = bytes.fromhex(document['salt'])
        self.by_digest = {item['sha256']: item['kind'] for item in document['items']}
        self.lengths = {int(item['length']) for item in document['items']}

    def finder(self) -> 'Finder':
        return Finder([], self)

    def kind(self, token: bytes) -> str | None:
        return self.by_digest.get(hashlib.sha256(self.salt + token).hexdigest()) if len(token) in self.lengths else None


class Finder:
    """Every login hit in one view of a file: (start, end, kind, how) with offsets into that view."""

    def __init__(self, exact: list[tuple[bytes, bool, str]], digests: DigestBook | None) -> None:
        self.exact, self.digests = exact, digests

    def hits(self, data: bytes) -> list[tuple[int, int, str, str]]:
        found: list[tuple[int, int, str, str]] = []
        for form, short, kind in self.exact:
            start = data.find(form)
            while start >= 0:
                end = start + len(form)
                if not short or ((start == 0 or not BOUNDARY.match(data[start - 1:start]))
                                 and (end == len(data) or not BOUNDARY.match(data[end:end + 1]))):
                    found.append((start, end, kind, 'book'))
                start = data.find(form, start + 1)
        if self.digests:
            for tokens in (TOKENS_WIDE, TOKENS_NARROW):
                for match in tokens.finditer(data):
                    kind = self.digests.kind(match.group(0))
                    if kind:
                        found.append((match.start(), match.end(), kind, 'digest'))
        for name, pattern in PATTERNS.items():
            for match in pattern.finditer(data):
                group = match.lastindex or 0
                value = match.group(group)
                if value and not PLACEHOLDER.match(value.strip(b'\\')):
                    found.append((match.start(group), match.end(group), f'shape:{name}', 'pattern'))
        return found


def is_reparse(entry: os.DirEntry) -> bool:
    if entry.is_symlink() or (hasattr(entry, 'is_junction') and entry.is_junction()):
        return True
    try:
        return bool(getattr(entry.stat(follow_symlinks=False), 'st_file_attributes', 0) & REPARSE)
    except OSError:
        return True


def walk(root: Path) -> Iterator[tuple[Path, str | None]]:
    """(file, None) for every regular file under root and (path, reason) for what could not be listed; a junction,
    symlink or other reparse point is reported as skipped by design, never entered."""
    root = Path(root)
    if root.is_file():
        yield root, None
        return
    if not root.is_dir():
        yield root, 'root missing'
        return
    stack = [root]
    while stack:
        directory = stack.pop()
        try:
            entries = list(os.scandir(directory))
        except OSError as error:
            yield directory, f'directory unreadable: {type(error).__name__}'
            continue
        for entry in entries:
            if is_reparse(entry):
                continue
            try:
                if entry.is_dir(follow_symlinks=False):
                    stack.append(Path(entry.path))
                elif entry.is_file(follow_symlinks=False):
                    yield Path(entry.path), None
            except OSError as error:
                yield Path(entry.path), f'entry unreadable: {type(error).__name__}'


def walk_files(root: Path) -> Iterator[Path]:
    """Every regular file under root (listing errors are the sweep's to report; this helper keeps the old callers)."""
    for path, reason in walk(Path(root)):
        if reason is None:
            yield path


def views(data: bytes, depth: int = 0) -> Iterator[tuple[str, bytes, Any]]:
    """(form, bytes, back) for each form a login can take in these bytes; back maps a span of the view to a span of the
    original bytes, or is None when the view is an archive member (only the whole file can be cleaned)."""
    yield 'raw', data, (lambda start, end: (start, end))
    head = data[:4096]
    if head.count(b'\x00') * 4 > len(head) and len(data) > 1:
        try:
            text = data.decode('utf-16-le', 'strict').encode('utf-8', 'surrogatepass')
            yield 'utf16', text, None
        except UnicodeDecodeError:
            pass
    if depth >= MAX_DEPTH:
        return
    for match in HEX_RUN.finditer(data):
        decoded = bytes.fromhex(match.group(0).decode())
        base = match.start()
        for form, inner, back in views(decoded, depth + 1):
            mapped = (lambda start, end, base=base, back=back: None if back is None or back(start, end) is None
                      else (base + 2 * back(start, end)[0], base + 2 * back(start, end)[1]))
            yield f'hex@{base}/{form}', inner, mapped
    # A file that is an archive and cannot be opened is unreadable; inside a decoded value the same magic is often chance
    # (a tick hash decoded as hex), so there only what decodes is scanned, the rest staying covered by the raw view.
    if data[:2] == b'\x1f\x8b':
        try:
            inner = gzip.GzipFile(fileobj=io.BytesIO(data)).read(MAX_DECODED + 1)
        except (OSError, EOFError) as error:
            if depth == 0:
                raise ArchiveError(f'gzip unreadable: {type(error).__name__}') from error
            inner = partial_gzip(data)
        for form, member, _ in views(inner, depth + 1) if inner else ():
            yield f'gzip/{form}', member, None
    if data[:4] == b'PK\x03\x04' or (len(data) > 22 and zipfile.is_zipfile(io.BytesIO(data))):
        try:
            with zipfile.ZipFile(io.BytesIO(data)) as archive:
                members = [(info.filename, archive.read(info)) for info in archive.infolist()]
        except (zipfile.BadZipFile, OSError, RuntimeError, EOFError, ValueError, NotImplementedError) as error:
            if depth == 0:
                raise ArchiveError(f'zip unreadable: {type(error).__name__}') from error
            members = []
        for name, content in members:
            for form, member, _ in views(content, depth + 1):
                yield f'zip:{name}/{form}', member, None
    if len(data) > 262 and data[257:262] == b'ustar':
        try:
            with tarfile.open(fileobj=io.BytesIO(data)) as archive:
                members = [(info.name, (archive.extractfile(info) or io.BytesIO()).read()) for info in archive.getmembers() if info.isfile()]
        except (tarfile.TarError, OSError, EOFError) as error:
            if depth == 0:
                raise ArchiveError(f'tar unreadable: {type(error).__name__}') from error
            members = []
        for name, content in members:
            for form, member, _ in views(content, depth + 1):
                yield f'tar:{name}/{form}', member, None


def partial_gzip(data: bytes) -> bytes:
    """What a damaged gzip stream inside a decoded value still yields; nothing when its header is not gzip at all."""
    try:
        return zlib.decompressobj(31).decompress(data, MAX_DECODED + 1)
    except zlib.error:
        return b''


class ArchiveError(Exception):
    pass


def file_hits(data: bytes, finder: Finder, public=None, public_rows=None) -> tuple[list[dict], list[tuple[int, int]], bool]:
    """Hits (form, kind, how) of one file, the raw spans to blank, and whether an archive member holds one. A username or
    a shape hit whose value public() names as already published (the repository's own tracked files) goes to public_rows
    instead; a key, token, secret, password or minted login is never excused."""
    rows, spans, in_archive = [], [], False
    for form, view, back in views(data):
        found = finder.hits(view)
        fields = [(start, end) for start, end, kind, _ in found if kind.startswith('shape:')]
        booked = [(start, end) for start, end, kind, _ in found if not kind.startswith('shape:')]
        overlaps = lambda start, end, spans_: any(start < right and left < end for left, right in spans_)
        for start, end, kind, how in found:
            # The retired account's name is a public word outside a login field; a login field's value is public only
            # when it is no booked value (a sample login in a test file, never the retired account's own name).
            excusable = (kind == 'fixed-username' and not overlaps(start, end, fields)) or \
                        (kind.startswith('shape:') and not overlaps(start, end, booked))
            reason = public(view[start:end]) if public and excusable else None
            if reason:
                public_rows.append(dict(form=form, kind=kind, how=how, reason=reason))
                continue
            rows.append(dict(form=form, kind=kind, how=how))
            span = back(start, end) if back else None
            if span is None:
                in_archive = True
            else:
                spans.append(span)
    return rows, spans, in_archive


def sweep(roots: Iterable[Path | str], finder: Finder | None = None, scrub: bool = False, public=None) -> dict[str, Any]:
    finder = finder or Finder([], None)
    files = 0
    hits, incomplete, scrubbed, published = [], [], [], []
    for root in roots:
        for path, reason in walk(Path(root)):
            if reason:
                incomplete.append(dict(path=str(path), reason=reason))
                continue
            files += 1
            try:
                data = path.read_bytes()
                public_rows = []
                rows, spans, in_archive = file_hits(data, finder, public, public_rows)
                if public_rows:
                    published.append(dict(path=str(path), hits=len(public_rows), kinds=sorted({hit['kind'] for hit in public_rows}),
                                          reasons=sorted({hit['reason'] for hit in public_rows})))
            except ArchiveError as error:
                incomplete.append(dict(path=str(path), reason=str(error)))
                continue
            except OSError as error:
                incomplete.append(dict(path=str(path), reason=f'file unreadable: {type(error).__name__}'))
                continue
            if not rows:
                continue
            row = dict(path=str(path), bytes=len(data), sha256_before=hashlib.sha256(data).hexdigest(),
                       kinds=sorted({hit['kind'] for hit in rows}), forms=sorted({hit['form'] for hit in rows}))
            hits.append(row)
            if scrub:
                scrubbed.append(clean_file(path, data, spans, in_archive, row, finder))
    leaked_after = [row for row in scrubbed if not row['clean_after']]
    status = 'INCOMPLETE' if incomplete else 'CLEAN' if not hits or (scrub and not leaked_after) else 'LEAKED'
    return dict(status=status, clean=status == 'CLEAN' and not hits, scrubbed_clean=status == 'CLEAN',
                files_scanned=files, files_with_secrets=hits, incomplete=incomplete, scrubbed=scrubbed, public=published)


def repository_public(repo: Path | str):
    """public() for sweep: a value already in the repository's tracked files at HEAD is published, never a login."""
    cache: dict[bytes, str | None] = {}

    def public(value: bytes) -> str | None:
        if value not in cache:
            try:
                text = value.decode('utf-8')
                done = subprocess.run(['git', '-C', str(repo), 'grep', '-F', '-I', '-l', '-e', text, 'HEAD', '--'],
                                      capture_output=True, text=True, timeout=120)
                files = [line for line in done.stdout.splitlines() if line]
                cache[value] = f'in {len(files)} tracked file(s) of the repository' if done.returncode == 0 and files else None
            except (UnicodeDecodeError, OSError, subprocess.TimeoutExpired):
                cache[value] = None
        return cache[value]
    return public


def clean_file(path: Path, data: bytes, spans: list[tuple[int, int]], in_archive: bool, row: dict, finder: Finder) -> dict:
    """Blanks every span with 'x' (hex '78' inside a hex payload), or replaces an archive that holds a login with a
    receipt; then sweeps the result again."""
    if in_archive:
        receipt = path.with_name(path.name + '.scrubbed.json')
        receipt.write_text(json.dumps(dict(removed=str(path), reason='an archive member held a relay login',
                                           kinds=row['kinds'], forms=row['forms'], bytes=row['bytes'],
                                           sha256_before=row['sha256_before']), indent=2) + '\n', encoding='utf-8')
        path.unlink()
        return dict(path=str(path), action='archive replaced by its receipt', receipt=str(receipt), clean_after=not path.exists())
    blanked = bytearray(data)
    for start, end in spans:
        segment = bytes(blanked[start:end])
        is_hex = re.fullmatch(rb'[0-9a-fA-F]*', segment) is not None and (end - start) % 2 == 0 and any(
            start >= match.start() and end <= match.end() for match in HEX_RUN.finditer(data))
        blanked[start:end] = (b'78' * ((end - start) // 2)) if is_hex else b'x' * (end - start)
    path.write_bytes(bytes(blanked))
    remaining, _, still_archive = file_hits(bytes(blanked), finder)
    return dict(path=str(path), action='blanked in place', spans=len(spans), sha256_after=hashlib.sha256(blanked).hexdigest(),
                clean_after=not remaining and not still_archive)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    run = sub.add_parser('sweep')
    run.add_argument('--root', type=Path, action='append', required=True)
    run.add_argument('--digests', type=Path)
    run.add_argument('--scrub', action='store_true')
    run.add_argument('--out', type=Path, required=True)
    run.add_argument('--public-repo', type=Path, help='a username or shape hit whose value this repository already tracks is reported as public')
    options = parser.parse_args(argv)
    finder = DigestBook(json.loads(options.digests.read_text(encoding='utf-8'))).finder() if options.digests else Finder([], None)
    public = repository_public(options.public_repo) if options.public_repo else None
    first = sweep(options.root, finder, scrub=options.scrub, public=public)
    verify = sweep(options.root, finder, public=public) if options.scrub else None
    result = dict(sweep=first, verify=verify)
    options.out.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    final = verify or first
    print(json.dumps(dict(status=final['status'], files=final['files_scanned'], hits=len(first['files_with_secrets']),
                          after=len(final['files_with_secrets']), incomplete=len(final['incomplete']))))
    return 0 if final['status'] == 'CLEAN' else 3 if final['status'] == 'INCOMPLETE' else 1


if __name__ == '__main__':
    sys.exit(main())
