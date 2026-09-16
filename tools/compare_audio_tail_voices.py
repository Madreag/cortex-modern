#!/usr/bin/env python
"""Compare one AudioRuntime voice on two peer archives at the same sim tick.

The comparer of record is snapshot_runtime.project(..., shared=True). The row
isolates one identity so an unrelated mixer drop cannot fail it.
"""

import argparse
import sys
from pathlib import Path

if __package__:
    from . import snapshot_runtime
else:
    import snapshot_runtime

FAIL = "AudioRuntime voices lists differ at a sound's tail"


def decode_archive(data):
    if isinstance(data, str):
        data = data.encode("ascii")
    return snapshot_runtime.decode(data)


def isolate_voice(runtime, identity):
    matches = [voice for voice in runtime.get("voices", []) if int(voice.get("identity", 0)) == int(identity)]
    if len(matches) != 1:
        raise ValueError(f"isolated voice count is {len(matches)} for identity {identity}")
    return matches[0]


def compare_peer_voices(first_text, second_text, identity):
    first = isolate_voice(decode_archive(first_text), identity)
    second = isolate_voice(decode_archive(second_text), identity)
    return snapshot_runtime.project(first, shared=True) == snapshot_runtime.project(second, shared=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("first", help="first peer AudioRuntime checkpoint")
    parser.add_argument("second", help="second peer AudioRuntime checkpoint")
    parser.add_argument("identity", type=int, help="voice identity to isolate")
    args = parser.parse_args(argv)
    ok = compare_peer_voices(Path(args.first).read_bytes(), Path(args.second).read_bytes(), args.identity)
    print(("PASS " if ok else "FAIL ") + FAIL)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
