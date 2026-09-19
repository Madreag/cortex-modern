"""Check the print discipline the drivers depend on: one tag per line, one write per line.

A diagnostic line a parser reads has to reach the log whole. A multi-inserter print
(``std::cout << tag << field << ...``) lets another thread's own line land inside it, so
every tagged line goes out through the single-write helpers in Source/System/System.cpp.
Source only; no engine, no launch.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

MENU_SCRIPT_STREAMED = re.compile(r'std::(?:cout|cerr)\s*<<\s*(?:std::format\()?"\[menu-script\]')
FPRINTF_STDERR = re.compile(r'std::fprintf\(stderr,\s*"RTE ')
WRITER = re.compile(r"void WriteWholeLine\(std::ostream& stream, const std::string& line\)")
LOCKED_WRITE = re.compile(r"std::scoped_lock printLock\(PrintLock\(\)\);")


def hits(path: Path, pattern: re.Pattern) -> list:
    if not path.is_file():
        return [{"file": str(path), "line": 0, "text": "file missing"}]
    found = []
    for number, text in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        if pattern.search(text):
            found.append({"file": str(path), "line": number, "text": text.strip()})
    return found


def check(repo: Path) -> dict:
    main, rte_error, system = repo / "Source/Main.cpp", repo / "Source/System/RTEError.cpp", repo / "Source/System/System.cpp"
    system_text = system.read_text(encoding="utf-8", errors="replace") if system.is_file() else ""
    rows = {
        "menu_script_lines_are_one_write": hits(main, MENU_SCRIPT_STREAMED),
        "rte_error_lines_use_the_print_lock": hits(rte_error, FPRINTF_STDERR),
        "menu_script_helper_present": [] if "void MenuScriptPrint(const std::string& line)" in
                                      main.read_text(encoding="utf-8", errors="replace") else
                                      [{"file": str(main), "line": 0, "text": "MenuScriptPrint helper missing"}],
        "one_locked_write_per_line": [] if WRITER.search(system_text) and LOCKED_WRITE.search(system_text) else
                                     [{"file": str(system), "line": 0, "text": "WriteWholeLine under PrintLock missing"}],
        "print_to_cli_writes_whole_lines": [] if 'WriteWholeLine(std::cout, "\\r" + stringToPrint)' in system_text else
                                           [{"file": str(system), "line": 0, "text": "PrintToCLI does not use WriteWholeLine"}],
    }
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
            print("[print-discipline] {} FAIL: {} observed".format(name, len(offenders)))
        else:
            print("[print-discipline] {} PASS".format(name))
    result = {"pass": failed == 0, "rows": {name: offenders for name, offenders in rows.items()},
              "repo": str(options.repo.resolve())}
    if options.out:
        options.out.parent.mkdir(parents=True, exist_ok=True)
        options.out.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print("[print-discipline] " + ("PASS" if result["pass"] else "FAIL"))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
