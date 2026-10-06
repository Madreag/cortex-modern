"""Compare the real C++ timestamped stream with the original expanded RGB timeline."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def check(fixture: Path, ffmpeg: str, root: Path, fps: int, slots: list[int]) -> dict:
    stream = root / f"{fps}-{'-'.join(map(str, slots))}.mkv"
    subprocess.run([str(fixture), str(stream), str(fps), *map(str, slots)], check=True)
    decoded = subprocess.run(
        [ffmpeg, "-hide_banner", "-loglevel", "error", "-f", "matroska", "-i", str(stream),
         "-vf", f"fps={fps}:round=near", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        check=True, capture_output=True,
    ).stdout
    originals = [bytes((frame * 67 + byte * 13) % 256 for byte in range(16 * 16 * 3))
                 for frame in range(len(slots))]
    expected = bytearray()
    for frame, slot in enumerate(slots):
        next_slot = slots[frame + 1] if frame + 1 < len(slots) else slot + 1
        expected.extend(originals[frame] * (next_slot - slot))
    assert decoded == expected, (fps, slots, len(decoded), len(expected))
    return {"fps": fps, "slots": slots, "frames": len(expected) // (16 * 16 * 3), "all_rgb_bytes_equal": True}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", type=Path, required=True, help="compiled frame_capture_stream_fixture.cpp")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="frame-capture-stream-") as temporary:
        rows = [check(args.fixture.resolve(), args.ffmpeg, Path(temporary), fps, slots)
                for fps in (1, 24, 30, 60)
                for slots in ([0], [0, 1], [0, 8, 10], [0, 1, 100], [0, 4, 9, 12, 16, 23])]
    result = {"checks": len(rows), "passed": len(rows), "cases": rows}
    if args.out:
        args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"timestamped RGB timeline: {len(rows)}/{len(rows)}, every decoded byte equal")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
