"""Relay logins kept in memory, and the gate that proves no retained file holds one.

    python tools/relay_secrets.py sweep --root <dir> [--root <dir> ...] [--digests <book.json>] [--fixtures <registry.json>]
                                        [--scrub] --out <receipt.json>

The Cloudflare key file, a coturn secret and every login a run mints or observes are read into a SecretBook; the book
never prints, logs or writes a value, and redact() takes every booked value out of a text before it is printed or saved.
A box that must be swept without the values gets a DigestBook: a per-run salt and the salted SHA-256 of each value.

sweep() reads every regular file under the roots (a junction, symlink or other reparse point is neither entered nor read;
nothing is skipped for its size or extension) in every form a login can take there: the raw bytes, JSON-escaped and
UTF-16 forms, JSON Unicode escapes decoded, hex-encoded payloads decoded, and the members of gzip, zip and tar archives.
A hit is the exact book (a short value only at a token boundary; a value booked for login fields only where a login
field holds it), the digest book, or a login's own shape: a JSON username/credential field, the relay settings keys in
INI or JSON, a menu entry into a relay login box, a coturn REST username, a 64-hex value after a login key, a coturn
account line. A shape hit is excused only by the fixture registry (tools/relay_fixtures.json): its value's digest is a
registered synthetic value AND the line around it is that value's registered defining line; a booked value is never
excused. A file, directory or archive that cannot be read, or a decode a depth or size limit stops, makes the sweep
INCOMPLETE, never clean. With scrub, a hit is overwritten in place in the representation it was found in (x of equal
length; the hex or UTF-16 encoding of x inside such a span) and an archive holding one is replaced by a receipt; the file
is swept again and must come back clean. Reports name the file, the form and the kind, never a value.
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
JSON_ESCAPE = re.compile(rb'\\(u[0-9a-fA-F]{4}|["\\/bfnrt])')
UNICODE_ESCAPE = re.compile(rb'\\u[0-9a-fA-F]{4}')
MAX_DECODED = 1 << 30
MAX_DEPTH = 3
FIXTURES = Path(__file__).resolve().with_name('relay_fixtures.json')
PLACEHOLDER = re.compile(rb'^(?:x+|redacted|<[^>]*>|REPLACE_WITH[A-Z_]*|\*+)?$')
RELAY_KEYS = rb'(NetworkTurnUser|NetworkTurnPass|NetworkPlayerTurnUser|NetworkPlayerTurnPass)'
PATTERNS = {
    # A JSON login field, and the same field escaped inside another JSON string.
    'json-login-field': re.compile(rb'"(username|credential)"\s*:\s*"((?:[^"\\]|\\.)*)"'),
    'escaped-json-login-field': re.compile(rb'\\"(username|credential)\\"\s*:\s*\\"((?:[^"\\]|\\\\(?:\\\\|\\"))*)\\"'),
    # The relay settings keys as JSON (a harness spec or receipt carrying a peer's settings), plain or escaped.
    'json-relay-setting': re.compile(rb'(?i)"' + RELAY_KEYS + rb'"\s*:\s*"((?:[^"\\]|\\.)*)"'),
    'escaped-json-relay-setting': re.compile(rb'(?i)\\"' + RELAY_KEYS + rb'\\"\s*:\s*\\"((?:[^"\\]|\\\\(?:\\\\|\\"))*)\\"'),
    # A relay login written into an INI (Settings.ini) by the game or a harness.
    'ini-relay-login': re.compile(rb'(?mi)^[ \t]*' + RELAY_KEYS + rb'[ \t]*=[ \t]*([^\r\n]*?)[ \t]*\r?$'),
    # A menu script typing into a relay login box, and the menu automation's echo of it.
    'menu-relay-login': re.compile(rb'(?:^|[\]\s])set_?text[ \t]+(Text\w*Relay\w*(?:User|Pass)\w*)[ \t]+([^\s\\"]+)'),
    # coturn's REST username as the directory mints it: expiry seconds, a colon, a 24-hex tag.
    'coturn-rest-username': re.compile(rb'(?<![0-9])(1[0-9]{9}:[0-9a-f]{24})(?![0-9a-f])'),
    # A 64-hex value right after a login key (Cloudflare's minted username and credential are 64 hex characters).
    'hex64-after-login-key': re.compile(rb'(?i)(username|credential|turnuser|turnpass|password|user)[^A-Za-z0-9]{1,8}([0-9a-f]{64})(?![0-9a-f])'),
    # A coturn account or REST secret line.
    'coturn-secret-line': re.compile(rb'(?mi)^[ \t]*(user|static-auth-secret)[ \t]*=[ \t]*(\S+)'),
}
# The retired coturn account's username is the project's own name, so it is a login only where a login field holds it.
RETIRED_USERNAME_SCOPE = 'login-field'


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


def variants(value: bytes) -> dict[bytes, str]:
    """The forms a value takes in a file, each with its representation: itself, JSON-escaped once and twice, UTF-16, hex.
    Other JSON escapes are read by the json-unescaped view."""
    text = value.decode('utf-8', 'surrogateescape')
    once = json.dumps(text)[1:-1]
    forms = {value: 'raw', once.encode('utf-8', 'surrogateescape'): 'raw', json.dumps(once)[1:-1].encode('utf-8', 'surrogateescape'): 'raw'}
    forms.setdefault(text.encode('utf-16-le', 'surrogatepass'), 'utf16')
    forms.setdefault(value.hex().encode(), 'hex')
    forms.setdefault(value.hex().upper().encode(), 'hex')
    return forms


def json_unescape(data: bytes) -> bytes:
    """JSON string escapes decoded once (\\uXXXX to UTF-8, the single-character escapes to their characters)."""
    def one(match: re.Match) -> bytes:
        code = match.group(1)
        if code[:1] == b'u':
            return chr(int(code[1:], 16)).encode('utf-8', 'surrogatepass')
        return {b'"': b'"', b'\\': b'\\', b'/': b'/', b'b': b'\b', b'f': b'\f', b'n': b'\n', b'r': b'\r', b't': b'\t'}[code]
    return JSON_ESCAPE.sub(one, data)


class SecretBook:
    """Named secrets held in memory; nothing it reports carries a value."""

    def __init__(self) -> None:
        self._values: dict[bytes, str] = {}
        self._scopes: dict[bytes, str] = {}

    def add(self, kind: str, value: Any, scope: str = 'anywhere') -> None:
        if isinstance(value, str) and value and not PLACEHOLDER.match(value.encode('utf-8', 'replace')):
            self._values.setdefault(value.encode('utf-8'), kind)
            self._scopes.setdefault(value.encode('utf-8'), scope)

    def add_turn_config(self, config: dict[str, Any]) -> None:
        for key in ('api_token', 'turn_key_id', 'static_auth_secret'):
            self.add(f'backend-{key}', config.get(key))

    def add_offer(self, offer: dict[str, Any], kind: str = 'minted') -> None:
        for server in offer.get('iceServers', []) if isinstance(offer, dict) else []:
            self.add(f'{kind}-username', server.get('username'))
            self.add(f'{kind}-credential', server.get('credential'))

    def add_fixed_login(self, conf: Path | str, username_scope: str = 'anywhere') -> None:
        user, password = read_fixed_login(conf)
        self.add('fixed-username', user, username_scope)
        self.add('fixed-password', password)

    def kinds(self) -> list[str]:
        return sorted(set(self._values.values()))

    def __len__(self) -> int:
        return len(self._values)

    def usernames(self, kind_suffix: str = '-username') -> list[str]:
        return [value.decode() for value, kind in self._values.items() if kind.endswith(kind_suffix)]

    def digests(self, salt: bytes | None = None) -> dict[str, Any]:
        """The book as a box may carry it: a fresh salt and each value's salted SHA-256, length and scope, never the value."""
        salt = salt or random_source.token_bytes(16)
        return dict(salt=salt.hex(), items=[dict(kind=kind, length=len(value), scope=self._scopes.get(value, 'anywhere'),
                                                 sha256=hashlib.sha256(salt + value).hexdigest()) for value, kind in self._values.items()])

    def finder(self) -> 'Finder':
        return Finder([(form, len(form) < SHORT, kind, self._scopes.get(value, 'anywhere'), representation)
                       for value, kind in self._values.items() for form, representation in variants(value).items()], None)

    def redact(self, text: Any) -> Any:
        """The text with every booked value, in every form, replaced by its kind; anything else unchanged."""
        if not isinstance(text, str) or not self._values:
            return text
        data = text.encode('utf-8', 'surrogateescape')
        for value, kind in sorted(self._values.items(), key=lambda item: -len(item[0])):
            for form in variants(value):
                data = data.replace(form, f'<{kind}>'.encode())
        return data.decode('utf-8', 'surrogateescape')

    def scan(self, roots: Iterable[Path | str], scrub: bool = False) -> dict[str, Any]:
        return sweep(roots, self.finder(), scrub=scrub)


