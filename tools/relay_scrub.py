"""A relay login an engine wrote into a file, found, revoked with Cloudflare and blanked in place.

    python tools/relay_scrub.py <file or dir> [...] [--turn-config D:/mx/coturn-20260920/turn-config-cloudflare.json]
                                [--no-revoke] --out <receipt.json>

Every login the files hold in any form relay_secrets.sweep reads (raw, escaped JSON, INI, hex payloads, archive members)
is found by its shape; each JSON username is revoked through Cloudflare's credentials/<username>/revoke when the key file
is given (204 = revoked), then every hit is blanked in place (an archive holding one is replaced by its receipt) and the
files are swept again. The receipt keeps counts, statuses and sha256 before and after, never a login.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Callable
from urllib.error import HTTPError
from urllib.request import Request, urlopen

sys.path.insert(0, str(Path(__file__).resolve().parent))
from relay_secrets import PATTERNS, Finder, SecretBook, read_turn_config, sweep, views, walk  # noqa: E402

USER_AGENT = 'cccp-session-directory/1'
JSON_FIELDS = ('json-login-field', 'escaped-json-login-field')


def usernames(data: bytes) -> list[str]:
    """The JSON usernames in every readable form of the bytes, for revocation (kept in memory only)."""
    found = []
    for _, view, _ in views(data):
        for name in JSON_FIELDS:
            for match in PATTERNS[name].finditer(view):
                if match.group(1) == b'username':
                    value = match.group(2).replace(b'\\\\', b'\\').replace(b'\\"', b'"')
                    if value and set(value) != {ord('x')}:
                        found.append(value.decode('utf-8', 'replace'))
    return found


def cloudflare_revoker(config: dict) -> Callable[[str], int]:
    def revoke(username: str) -> int:
        request = Request(f'https://rtc.live.cloudflare.com/v1/turn/keys/{config["turn_key_id"]}/credentials/{username}/revoke',
                          data=b'', method='POST', headers={'Authorization': 'Bearer ' + config['api_token'], 'User-Agent': USER_AGENT})
        try:
            with urlopen(request, timeout=15) as response:
                return response.status
        except HTTPError as error:
            return error.code
        except OSError:
            return 0
    return revoke


def scrub(paths: list[Path], revoke: Callable[[str], int] | None = None, book: SecretBook | None = None) -> dict:
    """Revoke what can be revoked, then blank every login in the paths and sweep them again."""
    statuses, seen = [], set()
    files = [path for root in map(Path, paths) for path, reason in walk(root) if reason is None]
    for path in files:
        try:
            names = usernames(path.read_bytes())
        except OSError:
            continue
        for name in names:
            if revoke and name not in seen:
                seen.add(name)
                statuses.append(revoke(name))
    finder = (book or SecretBook()).finder()
    first = sweep(paths, finder, scrub=True)
    after = sweep(paths, finder)
    rows = {row['path']: row for row in first['files_with_secrets']}
    done = {row['path']: row for row in first['scrubbed']}
    report = [dict(path=str(path), logins=sum(kind.endswith('username') or kind == 'shape:json-login-field' or
                                               kind == 'shape:escaped-json-login-field' for kind in rows.get(str(path), {}).get('kinds', []))
                   if str(path) in rows else 0, values_blanked=done.get(str(path), {}).get('spans', 0),
                   action=done.get(str(path), {}).get('action'), sha256_before=rows.get(str(path), {}).get('sha256_before'),
                   sha256_after=done.get(str(path), {}).get('sha256_after'))
              for path in files]
    return dict(files=report, revokes=statuses, status=after['status'], incomplete=first['incomplete'] + after['incomplete'],
                hits_before=len(first['files_with_secrets']), hits_after=len(after['files_with_secrets']))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('files', type=Path, nargs='+')
    parser.add_argument('--turn-config', type=Path)
    parser.add_argument('--no-revoke', action='store_true')
    parser.add_argument('--out', type=Path, required=True)
    options = parser.parse_args(argv)
    revoke = None if options.no_revoke or not options.turn_config else cloudflare_revoker(read_turn_config(options.turn_config))
    result = scrub(options.files, revoke)
    options.out.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(dict(status=result['status'], hits_before=result['hits_before'], hits_after=result['hits_after'],
                          revokes=result['revokes'])))
    return 0 if result['status'] == 'CLEAN' and all(status == 204 for status in result['revokes']) else 1


if __name__ == '__main__':
    raise SystemExit(main())
