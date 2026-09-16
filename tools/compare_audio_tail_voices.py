#!/usr/bin/env python
"""Compare one AudioRuntime voice on two peer archives at the same sim tick.

The comparer of record is snapshot_runtime.project(..., shared=True). With no
files, the arm feeds a mixer-retirement pair (one peer still holds the tail
voice, the other dropped it) and the matching tip pair.
"""

import argparse
import json
import sys
from pathlib import Path

if __package__:
    from . import snapshot_runtime
else:
    import snapshot_runtime

FAIL = "AudioRuntime voices lists differ at a sound's tail"
TAIL_IDENTITY = 7
TAIL_POSITION = 12
FIXTURE_DIR = Path(__file__).resolve().parent / "audio_tail_fixtures"


def decode_archive(data):
    if isinstance(data, str):
        data = data.encode("ascii")
    return snapshot_runtime.decode(data)


def isolate_voice(runtime, identity):
    matches = [voice for voice in runtime.get("voices", []) if int(voice.get("identity", 0)) == int(identity)]
    if len(matches) != 1:
        raise ValueError(f"isolated voice count is {len(matches)} for identity {identity}")
    return matches[0]


def emit_peer_voice(identity, position):
    return {
        "version": "AudioVoice3",
        "identity": identity,
        "owner": 1,
        "path": "sfx",
        "playing": 1,
        "bus": 0,
        "priority": 64,
        "loops": 0,
        "position": position,
        "loop_start": 0,
        "loop_end": 1,
        "frequency": 44100,
        "minimum_audible_distance": 0,
        "control": {"version": "AudioControl1", "paused": 0, "volume": 1, "pitch": 1},
    }


def emit_peer_runtime(identity, position, voices=None):
    if voices is None:
        voices = [emit_peer_voice(identity, position)]
    return {"version": "AudioRuntime3", "voices": voices}


def emit_mixer_retired_pair():
    return emit_peer_runtime(TAIL_IDENTITY, TAIL_POSITION), emit_peer_runtime(TAIL_IDENTITY, TAIL_POSITION, voices=[])


def emit_tip_pair():
    return emit_peer_runtime(TAIL_IDENTITY, TAIL_POSITION), emit_peer_runtime(TAIL_IDENTITY, TAIL_POSITION)


def load_fixture(name):
    path = FIXTURE_DIR / name
    return json.loads(path.read_text(encoding="utf-8"))


def compare_runtimes(first, second):
    return snapshot_runtime.project(first, shared=True) == snapshot_runtime.project(second, shared=True)


def compare_isolated(first, second, identity):
    left = isolate_voice(first, identity)
    right = isolate_voice(second, identity)
    return snapshot_runtime.project(left, shared=True) == snapshot_runtime.project(right, shared=True)


def compare_peer_voices(first_text, second_text, identity):
    return compare_isolated(decode_archive(first_text), decode_archive(second_text), identity)


def run_arm():
    retired = (load_fixture("mixer_retired_kept.json"), load_fixture("mixer_retired_dropped.json"))
    if compare_runtimes(retired[0], retired[1]):
        print("FAIL comparer accepted a mixer-retired peer pair")
        return 1
    print("PASS comparer fails a mixer-retired peer pair")
    tip = (load_fixture("tip_peer_a.json"), load_fixture("tip_peer_b.json"))
    isolate_voice(tip[0], TAIL_IDENTITY)
    isolate_voice(tip[1], TAIL_IDENTITY)
    ok = compare_runtimes(tip[0], tip[1])
    print(("PASS " if ok else "FAIL ") + FAIL)
    return 0 if ok else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("first", nargs="?", help="first peer AudioRuntime checkpoint")
    parser.add_argument("second", nargs="?", help="second peer AudioRuntime checkpoint")
    parser.add_argument("identity", nargs="?", type=int, help="voice identity to isolate")
    args = parser.parse_args(argv)
    if args.first and args.second and args.identity is not None:
        ok = compare_peer_voices(Path(args.first).read_bytes(), Path(args.second).read_bytes(), args.identity)
        print(("PASS " if ok else "FAIL ") + FAIL)
        return 0 if ok else 1
    return run_arm()


if __name__ == "__main__":
    sys.exit(main())
