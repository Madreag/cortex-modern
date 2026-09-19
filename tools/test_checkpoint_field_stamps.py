"""Census the checkpoint's archived fields and the setters that write them without a stamp.

`Scene::SaveSceneObject` caches the whole serialized text of one SceneObject under one key and
serves it again while `Entity::CheckpointWriteGeneration` has not moved, children included. So a
setter that writes a field the checkpoint carries and does not call `TouchCheckpoint` makes the
archive serve that object's every field from an earlier tick. `PostTravel` stamps whatever moved,
which hides the whole class for anything in flight; it is the resting, held and attached objects
that carry the stale text.

The scan is static and bounded: it reads headers and sources and never launches the engine.

  ARCHIVED FIELDS come from the three writers a checkpoint uses -
    `writer.NewPropertyWithValue("Name", m_Field)` inside a SaveSnapshotConfiguration,
    `archive(... m_Field ...)` inside a Save*Runtime / SaveCheckpoint,
    `self.m_Field` inside a VisitCheckpoint template.
  WRITERS include inline and out-of-line setters, resets, clears and container mutations.

ENFORCED SCOPE: the classes this tree has finished stamping (see ENFORCED below) fail the suite on
an unstamped setter of an archived field. Every other class is printed as an inventory - the fix
column for the lanes that finish them - and does not fail the run. Widening ENFORCED is the point
of those lanes; nothing here may be narrowed to make a failure go away.

Run: python tools/test_checkpoint_field_stamps.py --repo <tree> [--out result.json]
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

# The headers the census reads. Atom is not an Entity and is archived through its owner, so it is
# held to the same rule through the seam it got for that.
HEADER_GLOBS = ("Source/Entities/*.h", "Source/System/Atom.h", "Source/System/Controller.h")
SOURCE_GLOBS = ("Source/Entities/*.cpp", "Source/System/Atom.cpp", "Source/System/Controller.cpp")

# Classes whose archived-field setters are all stamped today. A class joins this list when its lane
# has stamped it, never to silence a failure.
ENFORCED = ("ACDropShip", "ACRocket", "ACrab", "ACraft", "ADoor", "ADSensor", "AEJetpack", "AEmitter",
            "AHuman", "Activity", "Actor", "Area", "Arm", "Atom", "AtomGroup", "Attachable", "BunkerAssembly",
            "BunkerAssemblyScheme", "Controller", "Deployment", "DynamicSong", "DynamicSongSection", "Emission",
            "Exit", "Gib", "GlobalScript", "HDFirearm", "HeldDevice", "Icon", "Leg", "LimbPath", "Loadout",
            "MOPixel", "MOSParticle", "MOSRotating", "MOSprite", "Magazine", "Material", "MetaPlayer", "MetaSave",
            "MovableObject", "PEmitter", "PieMenu", "PieSlice", "Round", "SLBackground", "SLTerrain", "SOPlacer",
            "Scene", "SceneLayerImpl", "SceneObject", "SoundContainer", "SoundSet", "TDExplosive", "TerrainDebris",
            "TerrainFrosting", "TerrainObject", "ThrownDevice", "Turret")

ARCHIVED_PROPERTY = re.compile(r"NewPropertyWithValue\(\s*\"[^\"]*\"\s*,\s*(?:CheckpointText\()?\s*(m_[A-Za-z0-9_]+)")
ARCHIVED_MEMBER = re.compile(r"\b(?:self\.)?(m_[A-Za-z0-9_]+)")
SETTER = re.compile(
    r"\b(?P<qualifier>(?:[A-Za-z_]\w*(?:<[^;{}\n]*>)?::)*)"
    r"(?P<name>(?:Set|Add|Clear|Reset|Enable|Disable|Remove|Request)[A-Za-z0-9_]*)\s*"
    r"\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?(?:final\s*)?\{", re.MULTILINE)
ASSIGNS = re.compile(r"\b(m_[A-Za-z0-9_]+)(?:\s*\[[^;\]\n]+\]|\.[A-Za-z_]\w*)*\s*(?:=(?!=)|[+*/%&|^-]=|\+\+|--)")
PREFIX_WRITE = re.compile(r"(?:\+\+|--)\s*(m_[A-Za-z0-9_]+)")
MEMBER_WRITE = re.compile(r"\b(m_[A-Za-z0-9_]+)(?:\s*\[[^;\]\n]+\])*\s*\.\s*(?:Set\w*|Reset\w*|Clear\w*|fill|assign|swap|clear|erase|insert|emplace\w*|push_\w+|pop_\w+|resize)\s*\(")
CLASS_HEAD = re.compile(r"^\s*class\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)\b", re.MULTILINE)
ARCHIVE_CALL = re.compile(r"\barchive\s*\(", re.MULTILINE)
CHECKPOINT_WRITER = re.compile(r"(SaveSnapshotConfiguration|SaveCheckpoint|Save[A-Za-z]*Runtime|VisitCheckpoint)")
WRITER_BODY = re.compile(r"\b(?:Save(?:[A-Za-z]*Runtime|SnapshotConfiguration|Checkpoint)?|VisitCheckpoint)\s*\([^;{}]*\)\s*(?:const\s*)?(?:override\s*)?\{", re.MULTILINE)


def balanced(text: str, start: int, opener: str = "(", closer: str = ")") -> str:
    """The text from `start` (at an opener) through its matching closer."""
    depth = 0
    for index in range(start, len(text)):
        if text[index] == opener:
            depth += 1
        elif text[index] == closer:
            depth -= 1
            if depth == 0:
                return text[start:index + 1]
    return text[start:]


def archived_fields(repo: Path) -> set[str]:
    """Every member the checkpoint's own writers put into an archive."""
    fields: set[str] = set()
    for pattern in (*HEADER_GLOBS, *SOURCE_GLOBS, "Source/System/*.cpp", "Source/System/*.h"):
        for path in sorted(repo.glob(pattern)):
            text = path.read_text(encoding="utf-8", errors="replace")
            for match in ARCHIVED_PROPERTY.finditer(text):
                fields.add(match.group(1))
            source = code_only(text)
            for writer in WRITER_BODY.finditer(source):
                fields.update(ARCHIVED_MEMBER.findall(balanced(source, writer.end() - 1, "{", "}")))
            # An archive(...) call inside a checkpoint writer names its fields positionally.
            for call in ARCHIVE_CALL.finditer(text):
                head = text.rfind("\n\n", 0, call.start())
                window = text[max(0, call.start() - 2000):call.start()]
                if not CHECKPOINT_WRITER.search(window):
                    continue
                for member in ARCHIVED_MEMBER.finditer(balanced(text, call.end() - 1)):
                    fields.add(member.group(1))
    return fields