class DigestBook:
    """A book a box can carry: salted digests of the values; a token of a booked length is hashed and compared."""

    def __init__(self, document: dict[str, Any]) -> None:
        self.salt = bytes.fromhex(document['salt'])
        self.by_digest = {item['sha256']: (item['kind'], item.get('scope', 'anywhere')) for item in document['items']}
        self.lengths = {int(item['length']) for item in document['items']}

    def finder(self) -> 'Finder':
        return Finder([], self)

    def kind(self, token: bytes) -> tuple[str, str] | None:
        return self.by_digest.get(hashlib.sha256(self.salt + token).hexdigest()) if len(token) in self.lengths else None


class Fixtures:
    """The registry of synthetic values a tracked file defines: the value's SHA-256 with the SHA-256 of its defining line.
    A shape hit is a fixture only when both match; no value is held."""

    def __init__(self, document: dict[str, Any] | None = None) -> None:
        self.entries = list((document or {}).get('fixtures', []))
        self.pairs = {(entry['value_sha256'], entry['line_sha256']) for entry in self.entries}

    @classmethod
    def load(cls, path: Path | str | None = None) -> 'Fixtures':
        path = Path(path or FIXTURES)
        return cls(json.loads(path.read_text(encoding='utf-8'))) if path.is_file() else cls()

    def excuses(self, value: bytes, line: bytes) -> bool:
        return (hashlib.sha256(value).hexdigest(), hashlib.sha256(line).hexdigest()) in self.pairs


