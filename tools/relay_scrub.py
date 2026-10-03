"""A relay login an engine wrote into a file, found, revoked with Cloudflare and blanked in place.

    python tools/relay_scrub.py <file> [<file> ...] [--turn-config D:/mx/coturn-20260920/turn-config-cloudflare.json]
                                [--no-revoke] --out <receipt.json>

Every JSON "username"/"credential" pair inside the files (text or binary, e.g. a replay that recorded the agreed match
config) is read into memory only. With the key file (read by path) each username is revoked through Cloudflare's
credentials/<username>/revoke (204 = revoked); then every value's bytes are overwritten with 'x' of the same length, so
the file keeps its size. The receipt keeps counts, statuses and sha256 before and after, never a login.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Callable
from urllib.error import HTTPError
from urllib.request import Request, urlopen

sys.path.insert(0, str(Path(__file__).resolve().parent))
from relay_secrets import read_turn_config  # noqa: E402

LOGIN = re.compile(rb'"(username|credential)"\s*:\s*"([^"\\]{1,1024})"')
USER_AGENT = 'cccp-session-directory/1'


def logins(data: bytes) -> list[tuple[str, int, int, bytes]]:
    """(field, start, end, value) of every username/credential string value in the bytes."""
    return [(match[1].decode(), match.start(2), match.end(2), match[2]) for match in LOGIN.finditer(data)]


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


def scrub(paths: list[Path], revoke: Callable[[str], int] | None = None) -> dict:
    files, statuses, seen = [], [], set()
    for path in map(Path, paths):
        data = bytearray(path.read_bytes())
        before = hashlib.sha256(data).hexdigest()
        found = logins(bytes(data))
        for field, start, end, value in found:
            if field == 'username' and revoke and value not in seen:
                seen.add(value)
                statuses.append(revoke(value.decode('utf-8', 'replace')))
            data[start:end] = b'x' * (end - start)
        if found:
            path.write_bytes(bytes(data))
        files.append(dict(path=str(path), bytes=len(data), logins=sum(field == 'username' for field, *_ in found),
                          values_blanked=len(found), sha256_before=before, sha256_after=hashlib.sha256(data).hexdigest()))
    return dict(files=files, revokes=statuses)


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
    print(json.dumps({'files': [(Path(row['path']).name, row['logins'], row['values_blanked']) for row in result['files']],
                      'revokes': result['revokes']}))
    return 0 if all(status == 204 for status in result['revokes']) else 1


if __name__ == '__main__':
    raise SystemExit(main())
