#!/usr/bin/env python3
"""Split the sanitizer reports in a suite's engine logs into findings, one per distinct stack, each with its rows.

    python3 sanitizer_digest.py <suite root> [--out <prefix>]

Reads every stdout.log and stderr.log under the root (symlinks are never entered). A LeakSanitizer block ("Direct leak
of ..." / "Indirect leak of ..."), a ThreadSanitizer or AddressSanitizer report and a UBSan "runtime error" line are one
report each; reports with the same kind, headline shape and top frames are one finding. Writes <prefix>.json and
<prefix>.txt; the last stdout line is the count summary.
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import re
from pathlib import Path

# ASan frames read "#0 0x... in f file:line (bin+0x...)", TSan frames "#0 f file:line (bin+0x...) (BuildId: ...)".
FRAME = re.compile(r"^\s*#(\d+) (?:0x[0-9a-f]+ in )?(.+?)(?: \(\S+\+0x[0-9a-f]+\))?(?: \(BuildId: [0-9a-f]+\))?$")
START = [
    ("leak", re.compile(r"^(Direct|Indirect) leak of \d+ byte\(s\) in \d+ object\(s\) allocated from:")),
    ("tsan", re.compile(r"^WARNING: ThreadSanitizer: (.+?)(?: \(pid=\d+\))?$")),
    ("asan", re.compile(r"^==\d+==ERROR: AddressSanitizer: (.+)")),
    ("ubsan", re.compile(r"^(\S+:\d+:\d+): runtime error: (.+)")),
]
END = re.compile(r"^(SUMMARY: |==================|==\d+==ABORTING)")
TOP_FRAMES = 6
KEEP_FRAMES = 16


def logs(root: Path):
    for directory, subdirs, files in os.walk(root, followlinks=False):
        subdirs[:] = [d for d in subdirs if d != "runtime" and not os.path.islink(os.path.join(directory, d))]
        for name in files:
            if name in ("stdout.log", "stderr.log"):
                yield Path(directory) / name


def row_of(path: Path, root: Path) -> str:
    parts = path.relative_to(root).parts
    return "/".join(parts[:-1]) or "."


def shape(text: str) -> str:
    return re.sub(r"0x[0-9a-f]+|\b\d+\b", "N", text)


def reports(path: Path):
    lines = path.read_text(errors="replace").splitlines()
    index = 0
    while index < len(lines):
        line = lines[index]
        for kind, pattern in START:
            if pattern.match(line):
                block = [line]
                cursor = index + 1
                if kind == "ubsan":
                    while cursor < len(lines) and FRAME.match(lines[cursor]):
                        block.append(lines[cursor])
                        cursor += 1
                else:
                    seen_frame = False
                    while cursor < len(lines):
                        text = lines[cursor]
                        if kind == "leak" and seen_frame and not FRAME.match(text):
                            break
                        if kind != "leak" and END.match(text):
                            if text.startswith("SUMMARY: "):
                                block.append(text)
                                cursor += 1
                            break
                        seen_frame = seen_frame or bool(FRAME.match(text))
                        block.append(text)
                        cursor += 1
                yield kind, index + 1, block
                index = cursor - 1
                break
        index += 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("root", type=Path)
    parser.add_argument("--out", type=Path)
    options = parser.parse_args()
    root = options.root.resolve()
    prefix = options.out or root / "sanitizer-digest"
    findings: dict[tuple, dict] = {}
    per_kind = collections.Counter()
    for path in sorted(logs(root)):
        for kind, number, block in reports(path):
            per_kind[kind] += 1
            frames = [m.group(2) for m in map(FRAME.match, block) if m]
            key = (kind, shape(block[0]) if kind != "leak" else block[0].split()[0], tuple(frames[:TOP_FRAMES]))
            entry = findings.setdefault(key, {"kind": kind, "headline": block[0], "top_frames": frames[:KEEP_FRAMES],
                                              "first": f"{path}:{number}", "count": 0, "rows": []})
            entry["count"] += 1
            row = row_of(path, root)
            if row not in entry["rows"]:
                entry["rows"].append(row)
    ordered = sorted(findings.values(), key=lambda f: (f["kind"], -f["count"]))
    document = {"root": str(root), "reports": dict(per_kind), "findings": len(ordered), "items": ordered}
    Path(f"{prefix}.json").write_text(json.dumps(document, indent=2) + "\n")
    with open(f"{prefix}.txt", "w") as text:
        for number, item in enumerate(ordered, 1):
            text.write(f"[{number}] {item['kind']} x{item['count']} rows={','.join(item['rows'])}\n    {item['headline']}\n")
            for frame in item["top_frames"]:
                text.write(f"      in {frame}\n")
            text.write(f"    first: {item['first']}\n")
    print(f"reports={dict(per_kind)} findings={len(ordered)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
