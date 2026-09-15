"""Name the first per-object simdump row two runs disagree on, matching rows by uid."""

import argparse
import json
from pathlib import Path


def load(path):
    ticks = {}
    with Path(path).open("r", encoding="utf-8", errors="replace") as source:
        for line in source:
            line = line.rstrip("\n")
            head = line.split(" ", 1)[0]
            if not head.isdigit():
                continue
            ticks.setdefault(int(head), []).append(line)
    return ticks


def key_of(line):
    parts = line.split(" ")
    uid = next((p for p in parts if p.startswith("uid=")), None)
    return (parts[1], uid if uid else " ".join(parts[1:3]))


def fields_of(line):
    out = {}
    for token in line.split(" ")[1:]:
        name, sep, value = token.partition("=")
        if sep:
            out.setdefault(name, []).append(value)
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("left", type=Path)
    parser.add_argument("right", type=Path)
    parser.add_argument("--json", type=Path)
    parser.add_argument("--max-rows", type=int, default=6)
    options = parser.parse_args()
    left, right = load(options.left), load(options.right)
    report = {"left": str(options.left), "right": str(options.right), "rows": []}
    for tick in sorted(set(left) & set(right)):
        lmap = {key_of(line): line for line in left[tick]}
        rmap = {key_of(line): line for line in right[tick]}
        shared = [key for key in lmap if key in rmap and lmap[key] != rmap[key]]
        only_left = sorted(str(key) for key in lmap if key not in rmap)
        only_right = sorted(str(key) for key in rmap if key not in lmap)
        if not shared and not only_left and not only_right:
            continue
        report["tick"] = tick
        report["only_left"] = only_left[:options.max_rows]
        report["only_right"] = only_right[:options.max_rows]
        for key in shared[:options.max_rows]:
            lf, rf = fields_of(lmap[key]), fields_of(rmap[key])
            differing = {name: {"left": lf[name], "right": rf.get(name)} for name in lf if lf[name] != rf.get(name)}
            report["rows"].append({"object": str(key), "fields": differing,
                                   "left_line": lmap[key], "right_line": rmap[key]})
        break
    text = json.dumps(report, indent=2)
    if options.json:
        options.json.write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
