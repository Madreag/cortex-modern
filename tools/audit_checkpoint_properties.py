"""Inventory native Lua APIs and named checkpoint evidence without inferring correctness."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def calls(text, name):
    for match in re.finditer(r"\." + name + r"\s*\(", text):
        start = cursor = match.end()
        depth = 1
        quoted = False
        escaped = False
        args = []
        while cursor < len(text) and depth:
            char = text[cursor]
            if quoted:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == '"':
                    quoted = False
            elif char == '"':
                quoted = True
            elif char in "([{":
                depth += 1
            elif char in ")]}":
                depth -= 1
                if depth == 0:
                    args.append(text[start:cursor].strip())
            elif char == "," and depth == 1:
                args.append(text[start:cursor].strip())
                start = cursor + 1
            cursor += 1
        if depth:
            raise ValueError(f"Unterminated {name} at {match.start()}")
        yield match.start(), args


def inventory():
    rows, parents = [], {}
    for binding in sorted((ROOT / "Source/Lua").glob("LuaBindings*.cpp")):
        text = binding.read_text(encoding="utf-8")
        sections = list(re.finditer(r"LuaBindingRegisterFunctionDefinitionForType\(\w+,\s*(\w+)\)\s*\{", text))
        for index, section in enumerate(sections):
            cls = section[1]
            end = sections[index + 1].start() if index + 1 < len(sections) else len(text)
            body = text[section.end():end]
            parent = re.search(r"(?:Concrete|Abstract)TypeLuaClassDefinition\(\w+,\s*(\w+)\)", body)
            if parent:
                parents[cls] = parent[1]
            for kind in ("property", "def_readwrite", "def_readonly", "def"):
                for offset, args in calls(body, kind):
                    if not args or not re.fullmatch(r'"[^"]+"', args[0]):
                        continue
                    prefix = body[:offset].rsplit("\n", 1)[-1]
                    if "//" in prefix:
                        continue
                    writable = kind == "def_readwrite" or (kind == "property" and len(args) >= 3 and not args[2].startswith("luabind::"))
                    row = {
                        "class": cls, "api": args[0][1:-1], "kind": kind,
                        "writable_property": writable,
                        "binding": str(binding.relative_to(ROOT)).replace("\\", "/"),
                        "line": text.count("\n", 0, section.end() + offset) + 1,
                        "native": args[1:],
                        "assessment": "unreviewed",
                        "evidence": [],
                    }
                    if kind in ("property", "def_readonly") and not writable:
                        row["assessment"] = "read access; returned aliases and side effects need review"
                    rows.append(row)
    unique = {}
    for row in rows:
        key = (row["class"], row["api"], row["kind"], tuple(row["native"]))
        if key in unique:
            unique[key].setdefault("duplicate_declarations", []).append({"path": row["binding"], "line": row["line"]})
        else:
            unique[key] = row
    return list(unique.values()), parents


def save_candidates(rows, parents):
    sources = {p.stem: p for folder in ("Entities", "System", "Managers") for p in (ROOT / "Source" / folder).glob("*.cpp")}
    scene = ROOT / "Source/Entities/Scene.cpp"
    for row in rows:
        candidates = {row["api"]}
        for native in row["native"]:
            candidates.update(re.findall(r"::(?:Get|Set|Is)(\w+)", native))
        candidates = {name.lower() for name in candidates}
        ancestry, cls = [], row["class"]
        while cls and cls not in ancestry:
            ancestry.append(cls)
            cls = parents.get(cls)
        row["ancestry"] = ancestry
        row["writer_name_matches"] = []
        files = {sources[cls] for cls in ancestry if cls in sources}
        if "MovableObject" in ancestry:
            files.add(scene)
        for path in sorted(files):
            for line, code in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
                match = re.search(r'writer\.NewProperty(?:WithValue)?\("([^"]+)"', code)
                if match and match[1].removeprefix("SpecialBehaviour_").lower() in candidates:
                    row["writer_name_matches"].append({"path": str(path.relative_to(ROOT)).replace("\\", "/"), "line": line, "property": match[1]})


def add_evidence(rows, parents, directory):
    if directory is None:
        return {"verified": False, "reason": "no execution evidence supplied"}
    result = json.loads((directory / "result.json").read_text(encoding="utf-8"))
    provenance = json.loads((directory / "provenance.json").read_text(encoding="utf-8"))
    if not all(result.get(key) for key in ("pass", "complete", "source_unchanged")):
        raise ValueError("Evidence must be a completed passing run on unchanged source")
    fixture = Path(provenance["script"]["path"])
    if hashlib.sha256(fixture.read_bytes()).hexdigest() != provenance["script"]["sha256"]:
        raise ValueError("The current fixture differs from the recorded run")
    source = fixture.read_text(encoding="utf-8")
    match = re.search(r"local function configure\(owned, count\).*?local values = \{(.*?)\};", source, re.S)
    if not match:
        raise ValueError("The named owned-actor configuration fixture was not found")
    properties = re.findall(r"\b([A-Za-z_]\w*)\s*=(?!=)", match[1])
    for prop in properties:
        cls = "AHuman"
        while cls:
            target = next((row for row in rows if row["class"] == cls and row["api"] == prop and row["writable_property"]), None)
            if target:
                target["evidence"].append({"case": "owned_ahuman_configuration", "result": str(directory / "result.json"), "modes": sorted({key.split("_")[0] for key in result["results"] if key != "reference"})})
                target["assessment"] = "exercised on Lua-owned AHuman; other owners and transitions unreviewed"
                break
            cls = parents.get(cls)
        else:
            raise ValueError(f"Fixture property {prop} has no writable binding in the AHuman hierarchy")
    return {"verified": True, "directory": str(directory), "fixture_properties": len(properties), "source_head": provenance["head"], "fixture_sha256": provenance["script"]["sha256"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--evidence", type=Path)
    options = parser.parse_args()
    rows, parents = inventory()
    save_candidates(rows, parents)
    evidence = add_evidence(rows, parents, options.evidence)
    classes = {}
    for cls in sorted({row["class"] for row in rows}):
        selected = [row for row in rows if row["class"] == cls]
        writable = [row for row in selected if row["writable_property"]]
        classes[cls] = {"apis": len(selected), "writable_properties": len(writable), "properties_exercised": sum(bool(row["evidence"]) for row in writable), "methods_unreviewed": sum(row["kind"] == "def" for row in selected)}
    world = [row for row in rows if "MovableObject" in row["ancestry"]]
    summary = {"apis": len(rows), "classes": len(classes), "writable_properties": sum(row["writable_property"] for row in rows), "properties_exercised": sum(bool(row["evidence"]) for row in rows), "methods_unreviewed": sum(row["kind"] == "def" for row in rows), "world_writable_properties": sum(row["writable_property"] for row in world), "world_methods_unreviewed": sum(row["kind"] == "def" for row in world)}
    report = {"status": "OPEN: an API inventory and evidence index, not a correctness gate", "limits": "Writer name matches are search leads only; commented code and unrelated paths may match. Copy constructors do not prove file restoration. Evidence applies only to its named owner and tested transitions. Arbitrary Lua graphs and native APIs without these binding macros need separate review.", "head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(), "summary": summary, "evidence": evidence, "classes": classes, "apis": rows}
    options.out.mkdir(parents=True, exist_ok=False)
    (options.out / "inventory.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    lines = ["# Native checkpoint coverage audit", "", report["status"], "", report["limits"], "", f"{summary['writable_properties']} writable properties; {summary['properties_exercised']} exercised by the named fixture. {summary['methods_unreviewed']} methods still require mutation/alias review.", "", "| Class | Writable properties | Properties exercised | Methods to review |", "|---|---:|---:|---:|"]
    for cls, counts in classes.items():
        lines.append(f"| {cls} | {counts['writable_properties']} | {counts['properties_exercised']} | {counts['methods_unreviewed']} |")
    (options.out / "inventory.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(json.dumps({"summary": summary, "evidence": evidence, "out": str(options.out)}))


if __name__ == "__main__":
    main()
