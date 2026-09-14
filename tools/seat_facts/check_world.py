"""Compare complete world blocks and expose their brain and alarm records."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from compare_snapshots import decode_base64, split_top_level


class Cursor:
    def __init__(self, text):
        self.tokens = text.split()
        self.offset = 0

    def take(self, count=1):
        if count < 0 or self.offset + count > len(self.tokens):
            raise ValueError("world block is truncated")
        result = self.tokens[self.offset:self.offset + count]
        self.offset += count
        return result

    def number(self):
        return int(self.take()[0])

    def sequence(self, width=1):
        return list(map(int, self.take(self.number() * width)))


def world(path):
    with zipfile.ZipFile(path) as archive:
        names = [name for name in archive.namelist() if name.lower().endswith("save.ini")]
        if len(names) != 1:
            raise ValueError("expected exactly one Save.ini")
        ini = archive.read(names[0]).decode("utf-8")
    block = "\n".join(split_top_level(ini).get("WorldStructure", []))
    raw = decode_base64(block.split("=", 1)[1].strip())
    cursor = Cursor(raw.decode("utf-8"))
    length, tag = cursor.number(), cursor.take()[0]
    if len(tag) != length or tag not in ("WorldStructure1", "WorldStructure2", "WorldStructure3"):
        raise ValueError("world layout is unsupported")
    for _ in range(10):
        cursor.sequence()
    cursor.take(4)
    alarms = [cursor.sequence(4) for _ in range(2)]
    cursor.sequence(2)
    cursor.sequence()
    cursor.sequence(2)
    if tag != "WorldStructure1":
        cursor.sequence(3)
    cursor.take(4)
    for _ in range(3):
        cursor.sequence()
    brains = cursor.sequence() if tag == "WorldStructure3" else None
    if cursor.offset != len(cursor.tokens):
        raise ValueError("world block has unconsumed fields")
    return raw, {"tag": tag, "sha256": hashlib.sha256(raw).hexdigest(), "bytes": len(raw), "brains": brains, "alarm_bits": alarms}


def compare(left, right, damage=False):
    a, host = world(left)
    b, client = world(right)
    checks = {"complete_world_block_equal": a == b, "brain_records_equal": host["brains"] == client["brains"],
              "alarm_records_equal": host["alarm_bits"] == client["alarm_bits"]}
    if damage:
        bits = lambda value: struct.unpack("<i", struct.pack("<f", value))[0]
        expected = [bits(880.0), bits(750.0064697265625), 0, bits(244.79998779296875),
                    bits(1120.1165771484375), bits(731.009765625), 1, bits(244.79998779296875)]
        checks["both_exact_alarms_on_host"] = host["alarm_bits"][0] == expected
        checks["both_exact_alarms_on_client"] = client["alarm_bits"][0] == expected
    return {"pass": all(checks.values()), "checks": checks, "host": host, "client": client}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", type=Path)
    parser.add_argument("client", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--damage", action="store_true")
    args = parser.parse_args()
    result = compare(args.host, args.client, args.damage)
    args.out.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
