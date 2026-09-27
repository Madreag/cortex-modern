"""Check that the coordinator hands the frame copies, never references into the round a plane tick may change.

The session plane ticks the lockstep coordinator on its own thread while a window is open; a caller that keeps a reference
or an iterator into the coordinator's state past the plane's lock reads memory a tick may be rewriting. Every getter of
NetLockstepCoordinator (Source/Network/NetLockstep.h) that returns a reference is listed here with the reason it may, and
every getter the frame reads round state through is listed as one that must return a value. A new reference getter, or a
value getter turned back into a reference, fails the row. The scan reads the header only.

Run: python tools/test_plane_value_getters.py --repo <tree> [--out result.json]
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

HEADER = "Source/Network/NetLockstep.h"
RUNNER_HEADER = "Source/System/ScenarioRunner.h"
CLASS_HEAD = "class NetLockstepCoordinator"
# Round state the frame reads: each must come back as a copy.
VALUE_GETTERS = ("HeldTransactions", "GetRoundConfigHash", "GetAgreedStartRecord", "GetMigrationResult", "GetMigrationAddress", "GetAuthoritativeCommandAcks",
                 "GetDelayChanges", "RemoteTransports", "GetPeerLeaveFrames", "GetPeerFrameWaivers", "ObservationEpochs")
# Reference getters that remain, each with the reason; a new one has to be added here with its own.
REFERENCE_ALLOWED = {
    "GetConfig": "the plane ticks only inside the frame loop's windows (WINDOW_FILES), and no reference a reader binds lives across one (checked below); "
                 "a per-tick copy of the whole round config would cost every tick for nothing",
    "GetStats": "the same windows bound every reference, and its per-peer map's find/end pairs must read one object, which a by-value return would split",
}
# The only files that open a plane window: the frame loop. A window anywhere else could hold a reference a reader bound before it opened.
WINDOW_FILES = {"Source/Main.cpp", "Source/Network/NetLockstep.cpp"}
BINDING = re.compile(r"const\s+auto&\s+\w+\s*=\s*[^;]*\b(?:GetStats|GetConfig)\(\)[^;]*;")
GETTER = re.compile(r"^\s*(?:static\s+)?(?P<ret>(?:const\s+)?[\w:<>,\s]+?)\s*(?P<ref>&)?\s*(?P<name>[A-Za-z_]\w*)\s*\([^;{]*\)\s*const\b")


def class_body(text: str) -> str:
    head = re.search(CLASS_HEAD + r"\s*(?::[^{;]*)?\{", text)
    if not head:
        raise ValueError("no class body for " + CLASS_HEAD)
    brace = head.end() - 1
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[brace:index]
    raise ValueError("unterminated class body")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    body = class_body((args.repo / HEADER).read_text(encoding="utf-8"))
    references, values, failures = {}, {}, []
    for line in body.splitlines():
        match = GETTER.match(line)
        if not match or match.group("name") in ("operator", "if", "for", "while", "return"):
            continue
        (references if match.group("ref") else values)[match.group("name")] = line.strip()
    for name, line in sorted(references.items()):
        if name in VALUE_GETTERS:
            failures.append(f"{name} returns a reference into the round: {line}")
        elif name not in REFERENCE_ALLOWED:
            failures.append(f"{name} is a new reference getter with no stated reason: {line}")
    for name in VALUE_GETTERS:
        if name not in values and name not in references:
            failures.append(f"{name} is gone from the header; update the list")
    for path in sorted((args.repo / "Source").rglob("*.cpp")):
        rel = path.relative_to(args.repo).as_posix()
        if "SelfTest" in rel:
            # A friend test reaches a coordinator's members past every check: only the plane's own check row may let the plane tick one.
            text = path.read_text(encoding="utf-8", errors="replace")
            for match in re.finditer(r"NetLockstepPlane::Target\((?!nullptr)", text):
                owners = re.findall(r"\bbool (Test\w+)\(std::string\* error\) \{", text[:match.start()])
                if not owners or owners[-1] != "TestPlaneCheckTripsOnAnUnguardedAccess":
                    failures.append(f"{rel}:{text.count(chr(10), 0, match.start()) + 1} lets the plane tick a coordinator a friend test reaches")
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        if "NetLockstepPlane::Window" in text and rel not in WINDOW_FILES:
            failures.append(f"{rel} opens a plane window outside the frame loop; review the references its readers keep, then list it")
        if rel in WINDOW_FILES:
            continue
        for match in BINDING.finditer(text):
            # The rest of the enclosing function: the binding lives until the brace that closes the block it is in.
            depth, end = 0, len(text)
            for index in range(match.end(), len(text)):
                if text[index] == "{":
                    depth += 1
                elif text[index] == "}":
                    if depth == 0:
                        end = index
                        break
                    depth -= 1
            if "NetLockstepPlane::Window" in text[match.end():end]:
                line = text.count("\n", 0, match.start()) + 1
                failures.append(f"{rel}:{line} keeps a GetStats/GetConfig reference across a plane window")
    runner = (args.repo / RUNNER_HEADER).read_text(encoding="utf-8")
    if not re.search(r"static\s+std::optional<NetMatchConfig>\s+GetLockstepMatchConfig\(\)", runner):
        failures.append("ScenarioRunner::GetLockstepMatchConfig no longer returns a copy")
    result = {"row": "plane-value-getters", "pass": not failures, "value_getters": sorted(n for n in VALUE_GETTERS if n in values),
              "references": sorted(references), "failures": failures}
    if args.out:
        args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"[plane-value-getters] {'PASS' if not failures else 'FAIL'} values={len(result['value_getters'])} references={result['references']}")
    for failure in failures:
        print(f"[plane-value-getters] {failure}")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