def logical_line(view: bytes, start: int, end: int) -> bytes:
    """The line around a span, read the way a person reads it: a real line, or one line of text kept inside a JSON string
    (split at its escaped newlines and unescaped), less a leading 'N<tab>' line number, stripped."""
    escaped_left = view.rfind(b'\\n', 0, start)
    left = max(view.rfind(b'\n', 0, start) + 1, escaped_left + 2 if escaped_left >= 0 else 0)
    candidates = [index for index in (view.find(b'\n', end), view.find(b'\\n', end)) if index >= 0]
    line = view[left:min(candidates) if candidates else len(view)]
    for _ in range(3):
        decoded = json_unescape(line)
        if decoded == line:
            break
        line = decoded
    line = re.sub(rb'^\s*\d+(?:\t|:)', b'', line.rstrip(b'\r'))
    return line.strip()


class Finder:
    """Every login hit in one view of a file: (start, end, kind, how, representation) with offsets into that view."""

    def __init__(self, exact: list[tuple], digests: DigestBook | None) -> None:
        self.exact, self.digests = exact, digests

    def hits(self, data: bytes) -> list[tuple[int, int, str, str, str]]:
        shapes: list[tuple[int, int, str, str, str]] = []
        for name, pattern in PATTERNS.items():
            for match in pattern.finditer(data):
                group = match.lastindex or 0
                value = match.group(group)
                if value and not PLACEHOLDER.match(value.strip(b'\\')):
                    shapes.append((match.start(group), match.end(group), f'shape:{name}', 'pattern', 'raw'))
        fields = [(start, end) for start, end, *_ in shapes]
        in_field = lambda start, end: any(start < right and left < end for left, right in fields)
        found: list[tuple[int, int, str, str, str]] = []
        for entry in self.exact:
            form, short, kind = entry[:3]
            scope = entry[3] if len(entry) > 3 else 'anywhere'
            representation = entry[4] if len(entry) > 4 else 'raw'
            start = data.find(form)
            while start >= 0:
                end = start + len(form)
                if not short or ((start == 0 or not BOUNDARY.match(data[start - 1:start]))
                                 and (end == len(data) or not BOUNDARY.match(data[end:end + 1]))):
                    if scope == 'anywhere' or in_field(start, end):
                        found.append((start, end, kind, 'book', representation))
                start = data.find(form, start + 1)
        if self.digests:
            for tokens in (TOKENS_WIDE, TOKENS_NARROW):
                for match in tokens.finditer(data):
                    booked = self.digests.kind(match.group(0))
                    if booked and (booked[1] == 'anywhere' or in_field(match.start(), match.end())):
                        found.append((match.start(), match.end(), booked[0], 'digest', 'raw'))
        return found + shapes


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


class ArchiveError(Exception):
    pass


class LimitError(ArchiveError):
    """A decode a depth or size limit stopped: what lies beyond it was not read."""


