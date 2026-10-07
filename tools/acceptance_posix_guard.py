"""Refuse a POSIX launch while another lane owns the box reservation."""
from __future__ import annotations
import argparse
import os
from pathlib import Path


def assert_available(environment=None, guard_root=None, required_free=None):
    environment=os.environ if environment is None else environment
    root=Path(guard_root) if guard_root is not None else Path(environment.get('CCCP_GUARD_ROOT', str(Path.home()/'.cortex-modern'/'guards')))
    marker=root/'ACCEPTANCE-STREAM-RUNNING'
    holder='unrecorded owner'
    if marker.exists():
        try:holder=(marker/'owner').read_text(encoding='utf-8').strip() or holder
        except OSError:pass
        token=environment.get('CC_ACCEPTANCE_BOX_OWNER')
        if not token or token!=holder:
            raise RuntimeError(f'box launch refused: {marker} held by {holder}')
    free=required_free or environment.get('CC_ACCEPTANCE_CROSS_READY')
    if free and not Path(free).is_file():
        raise RuntimeError(f'box launch refused: BOX-FREE-FOR-CROSS readiness {free} absent; holder {holder}')


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--check',action='store_true')
    parser.parse_args(argv)
    try:assert_available()
    except RuntimeError as error:print(str(error));return 3
    return 0


if __name__=='__main__':raise SystemExit(main())