def class_of(text: str, position: int) -> str:
    """Find the innermost class body, excluding forward declarations and closed nested classes."""
    best = ""
    for match in CLASS_HEAD.finditer(text, 0, position):
        brace = text.find("{", match.end())
        semicolon = text.find(";", match.end())
        if brace < 0 or (semicolon >= 0 and semicolon < brace):
            continue
        if brace < position < brace + len(balanced(text, brace, "{", "}")):
            best = match.group("name")
    return best


def code_only(text: str) -> str:
    """Blank comments and literals without moving a source location."""
    tokens = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')
    return tokens.sub(lambda match: re.sub(r"[^\n]", " ", match.group()), text)


def unstamped_setters(repo: Path, fields: set[str]) -> list[tuple[str, str, str, int, str]]:
    """(class, writer, field, line, path) for each archived write without a stamp."""
    findings: list[tuple[str, str, str, int, str]] = []
    for pattern in (*HEADER_GLOBS, *SOURCE_GLOBS):
        for path in sorted(repo.glob(pattern)):
            text = code_only(path.read_text(encoding="utf-8", errors="replace"))
            for match in SETTER.finditer(text):
                body = balanced(text, match.end() - 1, "{", "}")
                if re.search(r"\b(?:TouchCheckpoint|CheckpointChange)\b", body):
                    continue
                written = set(ASSIGNS.findall(body) + PREFIX_WRITE.findall(body) + MEMBER_WRITE.findall(body)) & fields
                if not written:
                    continue
                line = text.count("\n", 0, match.start()) + 1
                qualifier = match.group("qualifier").rstrip(":")
                owner = re.sub(r"<.*>", "", qualifier.split("::")[-1]) if qualifier else class_of(text, match.start())
                if not owner:
                    continue
                for field in sorted(written):
                    findings.append((owner, match.group("name"), field, line, str(path.relative_to(repo)).replace("\\", "/")))
    return findings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    repo = args.repo.resolve()

    fields = archived_fields(repo)
    if len(fields) < 50:
        print(f"[checkpoint-field-stamps] FAIL archived_field_census exit=1 only {len(fields)} fields found; the writers moved")
        return 1
    findings = unstamped_setters(repo, fields)
    enforced = [item for item in findings if item[0] in ENFORCED]
    inventory = [item for item in findings if item[0] not in ENFORCED]

    print(f"[checkpoint-field-stamps] archived fields: {len(fields)}; unstamped setters: {len(findings)}")
    for owner, setter, field, line, path in inventory:
        print(f"[checkpoint-field-stamps] INVENTORY {owner}::{setter} writes {field} unstamped  {path}:{line}")
    for owner, setter, field, line, path in enforced:
        print(f"[checkpoint-field-stamps] FAIL {owner}::{setter} writes archived {field} with no TouchCheckpoint  {path}:{line}")

    passed = not enforced
    print(f"[checkpoint-field-stamps] {'PASS' if passed else 'FAIL'} archived_setters_stamp_their_object "
          f"enforced={len(ENFORCED)} classes, {len(enforced)} unstamped in them, {len(inventory)} awaiting a lane")
    if args.out:
        args.out.write_text(json.dumps({
            "archived_fields": sorted(fields),
            "enforced_classes": list(ENFORCED),
            "unstamped_enforced": [list(item) for item in enforced],
            "unstamped_inventory": [list(item) for item in inventory],
            "passed": passed,
        }, indent=2), encoding="utf-8")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
