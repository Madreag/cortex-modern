"""Check tools/sanitizers/ubsan.supp and apply it to UBSan logs the way the runtime does.

    python tools/sanitizers/check_ubsan_supp.py --self-test
    python tools/sanitizers/check_ubsan_supp.py <log> [<log> ...] [--supp <file>]

The file may name only the reviewed third-party source files, one known check per entry, each entry
preceded by a comment with its reason; anything else is refused (exit 2). Given logs, it prints every UBSan report the
file would NOT suppress (a report's check comes from its message, its match is UBSan's substring/glob rule on the
report's source file), then the kept and suppressed counts. A report whose message maps to no known check is kept.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

DEFAULT_SUPP = Path(__file__).resolve().parent / "ubsan.supp"
ALLOWED_FILES = {
    "external/sources/LuaJIT-2.1/src/" + name
    for name in ("lj_api.c", "lj_func.c", "lib_base.c", "lib_jit.c", "lj_gc.c", "lj_asm_x86.h", "lj_emit_x86.h", "lj_asm.c")
} | {
    "external/sources/allegro 4.4.3.1-custom/src/file.c",
    "external/sources/allegro 4.4.3.1-custom/src/unicode.c",
    "src/steamnetworkingsockets/clientlib/steamnetworkingsockets_lowlevel.h",
}
REPORT = re.compile(r"^(?P<file>.+?):(?P<line>\d+):(?P<col>\d+): runtime error: (?P<message>.*)")
SUMMARY = re.compile(r"SUMMARY: UndefinedBehaviorSanitizer: [\w-]+ (?P<file>.+?):(?P<line>\d+):(?P<col>\d+)")

# The suppression name UBSan files each report under (compiler-rt ubsan_checks.inc), keyed by its message.
CHECKS = [
    ("vptr", re.compile(r"which does not point to an object of type|address .* with insufficient space for an object of type .*vptr")),
    ("null", re.compile(r"(member access within|member call on|load of|store to|reference binding to) null pointer")),
    ("alignment", re.compile(r"misaligned address")),
    ("shift-base", re.compile(r"left shift of (negative value|.* places cannot be represented)")),
    ("shift-exponent", re.compile(r"shift exponent .* (is too large|is negative)")),
    ("signed-integer-overflow", re.compile(r"signed integer overflow|negation of .* cannot be represented")),
    ("unsigned-integer-overflow", re.compile(r"unsigned integer overflow")),
    ("integer-divide-by-zero", re.compile(r"division by zero")),
    ("bounds", re.compile(r"index .* out of bounds")),
    ("pointer-overflow", re.compile(r"pointer index expression|applying (non-)?zero offset")),
    ("bool", re.compile(r"is not a valid value for type 'bool'")),
    ("enum", re.compile(r"is not a valid value for type")),
    ("function", re.compile(r"through pointer to incorrect function type")),
    ("nonnull-attribute", re.compile(r"null pointer passed as argument")),
    ("returns-nonnull-attribute", re.compile(r"null pointer returned from function")),
    ("float-cast-overflow", re.compile(r"is outside the range of representable values")),
    ("vla-bound", re.compile(r"variable length array bound")),
    ("unreachable", re.compile(r"execution reached an unreachable program point")),
]
KNOWN = {name for name, _ in CHECKS} | {"undefined"}


def template_match(template: str, text: str) -> bool:
    """compiler-rt's TemplateMatch: substring match, '*' any run, '^' anchors the start, '$' the end."""
    if not text:
        return False
    start = template.startswith("^")
    if start:
        template = template[1:]
    asterisk = False
    while template:
        if template[0] == "*":
            template, start, asterisk = template[1:], False, True
            continue
        if template[0] == "$":
            return not text or asterisk
        if not text:
            return False
        cut = min([i for i in (template.find("*"), template.find("$")) if i >= 0], default=len(template))
        piece, template = template[:cut], template[cut:]
        position = text.find(piece)
        if position < 0 or (start and position != 0):
            return False
        text, start = text[position + len(piece):], False
    return True


def check_of(message: str) -> str | None:
    for name, pattern in CHECKS:
        if pattern.search(message):
            return name
    return None


def parse(text: str) -> tuple[list[tuple[str, str]], list[str]]:
    """Refuse unreviewed paths, unknown checks, and entries without a reason."""
    entries, errors, reason = [], [], False
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line:
            reason = False
            continue
        if line.startswith("#"):
            reason = True
            continue
        check, _, pattern = line.partition(":")
        if check not in KNOWN or not pattern:
            errors.append(f"line {number}: '{line}' is not <check>:<path> with a known check")
            continue
        if pattern not in ALLOWED_FILES:
            errors.append(f"line {number}: '{pattern}' is not a reviewed third-party file")
            continue
        if not reason:
            errors.append(f"line {number}: '{line}' has no reason comment above it")
        entries.append((check, pattern))
        reason = False
    return entries, errors


