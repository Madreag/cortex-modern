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
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "sanitizers"))
import check_ubsan_supp as ubsan

# ASan frames read "#0 0x... in f file:line (bin+0x...)", TSan frames "#0 f file:line (bin+0x...) (BuildId: ...)".
FRAME = re.compile(r"^\s*#(\d+) (?:0x[0-9a-f]+ in )?(.+?)(?: \(\S+\+0x[0-9a-f]+\))?(?: \(BuildId: [0-9a-f]+\))?$")
START = [
    ("leak", re.compile(r"^(Direct|Indirect) leak of \d+ byte\(s\) in \d+ object\(s\) allocated from:")),
    ("tsan", re.compile(r"^WARNING: ThreadSanitizer: (.+?)(?: \(pid=\d+\))?$")),
    ("asan", re.compile(r"^==\d+==ERROR: AddressSanitizer: (.+)")),
    ("ubsan", re.compile(r"^(.+?:\d+:\d+): runtime error: (.+)")),
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


def start_of(line: str):
    for kind, pattern in START:
        if pattern.match(line):
            return kind
    return None


def reports(path: Path):
    """Streams the log (a TSan suite writes logs of several hundred MB); a block ends where its kind says it does."""
    kind, number, block, seen_frame = None, 0, [], False
    with open(path, errors="replace") as handle:
        for index, raw in enumerate(handle, 1):
            line = raw.rstrip("\r\n")
            if kind:
                is_frame = bool(FRAME.match(line))
                ended = ((kind == "ubsan" and not is_frame) or (kind == "leak" and seen_frame and not is_frame)
                         or (kind in ("tsan", "asan") and END.match(line)))
                if not ended:
                    seen_frame = seen_frame or is_frame
                    block.append(line)
                    continue
                if kind in ("tsan", "asan") and line.startswith("SUMMARY: "):
                    block.append(line)
                yield kind, number, block
                kind = None
            started = start_of(line)
            if started:
                kind, number, block, seen_frame = started, index, [line], False
        if kind:
            yield kind, number, block


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("root", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--ubsan-supp", type=Path, default=ubsan.DEFAULT_SUPP)
    options = parser.parse_args()
    entries, errors = ubsan.parse(options.ubsan_supp.read_text())
    if errors:
        parser.error("; ".join(errors))
    root = options.root.resolve()
    prefix = options.out or root / "sanitizer-digest"
    findings: dict[tuple, dict] = {}
    suppressed_findings: dict[tuple, dict] = {}
    per_kind = collections.Counter()
    suppressed_counts = collections.Counter()
    for path in sorted(logs(root)):
        for kind, number, block in reports(path):
            report = ubsan.REPORT.match(block[0]) if kind == "ubsan" else None
            suppressed = report is not None and ubsan.suppressed(entries, ubsan.check_of(report["message"]), report["file"])
            counts = suppressed_counts if suppressed else per_kind
            counts[kind] += 1
            frames = [m.group(2) for m in map(FRAME.match, block) if m]
            key = (kind, shape(block[0]) if kind != "leak" else block[0].split()[0], tuple(frames[:TOP_FRAMES]))
            if report:
                key += (report["file"], report["line"], report["col"])
            target = suppressed_findings if suppressed else findings
            entry = target.setdefault(key, {"kind": kind, "headline": block[0], "top_frames": frames[:KEEP_FRAMES],
                                              "first": f"{path}:{number}", "count": 0, "rows": []})
            entry["count"] += 1
            row = row_of(path, root)
            if row not in entry["rows"]:
                entry["rows"].append(row)
    ordered = sorted(findings.values(), key=lambda f: (f["kind"], -f["count"]))
    suppressed_ordered = sorted(suppressed_findings.values(), key=lambda f: f["headline"])
    document = {"root": str(root), "reports": dict(per_kind), "findings": len(ordered), "items": ordered,
                "ubsan_suppressions": str(options.ubsan_supp.resolve()), "suppressed_reports": dict(suppressed_counts),
                "suppressed_items": suppressed_ordered}
    Path(f"{prefix}.json").write_text(json.dumps(document, indent=2) + "\n")
    with open(f"{prefix}.txt", "w") as text:
        for number, item in enumerate(ordered, 1):
            text.write(f"[{number}] {item['kind']} x{item['count']} rows={','.join(item['rows'])}\n    {item['headline']}\n")
            for frame in item["top_frames"]:
                text.write(f"      in {frame}\n")
            text.write(f"    first: {item['first']}\n")
        for item in suppressed_ordered:
            text.write(f"[suppressed] x{item['count']} {item['headline']}\n    first: {item['first']}\n")
    print(f"reports={dict(per_kind)} findings={len(ordered)} suppressed={dict(suppressed_counts)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