def real_container(data: bytes) -> str | None:
    """The container a byte string really is (gzip with a readable header, zip, tar), or None."""
    if data[:2] == b'\x1f\x8b':
        try:
            zlib.decompressobj(31).decompress(data[:4096], 1)
            return 'gzip'
        except zlib.error:
            return None
    if data[:4] == b'PK\x03\x04' or (len(data) > 22 and zipfile.is_zipfile(io.BytesIO(data))):
        try:
            with zipfile.ZipFile(io.BytesIO(data)) as archive:
                archive.infolist()
            return 'zip'
        except (zipfile.BadZipFile, OSError, ValueError, RuntimeError):
            return None
    if len(data) > 262 and data[257:262] == b'ustar':
        return 'tar'
    return None


def views(data: bytes, depth: int = 0) -> Iterator[tuple[str, bytes, Any]]:
    """(form, bytes, back) for each form a login can take in these bytes; back maps a span of the view to (start, end,
    representation) in the original bytes, or is None when only the whole file can be cleaned."""
    yield 'raw', data, (lambda start, end, representation='raw': (start, end, representation))
    head = data[:4096]
    if head.count(b'\x00') * 4 > len(head) and len(data) > 1:
        try:
            text = data.decode('utf-16-le', 'strict').encode('utf-8', 'surrogatepass')
            yield 'utf16', text, None
        except UnicodeDecodeError:
            pass
    # Unicode escapes are text, not a container: decoded at the same depth, and only while the text keeps shrinking.
    if UNICODE_ESCAPE.search(data):
        unescaped = json_unescape(data)
        if len(unescaped) < len(data):
            for form, inner, _ in views(unescaped, depth):
                yield f'json-unescaped/{form}', inner, None
    if depth >= MAX_DEPTH:
        if HEX_RUN.search(data) or real_container(data):
            raise LimitError(f'nested deeper than {MAX_DEPTH} levels: not read past the limit')
        return
    for match in HEX_RUN.finditer(data):
        decoded = bytes.fromhex(match.group(0).decode())
        base = match.start()
        for form, inner, back in views(decoded, depth + 1):
            def mapped(start, end, representation='raw', base=base, back=back):
                inner_span = back(start, end, representation) if back else None
                return None if inner_span is None else (base + 2 * inner_span[0], base + 2 * inner_span[1], 'hex')
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
        if len(inner) > MAX_DECODED:
            raise LimitError(f'gzip member larger than {MAX_DECODED} bytes: not read past the limit')
        for form, member, _ in views(inner, depth + 1) if inner else ():
            yield f'gzip/{form}', member, None
    if data[:4] == b'PK\x03\x04' or (len(data) > 22 and zipfile.is_zipfile(io.BytesIO(data))):
        try:
            with zipfile.ZipFile(io.BytesIO(data)) as archive:
                if any(info.file_size > MAX_DECODED for info in archive.infolist()):
                    raise LimitError(f'zip member larger than {MAX_DECODED} bytes: not read past the limit')
                members = [(info.filename, archive.read(info)) for info in archive.infolist()]
        except LimitError:
            raise
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
                infos = [info for info in archive.getmembers() if info.isfile()]
                if any(info.size > MAX_DECODED for info in infos):
                    raise LimitError(f'tar member larger than {MAX_DECODED} bytes: not read past the limit')
                members = [(info.name, (archive.extractfile(info) or io.BytesIO()).read()) for info in infos]
        except LimitError:
            raise
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


def file_hits(data: bytes, finder: Finder, fixtures: Fixtures | None = None,
              fixture_rows: list | None = None) -> tuple[list[dict], list[tuple[int, int, str]], bool]:
    """Hits (form, kind, how) of one file, the raw spans to blank with their representation, and whether a span cannot be
    blanked in place. A shape hit that is a registered fixture on its own registered line goes to fixture_rows; a hit
    overlapping a booked value is never excused."""
    rows, spans, in_archive = [], [], False
    blankable: set[bytes] = set()  # values already found where they can be blanked in place
    for form, view, back in views(data):
        found = finder.hits(view)
        booked = [(start, end) for start, end, kind, *_ in found if not kind.startswith('shape:')]
        overlaps = lambda start, end: any(start < right and left < end for left, right in booked)
        for start, end, kind, how, representation in found:
            value = view[start:end]
            if fixtures and kind.startswith('shape:') and not overlaps(start, end) and fixtures.excuses(value, logical_line(view, start, end)):
                if fixture_rows is not None:
                    fixture_rows.append(dict(form=form, kind=kind))
                continue
            span = back(start, end, representation) if back else None
            if span is None and value in blankable:
                continue  # the same value in a decoded copy: blanking it in place removes this one too, and the verify re-reads
            rows.append(dict(form=form, kind=kind, how=how))
            if span is None:
                in_archive = True
            else:
                blankable.add(value)
                spans.append(span)
    return rows, spans, in_archive


