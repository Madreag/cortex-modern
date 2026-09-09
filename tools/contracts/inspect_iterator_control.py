"""Show which iterator node the invalid_iterator mutation lands on, in a regenerated and a Source20 archive.

graph_controls.mutate takes the first J node of the last VM. A J node written 'o' is an owned iterator and carries no
'last' cursor; one written 'r' carries 'last', which the engine's parser reads with reader:index(node.first) — a lower
bound. So the same mutation is a parse error on an 'r' node and a semantic error on an 'o' node.
"""

import argparse
import base64
import json
import re
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, "D:/Projects/reviews/recovery-2026-09-07")
from inspect_graph import parse  # noqa: E402

PAT = re.compile(rb"(?m)^(\s*LuaStateGraph = )(\d+)\|([^\r\n]+)")


def graphs(path):
    with zipfile.ZipFile(path) as archive:
        save = archive.read("Save.ini")
    return {
        int(m[2]): base64.b64decode(
            m[3].replace(b".", b"=").replace(b"-", b"+").replace(b"_", b"/")
        )
        for m in PAT.finditer(save)
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--archive",
        type=Path,
        action="append",
        required=True,
        help="label=path of a valid_full.ccsave; repeatable",
    )
    options = parser.parse_args()
    report = []
    for entry in options.archive:
        label, _, path = str(entry).partition("=")
        raw = graphs(Path(path))
        vm = max(raw)
        graph = parse(raw[vm])
        target = next(
            (key, node) for key, node in graph["nodes"].items() if node["kind"] == "J"
        )
        key, node = target
        report.append(
            {
                "archive": label,
                "vm": vm,
                "node": key,
                "owned": node["owned"],
                "first": node["first"],
                "last": node.get("last"),
                "parse_rejects_the_mutation": not node["owned"],
                "iterator_nodes_in_vm": sum(
                    1 for n in graph["nodes"].values() if n["kind"] == "J"
                ),
                "owned_iterators": sum(
                    1
                    for n in graph["nodes"].values()
                    if n["kind"] == "J" and n["owned"]
                ),
            }
        )
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
