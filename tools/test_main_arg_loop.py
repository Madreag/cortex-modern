"""Check that every path out of HandleMainArgs' argument loop advances the index.

`HandleMainArgs` (Source/Main.cpp) walks argv with `for (int i = 0; i < argCount;)` and a
single `++i` at the bottom of the body, so a handler that `continue`s has to advance `i`
itself. A handler that does not makes the loop re-read its own flag forever: the process
prints nothing further and never reaches the daemon boot, which is how
`-net-persistent-world` and `-net-world-fresh` hung every world run.

The scan is static and bounded - it reads the source and never launches the engine, so it
cannot be answered with a wall-clock wait.

Run: python tools/test_main_arg_loop.py --repo <tree> [--out result.json]
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

SOURCE = "Source/Main.cpp"
LOOP_HEAD = "for (int i = 0; i < argCount;)"
# Flags whose handlers must be present and must advance; the two that hung the world runs.
REQUIRED_FLAGS = ("-net-persistent-world", "-net-world-fresh")
# The cross-harness flags steer a headless run's faults and budgets; each handler refuses a run without the headless
# environment, and the rematch count also needs the records that advance its budget.
CROSS_FLAGS = {"-net-cross-ticket-rejoin": ("CCCP_HEADLESS",), "-net-cross-host-options": ("CCCP_HEADLESS",),
               "-net-cross-schedule": ("CCCP_HEADLESS",), "-net-cross-rematches": ("CCCP_HEADLESS", "CC_TEST_CROSS_RECORDS")}
ADVANCE = re.compile(r"\+\+i\b|\bi\s*\+=|\bi\s*=\s*i\s*\+|argValue\[\s*\+\+i\s*\]")
HANDLER = re.compile(r"currentArg\s*==\s*\"(?P<flag>[^\"]+)\"")


def loop_body(text: str) -> tuple[int, int, list[str]]:
    """The loop's line range (1-based, inclusive) and its lines, found from the head and the bottom `++i`."""
    lines = text.split("\n")
    head = next((n for n, line in enumerate(lines) if LOOP_HEAD in line), None)
    if head is None:
        raise SystemExit(f"{SOURCE}: no `{LOOP_HEAD}` - the loop this suite guards moved")
    depth = 0
    for n in range(head, len(lines)):
        depth += lines[n].count("{") - lines[n].count("}")
        if n > head and depth == 0:
            return head + 1, n + 1, lines[head:n + 1]
    raise SystemExit(f"{SOURCE}:{head + 1}: the argument loop's braces never close")


def enclosing_handler(body: list[str], index: int) -> tuple[int, str]:
    """Walk back from a `continue` to the `if (... currentArg == ...)` whose block it leaves."""
    depth = 0
    for n in range(index, -1, -1):
        depth += body[n].count("}") - body[n].count("{")
        if depth < 0 and "currentArg" in body[n]:
            return n, body[n].strip()
    return index, "<no enclosing currentArg test>"


def handler_extent(body: list[str], start: int) -> int:
    """The last line of the handler block that opens on `start`."""
    depth = 0
    for n in range(start, len(body)):
        depth += body[n].count("{") - body[n].count("}")
        if depth <= 0 and (n > start or "{" in body[n]):
            return n
    return start


def check(repo: Path) -> dict[str, list[dict]]:
    path = repo / SOURCE
    text = path.read_text(encoding="utf-8", errors="replace")
    first, _last, body = loop_body(text)
    rows: dict[str, list[dict]] = {"every_continue_advances_the_index": [], "world_flags_are_handled_and_advance": [],
                                   "cross_flags_require_headless": []}

    for n, line in enumerate(body):
        if line.strip() != "continue;":
            continue
        start, source = enclosing_handler(body, n)
        if not ADVANCE.search("\n".join(body[start:n + 1])):
            rows["every_continue_advances_the_index"].append(
                {"file": str(path), "line": first + n, "text": f"continue leaves {source} without advancing i"})

    handled = {}
    for n, line in enumerate(body):
        found = HANDLER.search(line)
        if found:
            handled.setdefault(found.group("flag"), n)
    for flag in REQUIRED_FLAGS:
        start = handled.get(flag)
        if start is None:
            rows["world_flags_are_handled_and_advance"].append(
                {"file": str(path), "line": first, "text": f"no handler for {flag}"})
            continue
        depth = 0
        end = start
        for n in range(start, len(body)):
            depth += body[n].count("{") - body[n].count("}")
            if n > start and depth <= 0:
                end = n
                break
        if not ADVANCE.search("\n".join(body[start:end + 1])):
            rows["world_flags_are_handled_and_advance"].append(
                {"file": str(path), "line": first + start, "text": f"{flag}'s handler never advances i - the loop re-reads it forever"})
    for flag, needed in CROSS_FLAGS.items():
        start = handled.get(flag)
        if start is None:
            rows["cross_flags_require_headless"].append({"file": str(path), "line": first, "text": f"no handler for {flag}"})
            continue
        block = "\n".join(body[start:handler_extent(body, start) + 1])
        for name in needed:
            if name not in block:
                rows["cross_flags_require_headless"].append(
                    {"file": str(path), "line": first + start, "text": f"{flag}'s handler never checks {name}"})
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, help="write the result JSON here")
    options = parser.parse_args()
    rows = check(options.repo.resolve())
    failed = 0
    for name, offenders in rows.items():
        if offenders:
            failed += 1
            for row in offenders:
                print("{}:{}: {}".format(row["file"], row["line"], row["text"]))
            print("[main-arg-loop] {} FAIL: {} observed".format(name, len(offenders)))
        else:
            print("[main-arg-loop] {} PASS".format(name))
    result = {"pass": failed == 0, "rows": rows, "repo": str(options.repo.resolve())}
    if options.out:
        options.out.parent.mkdir(parents=True, exist_ok=True)
        options.out.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print("[main-arg-loop] " + ("PASS" if result["pass"] else "FAIL"))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
