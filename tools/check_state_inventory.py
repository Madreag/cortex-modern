#!/usr/bin/env python
"""State inventory check for the snapshot classes.

Every data member of the classes a faithful clone copies (the movable-object tree, its atom groups,
limb paths, timers and controllers) must be either copied by that class's Create(reference) — the
copy constructor path the snapshot and the preview use — or classified in
Source/System/StateInventory.csv with a reason (reconstructed, transient, render, identity, link,
outside). A member that is neither is a hole the fidelity gates may not see yet, so the check
fails on it. Run: python tools/check_state_inventory.py [--report]

Exit 0 = every member accounted for, 1 = unclassified members (listed).
"""

import argparse
import csv
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENTITIES = ROOT / "Source" / "Entities"
SYSTEM = ROOT / "Source" / "System"
CSV_PATH = SYSTEM / "StateInventory.csv"

# The classes whose instances (or sub-objects) a faithful clone carries.
CLASSES = {
    "MovableObject": ENTITIES,
    "MOSprite": ENTITIES,
    "MOSRotating": ENTITIES,
    "MOSParticle": ENTITIES,
    "MOPixel": ENTITIES,
    "Attachable": ENTITIES,
    "Actor": ENTITIES,
    "AHuman": ENTITIES,
    "ACrab": ENTITIES,
    "ACraft": ENTITIES,
    "ACRocket": ENTITIES,
    "ACDropShip": ENTITIES,
    "ADoor": ENTITIES,
    "Arm": ENTITIES,
    "Leg": ENTITIES,
    "Turret": ENTITIES,
    "HeldDevice": ENTITIES,
    "HDFirearm": ENTITIES,
    "ThrownDevice": ENTITIES,
    "TDExplosive": ENTITIES,
    "Magazine": ENTITIES,
    "AEmitter": ENTITIES,
    "AEJetpack": ENTITIES,
    "PEmitter": ENTITIES,
    "Emission": ENTITIES,
    "AtomGroup": ENTITIES,
    "Atom": SYSTEM,
    "LimbPath": ENTITIES,
    "Controller": SYSTEM,
    "Timer": SYSTEM,
    "PieMenu": ENTITIES,
    "PieSlice": ENTITIES,
    "SoundSet": ENTITIES,
}
CLASSIFICATIONS = {
    "copied",
    "reconstructed",
    "transient",
    "render",
    "identity",
    "link",
    "outside",
}

MEMBER = re.compile(
    r"^\s*(?:static\s+)?(?:const\s+)?(?:inline\s+)?[A-Za-z_][\w:<>,\s\*&\.]*?[\s\*&]+(m_[A-Za-z0-9_]+)\s*(?:\[[^\]]*\])*\s*(?:=|;|\{)"
)
STATIC = re.compile(r"^\s*static\b")


def members_of(header: Path):
    found = []
    seen = set()
    in_class = False
    depth = 0
    text = re.sub(
        r"/\*.*?\*/",
        "",
        header.read_text(encoding="utf-8", errors="replace"),
        flags=re.S,
    )
    for line in text.splitlines():
        stripped = line.strip()
        if (
            stripped.startswith("//")
            or stripped.startswith("///")
            or stripped.startswith("return ")
        ):
            continue
        if re.match(r"^\s*class\s+\w+", line) and "{" in line:
            in_class = True
        if not in_class:
            continue
        if STATIC.match(line):
            continue
        m = MEMBER.match(line)
        if m and "(" not in line.split(m.group(1))[0]:
            name = m.group(1)
            if name not in seen:
                seen.add(name)
                found.append(name)
    return found


def create_body(cpp: Path, cls: str):
    text = cpp.read_text(encoding="utf-8", errors="replace")
    m = re.search(
        r"int\s+"
        + re.escape(cls)
        + r"::Create\(const\s+"
        + re.escape(cls)
        + r"&\s+(\w+)(?:,[^)]*)?\)\s*\{",
        text,
    )
    if not m:
        return "", ""
    ref = m.group(1)
    start = m.end()
    depth = 1
    i = start
    while i < len(text) and depth > 0:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    return text[start:i], ref


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--report", action="store_true", help="print every member with its status"
    )
    args = ap.parse_args()

    classified = {}
    if CSV_PATH.exists():
        with CSV_PATH.open(encoding="utf-8", newline="") as f:
            for row in csv.DictReader(f):
                key = (row["class"].strip(), row["member"].strip())
                classification = row["classification"].strip()
                if classification not in CLASSIFICATIONS:
                    print(
                        f"FAIL: {CSV_PATH.name}: {key} has unknown classification '{classification}'",
                        file=sys.stderr,
                    )
                    return 1
                if not row["reason"].strip():
                    print(
                        f"FAIL: {CSV_PATH.name}: {key} has no reason", file=sys.stderr
                    )
                    return 1
                classified[key] = (classification, row["reason"].strip())

    counts = {"copied": 0, **{c: 0 for c in CLASSIFICATIONS}}
    unclassified = []
    stale = set(classified)
    total = 0
    for cls, folder in CLASSES.items():
        header = folder / f"{cls}.h"
        cpp = folder / f"{cls}.cpp"
        if not header.exists():
            print(f"FAIL: missing header {header}", file=sys.stderr)
            return 1
        body, ref = create_body(cpp, cls) if cpp.exists() else ("", "")
        for member in members_of(header):
            total += 1
            copied = (
                bool(ref)
                and re.search(
                    r"\b" + re.escape(ref) + r"\." + re.escape(member) + r"\b", body
                )
                is not None
            )
            key = (cls, member)
            if copied:
                counts["copied"] += 1
                status = "copied"
                stale.discard(key)
            elif key in classified:
                status, reason = classified[key]
                counts[status] += 1
                stale.discard(key)
                status = f"{status}: {reason}"
            else:
                unclassified.append(key)
                status = "UNCLASSIFIED"
            if args.report:
                print(f"{cls}.{member}: {status}")
    for key in sorted(stale):
        print(
            f"NOTE: {CSV_PATH.name} row {key} names a member the headers no longer declare, or one Create(reference) now copies",
            file=sys.stderr,
        )
    print(
        "state inventory: "
        + ", ".join(f"{k}={v}" for k, v in counts.items())
        + f", total={total}, unclassified={len(unclassified)}"
    )
    if unclassified:
        for cls, member in unclassified:
            print(f"UNCLASSIFIED: {cls}.{member}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
