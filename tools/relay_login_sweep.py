"""Every relay login under the given roots in any form, by file, never by value; with no secret to look for.

    python tools/relay_login_sweep.py <root> [<root> ...] [--skip-dir <name>]

It reads every file (no size or extension is skipped; a junction or symlink is never entered) in every form
relay_secrets.sweep reads and finds a login by its shape: a JSON username/credential field (escaped too), an INI relay
login key, a coturn REST username, a 64-hex value after a login key, a coturn account line. A file or directory it
cannot read makes the result INCOMPLETE, never clean. --skip-dir drops whole named directories from the sweep and the
result lists every one it dropped.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from relay_secrets import Finder, sweep as structural_sweep  # noqa: E402


def sweep(roots: list[Path], skip_dirs: set[str] | None = None) -> dict:
    skip_dirs = set(skip_dirs or ())
    chosen, skipped = [], []
    for root in map(Path, roots):
        if skip_dirs and root.is_dir():
            for child in root.iterdir():
                (skipped if child.name in skip_dirs else chosen).append(child)
        else:
            chosen.append(root)
    result = structural_sweep(chosen, Finder([], None))
    return dict(status=result['status'], files_scanned=result['files_scanned'], incomplete=result['incomplete'],
                skipped_dirs=[str(path) for path in skipped],
                files_with_logins=[dict(path=row['path'], fields=row['kinds'], forms=row['forms']) for row in result['files_with_secrets']])


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('roots', type=Path, nargs='+')
    parser.add_argument('--skip-dir', action='append', default=[])
    options = parser.parse_args(argv)
    result = sweep(options.roots, set(options.skip_dir))
    print(json.dumps(result, indent=1))
    return 0 if result['status'] == 'CLEAN' else 3 if result['status'] == 'INCOMPLETE' else 1


if __name__ == '__main__':
    raise SystemExit(main())