def sweep(roots: Iterable[Path | str], finder: Finder | None = None, scrub: bool = False,
          fixtures: Fixtures | None = None) -> dict[str, Any]:
    finder = finder or Finder([], None)
    fixtures = Fixtures.load() if fixtures is None else fixtures
    files = 0
    hits, incomplete, scrubbed, excused = [], [], [], []
    for root in roots:
        for path, reason in walk(Path(root)):
            if reason:
                incomplete.append(dict(path=str(path), reason=reason))
                continue
            files += 1
            try:
                data = path.read_bytes()
                fixture_rows: list[dict] = []
                rows, spans, in_archive = file_hits(data, finder, fixtures, fixture_rows)
                if fixture_rows:
                    excused.append(dict(path=str(path), hits=len(fixture_rows), kinds=sorted({hit['kind'] for hit in fixture_rows})))
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
                scrubbed.append(clean_file(path, data, spans, in_archive, row, finder, fixtures))
    leaked_after = [row for row in scrubbed if not row['clean_after']]
    status = 'INCOMPLETE' if incomplete else 'CLEAN' if not hits or (scrub and not leaked_after) else 'LEAKED'
    return dict(status=status, clean=status == 'CLEAN' and not hits, scrubbed_clean=status == 'CLEAN',
                files_scanned=files, files_with_secrets=hits, incomplete=incomplete, scrubbed=scrubbed, fixtures=excused)


def blank(representation: str, length: int) -> bytes:
    """x of the span's own length, written in the span's representation."""
    if representation == 'hex':
        return b'78' * (length // 2) + b'x' * (length % 2)
    if representation == 'utf16':
        return b'x\x00' * (length // 2) + b'x' * (length % 2)
    return b'x' * length


def clean_file(path: Path, data: bytes, spans: list[tuple[int, int, str]], in_archive: bool, row: dict, finder: Finder,
               fixtures: Fixtures | None = None) -> dict:
    """Blanks every span in the representation it was found in, or replaces a file whose hit cannot be blanked in place
    (an archive member, a decoded escape) with a receipt; then sweeps the result again."""
    if in_archive:
        receipt = path.with_name(path.name + '.scrubbed.json')
        receipt.write_text(json.dumps(dict(removed=str(path), reason='a relay login sat where only the whole file can be cleaned',
                                           kinds=row['kinds'], forms=row['forms'], bytes=row['bytes'],
                                           sha256_before=row['sha256_before']), indent=2) + '\n', encoding='utf-8')
        path.unlink()
        return dict(path=str(path), action='file replaced by its receipt', receipt=str(receipt), clean_after=not path.exists())
    blanked = bytearray(data)
    for start, end, representation in spans:
        blanked[start:end] = blank(representation, end - start)
    path.write_bytes(bytes(blanked))
    try:
        remaining, _, still_archive = file_hits(bytes(blanked), finder, fixtures)
    except ArchiveError:
        remaining, still_archive = [dict(kind='unreadable after the scrub')], False
    return dict(path=str(path), action='blanked in place', spans=len(spans), sha256_after=hashlib.sha256(blanked).hexdigest(),
                clean_after=not remaining and not still_archive)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    run = sub.add_parser('sweep')
    run.add_argument('--root', type=Path, action='append', required=True)
    run.add_argument('--digests', type=Path)
    run.add_argument('--fixtures', type=Path, help='the fixture registry (default: relay_fixtures.json beside this script)')
    run.add_argument('--scrub', action='store_true')
    run.add_argument('--out', type=Path, required=True)
    options = parser.parse_args(argv)
    finder = DigestBook(json.loads(options.digests.read_text(encoding='utf-8'))).finder() if options.digests else Finder([], None)
    fixtures = Fixtures.load(options.fixtures)
    first = sweep(options.root, finder, scrub=options.scrub, fixtures=fixtures)
    verify = sweep(options.root, finder, fixtures=fixtures) if options.scrub else None
    result = dict(sweep=first, verify=verify, fixture_entries=len(fixtures.entries))
    options.out.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    final = verify or first
    print(json.dumps(dict(status=final['status'], files=final['files_scanned'], hits=len(first['files_with_secrets']),
                          after=len(final['files_with_secrets']), incomplete=len(final['incomplete']),
                          fixture_files=len(final['fixtures']), fixture_entries=len(fixtures.entries))))
    return 0 if final['status'] == 'CLEAN' else 3 if final['status'] == 'INCOMPLETE' else 1


if __name__ == '__main__':
    sys.exit(main())
