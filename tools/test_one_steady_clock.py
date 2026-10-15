"""Check that the game reads one steady clock: RTE::SteadyClock (Source/System/SteadyClock.h).

libstdc++ on macOS reads the wall clock for std::chrono::steady_clock, while RTE::SteadyClock reads the uptime
clock there; a deadline or a timestamp taken from one and compared with the other is years off on that build
(a returning player's image waited for 'a fresher base' that never came). So no source file but the clock's own
header names std::chrono::steady_clock, except the steady-clock self-test, which compares the library's clock with
the wall to say which one it is. Source scan only: no engine, no launch.

Run: python tools/test_one_steady_clock.py --repo <tree> [--out result.json]
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

CLOCK_HEADER = "Source/System/SteadyClock.h"
# The self-test that reads the library's clock on purpose, and the file it lives in.
LIBRARY_READER = ("Source/Main.cpp", "static bool RunSteadyClockSelfTest()")
TOKEN = re.compile(r"\bsteady_clock\b")
STRING = re.compile(r'"(?:\\.|[^"\\])*"')


def code_of(line: str) -> str:
    """The line without its string literals and its trailing comment."""
    return STRING.sub('""', line).split("//", 1)[0]


def reader_span(lines: list[str], signature: str) -> range:
    start = next((index for index, line in enumerate(lines) if line.startswith(signature)), None)
    if start is None:
        return range(0)
    end = next((index for index in range(start + 1, len(lines)) if lines[index] == "}"), len(lines) - 1)
    return range(start, end + 1)


def check(repo: Path) -> dict:
    offenders = []
    reader_found = False
    for path in sorted((repo / "Source").rglob("*")):
        if path.suffix not in (".h", ".hpp", ".cpp") or not path.is_file():
            continue
        name = path.relative_to(repo).as_posix()
        if name == CLOCK_HEADER:
            continue
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        allowed = range(0)
        if name == LIBRARY_READER[0]:
            allowed = reader_span(lines, LIBRARY_READER[1])
            reader_found = reader_found or len(allowed) > 0
        for index, line in enumerate(lines):
            if index not in allowed and TOKEN.search(code_of(line)):
                offenders.append({"file": name, "line": index + 1, "text": line.strip()[:200]})
    rows = {"one_steady_clock_in_source": offenders}
    rows["the_library_clock_self_test_is_present"] = [] if reader_found else \
        [{"file": LIBRARY_READER[0], "line": 0, "text": LIBRARY_READER[1] + " missing"}]
    header = repo / CLOCK_HEADER
    text = header.read_text(encoding="utf-8") if header.is_file() else ""
    rows["the_clock_has_its_own_time_point"] = [] if "using time_point = std::chrono::time_point<SteadyClock" in text else \
        [{"file": CLOCK_HEADER, "line": 0, "text": "RTE::SteadyClock with its own time_point missing"}]
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
            for row in offenders[:40]:
                print("{}:{}: {}".format(row["file"], row["line"], row["text"]))
            print("[one-steady-clock] {} FAIL: {} observed".format(name, len(offenders)))
        else:
            print("[one-steady-clock] {} PASS".format(name))
    result = {"pass": failed == 0, "rows": rows, "repo": str(options.repo.resolve())}
    if options.out:
        options.out.parent.mkdir(parents=True, exist_ok=True)
        options.out.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print("[one-steady-clock] " + ("PASS" if result["pass"] else "FAIL"))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