def suppressed(entries: list[tuple[str, str]], check: str | None, filename: str) -> bool:
    return check is not None and any(kind in (check, "undefined") and template_match(pattern, filename)
                                     for kind, pattern in entries)


def filter_lines(entries: list[tuple[str, str]], lines: list[str]) -> tuple[list[str], int]:
    kept, count, hidden = [], 0, set()
    for line in lines:
        report = REPORT.search(line)
        if report:
            if suppressed(entries, check_of(report["message"]), report["file"]):
                count += 1
                hidden.add((report["file"], report["line"], report["col"]))
            else:
                kept.append(line.strip())
            continue
        summary = SUMMARY.search(line)
        if summary:
            if (summary["file"], summary["line"], summary["col"]) in hidden:
                count += 1
            else:
                kept.append(line.strip())
    return kept, count


def self_test() -> int:
    failures = []
    shipped, errors = parse(DEFAULT_SUPP.read_text(encoding="utf-8"))
    if errors or not shipped:
        failures.append(f"the shipped file is refused or empty: {errors}")
    for bad in ("# r\nnull:../Source/Network/GnsTransport.cpp", "# r\nvptr:GnsTransport.cpp", "# r\nnull:external/sources/*",
                "null:external/sources/LuaJIT-2.1/src/lj_api.c", "# r\nnotacheck:external/sources/LuaJIT-2.1/src/x.c"):
        if not parse(bad)[1]:
            failures.append(f"accepted {bad!r}")
    log = [
        "../external/sources/LuaJIT-2.1/src/lj_api.c:1030:5: runtime error: member access within null pointer of type 'GCobj' (aka 'union GCobj')",
        "SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior ../external/sources/LuaJIT-2.1/src/lj_api.c:1030:5 in ",
        "../external/sources/LuaJIT-2.1/src/lj_func.c:153:46: runtime error: left shift of 49156 by 24 places cannot be represented in type 'int32_t' (aka 'int')",
        "../external/sources/LuaJIT-2.1/src/lib_base.c:139:3: runtime error: member access within null pointer of type 'GCobj' (aka 'union GCobj')",
        "../external/sources/LuaJIT-2.1/src/lib_jit.c:164:24: runtime error: left shift of negative value -1294396313",
        "../external/sources/LuaJIT-2.1/src/lj_api.c:77:9: runtime error: signed integer overflow: 2147483647 + 1 cannot be represented in type 'int'",
        "../Source/Network/GnsTransport.cpp:254:34: runtime error: member call on address 0x61c0000cb080 which does not point to an object of type 'ISteamNetworkingSockets'",
        "SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior ../Source/Network/GnsTransport.cpp:254:34 in ",
        "../Source/Lua/LuaMan.cpp:10:1: runtime error: member access within null pointer of type 'GCobj' (aka 'union GCobj')",
    ]
    kept, count = filter_lines(shipped, log)
    if count != 5:
        failures.append(f"suppressed {count}, want 5 (the four LuaJIT reports and one summary)")
    for want in (log[5], log[6], log[7], log[8]):
        if want.strip() not in kept:
            failures.append(f"not kept: {want[:90]}")
    for template, want in (("a/b.c", True), ("^a/b.c", False), ("a*c", True), ("b.c$", True), ("b$", False)):
        if template_match(template, "../a/b.c") != want:
            failures.append(f"template_match({template!r}, '../a/b.c') != {want}")
    for failure in failures:
        print(f"[check_ubsan_supp self-test] FAIL {failure}")
    print(f"[check_ubsan_supp self-test] {'PASS' if not failures else 'FAIL'} {len(failures)} failure(s)")
    return 0 if not failures else 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("logs", nargs="*", type=Path)
    parser.add_argument("--supp", type=Path, default=DEFAULT_SUPP)
    parser.add_argument("--self-test", action="store_true")
    options = parser.parse_args(argv)
    if options.self_test:
        return self_test()
    entries, errors = parse(options.supp.read_text(encoding="utf-8"))
    for error in errors:
        print(f"REFUSED {options.supp}: {error}")
    if errors:
        return 2
    total_kept, total_hidden = [], 0
    for log in options.logs:
        kept, hidden = filter_lines(entries, log.read_text(encoding="utf-8", errors="replace").splitlines())
        total_hidden += hidden
        total_kept += [f"{log}: {line}" for line in kept]
    for line in total_kept:
        print(f"KEPT {line}")
    print(f"kept {len(total_kept)} suppressed {total_hidden} ({len(entries)} entries, {len(options.logs)} logs)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
