"""Names the terrain pixels two peers' runs ended a tick on differently.

Each run directory holds what CC_TEST_TERRAIN_DUMP_TICKS wrote: for a tick T,
terrain-tick-T.json with the layer sizes and terrain-tick-T-mat.bin and
terrain-tick-T-fg.bin with the material and colour rows. For every tick both
runs wrote, each layer gets its count of differing pixels, the box around them
and the first few with both peers' values.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

SHOWN = 12


def dumps(root: Path) -> dict[int, Path]:
    found = {}
    for sidecar in root.rglob("terrain-tick-*.json"):
        found[json.loads(sidecar.read_text())["tick"]] = sidecar
    return found


def compare_layer(left: bytes, right: bytes, row_bytes: int, pixel_bytes: int) -> dict:
    if len(left) != len(right) or row_bytes == 0:
        return {"comparable": False, "sizes": [len(left), len(right)]}
    differing, box, shown = 0, None, []
    for offset in range(0, len(left), row_bytes):
        a, b = left[offset:offset + row_bytes], right[offset:offset + row_bytes]
        if a == b:
            continue
        y = offset // row_bytes
        for column in range(0, row_bytes, pixel_bytes):
            pa, pb = a[column:column + pixel_bytes], b[column:column + pixel_bytes]
            if pa == pb:
                continue
            x = column // pixel_bytes
            differing += 1
            box = [x, y, x, y] if box is None else [min(box[0], x), min(box[1], y), max(box[2], x), max(box[3], y)]
            if len(shown) < SHOWN:
                shown.append({"x": x, "y": y, "left": pa.hex(), "right": pb.hex()})
    return {"comparable": True, "pixels": differing, "box": box, "first": shown}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("left", type=Path, help="one peer's run directory")
    parser.add_argument("right", type=Path, help="the other peer's run directory")
    args = parser.parse_args()
    left, right = dumps(args.left), dumps(args.right)
    ticks = sorted(set(left) & set(right))
    if not ticks:
        print(f"[terrain-dumps] no tick dumped in both: left {sorted(left)} right {sorted(right)}")
        return 2
    report = []
    for tick in ticks:
        meta = json.loads(left[tick].read_text())
        width = meta["width"]
        row = {"tick": tick}
        for layer in ("mat", "fg"):
            row_bytes = meta[f"{layer}_row_bytes"]
            base = f"terrain-tick-{tick}-{layer}.bin"
            row[layer] = compare_layer((left[tick].parent / base).read_bytes(), (right[tick].parent / base).read_bytes(), row_bytes,
                                       max(1, row_bytes // width) if width else 1)
        report.append(row)
        print(f"[terrain-dumps] tick={tick} " + " ".join(
            f"{layer}={row[layer].get('pixels', 'n/a')} box={row[layer].get('box')}" for layer in ("mat", "fg")))
    print(json.dumps(report, indent=1))
    return 0 if all(row[layer].get("pixels") == 0 for row in report for layer in ("mat", "fg")) else 1


if __name__ == "__main__":
    raise SystemExit(main())
