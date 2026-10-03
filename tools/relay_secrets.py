"""Relay secrets kept in memory only, and the scan that proves no file holds one.

The Cloudflare TURN key file, a fixed relay login and every credential the directory mints are read into a SecretBook;
the book never prints, logs or writes a value. scan() walks the given roots (never entering a junction, symlink or
other reparse point) and reports which files hold which KIND of secret, never the secret itself.
"""
from __future__ import annotations

import json
import os
import re
import stat
from pathlib import Path
from typing import Any, Iterable

REPARSE = getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0x400)
# Shorter values match ordinary text by chance; every real key, token and minted login is far longer.
MIN_NEEDLE = 8


def read_turn_config(path: Path | str) -> dict[str, Any]:
    """The directory's TURN backend file, read by path; the caller hands the dict to the directory and the book."""
    config = json.loads(Path(path).read_text(encoding='utf-8'))
    if not isinstance(config, dict):
        raise ValueError(f'{Path(path).name}: not a TURN backend object')
    return config


def read_fixed_login(conf: Path | str) -> tuple[str, str]:
    """The first user=<name>:<password> line of a coturn config (lt-cred-mech)."""
    for line in Path(conf).read_text(encoding='utf-8', errors='replace').splitlines():
        match = re.match(r'\s*user\s*=\s*([^:\s]+):(\S+)', line)
        if match:
            return match[1], match[2]
    raise ValueError(f'{Path(conf).name} has no user=<name>:<password> line')


class SecretBook:
    """Named secrets held in memory; scan() reports kinds and paths only."""

    def __init__(self) -> None:
        self._needles: dict[bytes, str] = {}

    def add(self, kind: str, value: Any) -> None:
        if isinstance(value, str) and len(value) >= MIN_NEEDLE:
            self._needles.setdefault(value.encode('utf-8'), kind)

    def add_turn_config(self, config: dict[str, Any]) -> None:
        for key in ('api_token', 'turn_key_id', 'static_auth_secret'):
            self.add(f'backend-{key}', config.get(key))

    def add_offer(self, offer: dict[str, Any], kind: str = 'minted') -> None:
        for server in offer.get('iceServers', []) if isinstance(offer, dict) else []:
            self.add(f'{kind}-username', server.get('username'))
            self.add(f'{kind}-credential', server.get('credential'))

    def kinds(self) -> list[str]:
        return sorted(set(self._needles.values()))

    def __len__(self) -> int:
        return len(self._needles)

    def hits(self, data: bytes) -> list[str]:
        return sorted({kind for needle, kind in self._needles.items() if needle in data})

    def scan(self, roots: Iterable[Path | str]) -> dict[str, Any]:
        files, findings = 0, []
        for root in roots:
            for path in walk_files(Path(root)):
                files += 1
                try:
                    data = path.read_bytes()
                except OSError as error:
                    findings.append({'path': str(path), 'kinds': [], 'unreadable': type(error).__name__})
                    continue
                kinds = self.hits(data)
                if kinds:
                    findings.append({'path': str(path), 'kinds': kinds})
        clean = not any(row['kinds'] or row.get('unreadable') for row in findings)
        return {'secrets': len(self), 'kinds': self.kinds(), 'files_scanned': files, 'files_with_secrets': findings, 'clean': clean}


def is_reparse(entry: os.DirEntry) -> bool:
    if entry.is_symlink() or (hasattr(entry, 'is_junction') and entry.is_junction()):
        return True
    try:
        return bool(getattr(entry.stat(follow_symlinks=False), 'st_file_attributes', 0) & REPARSE)
    except OSError:
        return True


def walk_files(root: Path) -> Iterable[Path]:
    """Every regular file under root; a junction, symlink or other reparse point is neither entered nor read."""
    if root.is_file():
        yield root
        return
    if not root.is_dir():
        return
    stack = [root]
    while stack:
        directory = stack.pop()
        try:
            entries = list(os.scandir(directory))
        except OSError:
            continue
        for entry in entries:
            if is_reparse(entry):
                continue
            if entry.is_dir(follow_symlinks=False):
                stack.append(Path(entry.path))
            elif entry.is_file(follow_symlinks=False):
                yield Path(entry.path)
