"""Every JSON "username"/"credential" value under the given roots that is not blanked, by file, never by value.

    python tools/relay_login_sweep.py <root> [<root> ...] [--skip-dir engine --skip-dir Data]

Needs no secret to look for: it finds the shape the lobby codec writes a relay login in. A hit is a login some file holds
in the clear (a test fixture's fake value counts too: read the path). Junctions and symlinks are never entered.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from relay_scrub import logins  # noqa: E402
from relay_secrets import walk_files  # noqa: E402


def sweep(roots: list[Path], skip_dirs: set[str]) -> dict:
    hits, scanned = [], 0
    for root in roots:
        for path in walk_files(root):
            if skip_dirs & set(path.relative_to(root).parts[:-1]):
                continue
            scanned += 1
            try:
                found = [field for field, _, _, value in logins(path.read_bytes()) if set(value) != {ord('x')}]
            except OSError:
                continue
            if found:
                hits.append(dict(path=str(path), fields=sorted(set(found)), count=len(found)))
    return dict(files_scanned=scanned, files_with_logins=hits)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('roots', type=Path, nargs='+')
    parser.add_argument('--skip-dir', action='append', default=[])
    options = parser.parse_args(argv)
    result = sweep(options.roots, set(options.skip_dir))
    print(json.dumps(result, indent=1))
    return 1 if result['files_with_logins'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
