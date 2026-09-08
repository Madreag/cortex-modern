#!/usr/bin/env python
"""Compare shared snapshot state, or the complete per-peer state with --full.

Shared comparison projects only measured local AI, presentation and clock fields. It preserves
Lua graph identities and compares every VM, global, native payload and archive entry. Full
comparison has no local-state projection and is the separate restoration oracle.
"""

import argparse
import base64
import hashlib
import json
import math
from pathlib import Path
import re
import sys
import zipfile

if __package__:
    from . import snapshot_runtime
else:
    import snapshot_runtime


class GraphReader:
    """Read the checkpoint grammar without executing Lua or native constructors."""

    def __init__(self, data):
        self.data, self.pos = data, 0

    def char(self):
        value = self.peek()
        self.pos += 1
        return value

    def peek(self):
        if self.pos >= len(self.data):
            raise ValueError(f"truncated graph at byte {self.pos}")
        return chr(self.data[self.pos])

    def expect(self, value):
        for wanted in value:
            if self.char() != wanted:
                raise ValueError(f"expected {value!r} at byte {self.pos - 1}")

    def until(self, delimiter=";"):
        stop = self.data.find(delimiter.encode(), self.pos)
        if stop < 0:
            raise ValueError(f"unterminated graph token at byte {self.pos}")
        value = self.data[self.pos:stop].decode("ascii")
        self.pos = stop + 1
        return value

    def integer(self, delimiter=";", minimum=0):
        value = self.until(delimiter)
        if not re.fullmatch(r"-?\d+", value) or int(value) < minimum:
            raise ValueError(f"invalid graph integer {value!r}")
        return int(value)

    def count(self, delimiter=";"):
        count = self.integer(delimiter)
        if count > len(self.data) - self.pos:
            raise ValueError("graph count exceeds remaining bytes")
        return count

    def string(self, body=False):
        if not body:
            self.expect("s")
        size = self.count(":")
        value = self.data[self.pos:self.pos + size]
        self.pos += size
        return value

    def token(self):
        kind = self.char()
        if kind in "ztfAS":
            self.expect(";")
            return (kind,)
        if kind == "s":
            return kind, self.string(True)
        if kind in "nvm":
            value = self.until()
            parts = value.split(",")
            if len(parts) != {"n": 1, "v": 2, "m": 4}[kind]:
                raise ValueError(f"invalid {kind} payload")
            for part in parts:
                float(part)
            return kind, value
        if kind in "#qGM":
            return kind, self.integer(minimum=1 if kind in "#G" else 0)
        if kind == "c":
            return (kind, *[self.token() for _ in range(4)])
        if kind in "da":
            return kind, self.string()
        if kind == "P":
            return kind, tuple(self.token() for _ in range(self.count()))
        if kind in "ew":
            return kind, self.integer(":"), self.string()
        if kind == "g":
            return kind, tuple(self.string() for _ in range(self.count()))
        if kind == "p":
            return kind, self.string(), self.string(), self.string()
        if kind == "Q":
            return kind, self.string(), self.token()
        if kind in "xY":
            result = kind, self.token(), self.string(), self.token(), self.token()
            if kind == "Y":
                result += self.string(), self.string()
                if re.match(rb"\d+ [A-Za-z0-9_:]+ ", result[-1]):
                    snapshot_runtime.decode(result[-1])
            return result
        if kind in "bkh":
            owner = self.token()
            middle = self.string() if kind == "h" else self.token()
            return kind, owner, middle, self.token()
        if kind in "ijl":
            return kind, self.token(), self.token()
        raise ValueError(f"unknown graph token {kind!r} at byte {self.pos - 1}")


def graph_references(value):
    if isinstance(value, tuple) and value[:1] == ("#",):
        yield value[1]
    elif isinstance(value, dict):
        for item in value.values():
            yield from graph_references(item)
    elif isinstance(value, (tuple, list)):
        for item in value:
            yield from graph_references(item)


def parse_graph(data):
    reader = GraphReader(data)
    version = reader.until()
    if version not in ("SG1", "SG2", "SG3"):
        raise ValueError(f"unsupported graph version {version!r}")
    graph = {"version": version}
    for tag, label in (("r", "roots"), ("G", "globals"), ("L", "loaded")):
        reader.expect(tag)
        entries = {}
        for _ in range(reader.count()):
            name = reader.string()
            if name in entries:
                raise ValueError(f"duplicate {label} entry {name!r}")
            entries[name] = reader.token()
        graph[label] = entries
    if version != "SG1":
        reader.expect("E")
        graph["patches"] = [tuple(reader.token() for _ in range(3)) for _ in range(reader.count())]
    if version == "SG3":
        reader.expect("R")
        graph["rng"] = reader.token()
    if reader.peek() == "X":
        reader.expect("X")
        graph["gibs"] = [tuple(reader.token() for _ in range(3)) for _ in range(reader.count())]
    if reader.peek() == "Y":
        reader.expect("Y")
        graph["native_links"] = [(reader.token(), reader.string(), reader.token()) for _ in range(reader.count())]
    reader.expect("N")
    nodes = {}
    for expected in range(1, reader.count() + 1):
        kind, index = reader.char(), reader.integer(minimum=1)
        if index != expected:
            raise ValueError("noncontiguous or duplicate graph node ID")
        node = {"kind": kind}
        if kind == "T":
            reader.expect("P")
            if reader.peek() == "-":
                reader.expect("-;")
                node["path"] = None
            else:
                node["path"] = reader.token()
            reader.expect("M")
            node["meta"] = reader.token()
            reader.expect("k")
            node["pairs"] = [(reader.token(), reader.token()) for _ in range(reader.count())]
            keys = [key for key, _ in node["pairs"]]
            identities = [("n", float(key[1])) if key[:1] == ("n",) else key for key in keys]
            if len(keys) != len(set(identities)) or ("z",) in keys or any(key[:1] == ("n",) and math.isnan(float(key[1])) for key in keys):
                raise ValueError("duplicate or nil table key")
        elif kind == "F":
            how = reader.char()
            if how == "R":
                node["path"] = reader.token()
                if reader.peek() == "D":
                    reader.expect("D")
                    node["code"] = reader.string()
            elif how == "D":
                node["code"] = reader.string()
            else:
                raise ValueError("invalid function payload")
            reader.expect("E")
            node["env"] = reader.token()
            reader.expect("u")
            node["cells"] = []
            for _ in range(reader.count()):
                reader.expect("c")
                node["cells"].append(("#", reader.integer(minimum=1)))
        elif kind == "B":
            node["factory"] = reader.string()
            reader.expect("u")
            node["upvalues"] = [reader.token() for _ in range(reader.count())]
        elif kind == "J":
            ownership = reader.char()
            if ownership not in "or":
                raise ValueError("invalid iterator ownership")
            node["owned"] = ownership == "o"
            node["owner"], node["first"] = reader.token(), reader.token()
            if not node["owned"]:
                node["last"], node["creator"] = reader.token(), reader.token()
            reader.expect("u")
            node["values"] = [reader.token() for _ in range(reader.count())]
        elif kind == "U":
            if reader.peek() in "oO":
                node["uid"] = reader.integer() if reader.char() == "O" else None
                for key in ("class", "preset", "module", "ini"):
                    node[key] = reader.string()
            else:
                node["value"] = reader.token()
            if reader.pos < len(data) and reader.peek() == "I":
                reader.expect("I")
                node["instance"] = reader.token()
        elif kind == "C":
            if reader.peek() == "O":
                reader.expect("O")
                node["thread"], node["slot"] = reader.token(), reader.token()
            else:
                node["value"] = reader.token()
        elif kind == "H":
            node["status"] = reader.until()
            for key in ("first", "base", "top"):
                node[key] = reader.integer()
            if not 0 <= node["first"] <= node["base"] <= node["top"] <= len(data):
                raise ValueError("invalid coroutine bounds")
            entries = []
            for _ in range(node["top"] - node["first"]):
                tag = reader.char()
                if tag == "P":
                    entry = tag, reader.integer(":"), reader.integer()
                elif tag == "L":
                    entry = tag, reader.integer(minimum=-9007199254740991)
                elif tag == "K":
                    entry = tag, reader.string()
                elif tag == "V":
                    entry = tag, reader.token()
                else:
                    raise ValueError("invalid coroutine entry")
                entries.append(entry)
            node["entries"] = entries
        else:
            raise ValueError(f"unknown graph node {kind!r}")
        nodes[index] = node
    if reader.pos != len(data):
        raise ValueError(f"trailing graph data at byte {reader.pos}")
    graph["nodes"] = nodes
    if set(graph_references(graph)) - nodes.keys():
        raise ValueError("graph references missing nodes")
    pending = list(graph_references({key: value for key, value in graph.items() if key != "nodes"}))
    reachable = set()
    while pending:
        index = pending.pop()
        if index not in reachable:
            reachable.add(index)
            pending.extend(graph_references(nodes[index]))
    if reachable != nodes.keys():
        raise ValueError("graph contains unreachable nodes")
    return graph


_ACTOR_CLASSES = {"Actor", "AHuman", "ACrab", "ACraft", "ACDropShip", "ACRocket", "ADoor"}


def native_projection(text):
    """Project only fields identified by the completed-tick peer audit, at their native owner."""
    output, ancestry, actors, projected = [], [], set(), {}
    for line in text.splitlines(keepends=True):
        match = re.match(r"^(\t*)([^=\r\n]+?)(\s*=\s*)([^\r\n]*)(\r?\n)?$", line)
        if not match:
            output.append(line)
            continue
        tabs, name, separator, value, newline = match.groups()
        name = name.strip()
        ancestry = ancestry[:len(tabs)]
        owner = ancestry[-1] if ancestry else (None, None)
        replacement, reason = value, None
        if name == "UniqueID" and owner[1] in _ACTOR_CLASSES:
            actors.add(value.strip().encode())
        if name == "LimbPathState" and owner[1] in _ACTOR_CLASSES:
            fields = value.split()
            if fields[:1] == ["LP2"]:
                if len(fields) < 47 or not fields[46].isdigit() or len(fields) != 47 + 2 * int(fields[46]):
                    raise ValueError("malformed LP2 limb checkpoint")
                for field in fields[1:]:
                    float(field)
                fields[38] = fields[41] = "LOCAL_REAL_CLOCK"
                replacement, reason = " ".join(fields), "limb real-clock anchors"
        elif owner[1] in ("AEmitter", "AEJetpack") and name in ("SpecialBehaviour_AvgImpulse", "SpecialBehaviour_AvgBurstImpulse"):
            replacement, reason = "LOCAL_AI_CACHE", "emitter AI impulse cache"
        elif owner[1] == "SoundSet" and name in ("SpecialBehaviour_CurrentSelectionIsSet", "SpecialBehaviour_CurrentSelectionIndex"):
            replacement, reason = "LOCAL_SOUND_SELECTION", "sound selection"
        elif name == "Frame" and owner == ("Flash", "Attachable") and len(ancestry) > 1 and ancestry[-2][1] == "HDFirearm":
            replacement, reason = "LOCAL_MUZZLE_FRAME", "muzzle flash frame"
        if reason:
            projected[reason] = projected.get(reason, 0) + 1
        output.append(tabs + match[2] + separator + replacement + (newline or ""))
        ancestry.append((name, value.strip()))
    return "".join(output), actors, projected


def local_ai_boundaries(graph, actor_uids):
    roots = {}
    for uid, token in graph["roots"].items():
        if uid not in actor_uids or token[:1] != ("#",):
            continue
        node = graph["nodes"][token[1]]
        if node["kind"] == "T":
            for key, value in node["pairs"]:
                if key == ("s", b"AI"):
                    roots[token[1]] = value

    def reachable(seeds, boundaries, private=False):
        pending, seen = list(graph_references(seeds)), set()
        while pending:
            index = pending.pop()
            if index in seen:
                continue
            node = graph["nodes"][index]
            # Native owners and stateless code are dependencies, not private AI storage.
            if private and (index in roots or (node["kind"] == "F" and not node.get("cells") and node.get("env") == ("z",))
                    or (node["kind"] == "U" and node.get("value", (None,))[0] in set("ewqAGSMpagxbhijkl"))):
                continue
            seen.add(index)
            if private:
                node = {key: value for key, value in node.items() if key != "meta"}
            if index in boundaries:
                node = dict(node, pairs=[pair for pair in node["pairs"] if pair[0] != ("s", b"AI")])
            pending.extend(graph_references(node))
        return seen

    seeds = {key: value for key, value in graph.items() if key != "nodes"}
    boundaries = dict(roots)
    while True:
        public = reachable(seeds, boundaries)
        promote = {index for index, value in boundaries.items() if reachable(value, {}, True) & public}
        if not promote:
            return boundaries
        for index in promote:
            del boundaries[index]


class GraphMismatch(ValueError):
    pass


def masked_timer_tokens(value):
    """A Timer rides the graph as m<StartSimTime,SimTimeLimit,StartRealTime,RealTimeLimit>; the real anchor is per-process."""
    if isinstance(value, tuple):
        if value[:1] == ("m",):
            sim_start, sim_limit, _, real_limit = value[1].split(",")
            return "m", ",".join((sim_start, sim_limit, "LOCAL", real_limit))
        return tuple(masked_timer_tokens(item) for item in value)
    if isinstance(value, list):
        return [masked_timer_tokens(item) for item in value]
    if isinstance(value, dict):
        return {key: masked_timer_tokens(item) for key, item in value.items()}
    return value


def compare_graphs(first, second, actor_uids=None, cross_process=False):
    """Require a bijection, including table keys, closures, upvalue cells and native aliases."""
    if cross_process:
        first, second = masked_timer_tokens(first), masked_timer_tokens(second)
    cuts = [local_ai_boundaries(graph, actor_uids) if actor_uids is not None else {} for graph in (first, second)]

    def mismatch(path, reason):
        raise GraphMismatch(f"{path}: {reason}")

    def bind(a, b, path, forward, reverse):
        if a in forward and forward[a] != b:
            mismatch(path, "one Lua identity split into two")
        if b in reverse and reverse[b] != a:
            mismatch(path, "two Lua identities merged into one")
        forward[a], reverse[b] = b, a

    def solve(work, deferred, forward, reverse, seen):
        while True:
            while work:
                a, b, path = work.pop()
                if type(a) is not type(b):
                    mismatch(path, "value kinds differ")
                if isinstance(a, tuple) and a[:1] == ("#",) and b[:1] == ("#",):
                    aid, bid = a[1], b[1]
                    bind(aid, bid, path, forward, reverse)
                    if (aid, bid) in seen:
                        continue
                    seen.add((aid, bid))
                    left, right = dict(first["nodes"][aid]), dict(second["nodes"][bid])
                    if (aid in cuts[0]) != (bid in cuts[1]):
                        mismatch(path, "AI state has a shared alias on only one peer")
                    if aid in cuts[0]:
                        av, bv = cuts[0][aid], cuts[1][bid]
                        if av[:1] != ("#",) or bv[:1] != ("#",):
                            mismatch(path, "local AI boundary is not a Lua object")
                        bind(av[1], bv[1], path + ".AI", forward, reverse)
                        left["pairs"] = [pair for pair in left["pairs"] if pair[0] != ("s", b"AI")]
                        right["pairs"] = [pair for pair in right["pairs"] if pair[0] != ("s", b"AI")]
                    for node in (left, right):
                        if "ini" in node:
                            if re.match(rb"\d+ [A-Za-z0-9_:]+ ", node["ini"]):
                                snapshot_runtime.decode(node["ini"])
                                continue
                            native = node["ini"].decode()
                            if actor_uids is not None:
                                native = native_projection(native)[0]
                            node["ini"] = runtime_projection(native, "", actor_uids is not None, cross_process)[0].encode()
                    work.append((left, right, path))
                elif isinstance(a, dict):
                    if a.keys() != b.keys():
                        mismatch(path, f"fields differ: {sorted(a.keys() ^ b.keys(), key=repr)!r}")
                    for key in sorted(a, key=repr, reverse=True):
                        if key == "pairs" and a.get("kind") == "T" and b.get("kind") == "T":
                            left, right = a[key], b[key]
                            scalars_a = {k: v for k, v in left if k[:1] != ("#",)}
                            scalars_b = {k: v for k, v in right if k[:1] != ("#",)}
                            if scalars_a.keys() != scalars_b.keys():
                                mismatch(path, f"table keys differ: {sorted(scalars_a.keys() ^ scalars_b.keys(), key=repr)!r}")
                            for entry in sorted(scalars_a, key=repr, reverse=True):
                                work.append((scalars_a[entry], scalars_b[entry], path + f"[{entry!r}]"))
                            refs_a = [pair for pair in left if pair[0][:1] == ("#",)]
                            refs_b = [pair for pair in right if pair[0][:1] == ("#",)]
                            if len(refs_a) != len(refs_b):
                                mismatch(path, "reference-key counts differ")
                            if refs_a:
                                deferred.append((refs_a, refs_b, path))
                        else:
                            work.append((a[key], b[key], path + f".{key!r}"))
                elif isinstance(a, (tuple, list)):
                    if len(a) != len(b):
                        mismatch(path, "sequence lengths differ")
                    work.extend((a[i], b[i], path + f"[{i}]") for i in reversed(range(len(a))))
                elif a != b:
                    mismatch(path, f"{repr(a)[:180]} != {repr(b)[:180]}")
            if not deferred:
                return forward, reverse, seen
            left, right, path = deferred.pop()
            # Resolve reference keys after named roots and scalar keys have constrained the map.
            left.sort(key=lambda pair: pair[0][1] not in forward)
            key, value = left[0]
            candidates = [i for i, (other, _) in enumerate(right)
                if (key[1] not in forward or forward[key[1]] == other[1])
                and (other[1] not in reverse or reverse[other[1]] == key[1])
                and first["nodes"][key[1]]["kind"] == second["nodes"][other[1]]["kind"]]
            if not candidates:
                mismatch(path, "no identity-preserving table-key match")
            failure = None
            for index in candidates:
                other, other_value = right[index]
                next_deferred = list(deferred)
                if len(left) > 1:
                    next_deferred.append((left[1:], right[:index] + right[index + 1:], path))
                next_work = [(value, other_value, path + ".reference_value"), (key, other, path + ".reference_key")]
                if len(candidates) == 1:
                    work, deferred = next_work, next_deferred
                    break
                try:
                    return solve(next_work, next_deferred, dict(forward), dict(reverse), set(seen))
                except GraphMismatch as error:
                    failure = error
            else:
                raise failure

    a = {key: value for key, value in first.items() if key != "nodes"}
    b = {key: value for key, value in second.items() if key != "nodes"}
    mapping, _, _ = solve([(a, b, "graph")], [], {}, {}, set())
    return {"matched_nodes": len(mapping), "local_ai_boundaries": len(cuts[0])}


def read_graph_blocks(blocks):
    graphs = {}
    for block in blocks:
        value = block.split("=", 1)[1].strip()
        match = re.fullmatch(r"(\d+)\|([A-Za-z0-9_\-.=]+)", value)
        if not match:
            raise ValueError("invalid indexed LuaStateGraph")
        index = int(match[1])
        if index in graphs:
            raise ValueError(f"duplicate Lua VM index {index}")
        data = base64.b64decode(match[2].replace(".", "="), altchars=b"-_", validate=True)
        graphs[index] = parse_graph(data)
    if graphs and set(graphs) != set(range(len(graphs))):
        raise ValueError("noncontiguous Lua VM indexes")
    return graphs


def split_top_level(text: str) -> dict:
    """Split an ini stream into top-level properties (lines with no leading tab start a block)."""
    blocks = {}
    key = None
    lines = []
    for line in text.splitlines(keepends=True):
        if line.strip() and not line.startswith(("\t", " ")):
            if key is not None:
                blocks.setdefault(key, []).append("".join(lines))
            key = line.split("=", 1)[0].strip()
            lines = [line]
        else:
            lines.append(line)
    if key is not None:
        blocks.setdefault(key, []).append("".join(lines))
    return blocks


def normalize_snapshot_name(text: str, name: str) -> str:
    lines, ancestry = [], []
    for line in text.splitlines(keepends=True):
        match = re.match(r"^(\t*)([^=\r\n]+?)([ \t]*=[ \t]*)([^\r\n]*)(\r?\n)?$", line)
        if match:
            depth = len(match[1])
            ancestry = ancestry[:depth]
            key, value = match[2].strip(), match[4].strip()
            if ancestry in (["Scene"], ["Scene", "Terrain"]):
                if key == "PresetName" and value == name:
                    line = match[1] + match[2] + match[3] + "SNAPSHOT" + (match[5] or "")
                elif key == "SpecialBehaviour_PresetName" and decode_base64(value) == name.encode():
                    line = match[1] + match[2] + match[3] + "U05BUFNIT1Q." + (match[5] or "")
            ancestry.append(key)
        lines.append(line)
    return "".join(lines)


def decode_base64(value):
    return base64.b64decode(value.replace(".", "="), altchars=b"-_", validate=True)


_SPRITE_CLASSES = _ACTOR_CLASSES | {"MOSprite", "MOSRotating", "MOSParticle", "Attachable", "AEmitter", "AEJetpack",
    "Arm", "Leg", "Turret", "HeldDevice", "HDFirearm", "ThrownDevice", "TDExplosive", "Magazine"}
_NATIVE_RUNTIME_OWNERS = {
    "MovableObject": _SPRITE_CLASSES | {"MovableObject", "MOPixel"},
    "Actor": _ACTOR_CLASSES, "AHuman": {"AHuman"}, "MOSprite": _SPRITE_CLASSES,
    "MOSRotating": _SPRITE_CLASSES - {"MOSprite", "MOSParticle"}, "HeldDevice": {"HeldDevice", "HDFirearm", "ThrownDevice", "TDExplosive"},
    "HDFirearm": {"HDFirearm"}, "AEmitter": {"AEmitter", "AEJetpack"}, "PieMenu": {"PieMenu"},
}


def inventory_reference_roles(text, state):
    """Map only the local controlled actor and its actual serialized descendants.

    UIDs elsewhere stay global identities. The ownership path distinguishes two equal
    devices and keeps repeated GUI references attached to the same world object.
    """
    while isinstance(state, dict) and state.get("version") in ("GameActivity1", "GameActivity2"):
        state = state["base" if state["version"] == "GameActivity1" else "values"]
    if not isinstance(state, dict) or state.get("version") != "Activity1":
        return {}
    local_actor = state["actor_links"][0][0]
    if not local_actor:
        return {}
    root = {"children": {}, "path": (), "actor": None}
    stack, objects = [root], []
    for line in text.splitlines():
        match = re.match(r"^(\t*)([^=]+?)\s*=\s*(.*)$", line)
        if not match:
            continue
        depth, key, value = len(match[1]), match[2].strip(), match[3].strip()
        stack = stack[:depth + 1]
        parent = stack[-1]
        position = parent["children"].get((key, value), 0)
        parent["children"][(key, value)] = position + 1
        node = {"children": {}, "path": (*parent["path"], (key, value, position)), "actor": parent["actor"]}
        if value in _ACTOR_CLASSES and (parent["path"] and parent["path"][0][0] == "Scene"):
            node["actor"] = node
        if key == "UniqueID" and node["actor"] is not None:
            parent["uid"] = int(value)
            objects.append(parent)
        stack.append(node)
    roles = {}
    for item in objects:
        owner = item["actor"]
        if owner.get("uid") == local_actor:
            roles[item["uid"]] = ("LOCAL_ACTOR", *item["path"][len(owner["path"]):])
    if local_actor not in roles:
        raise ValueError("the local controlled actor is missing from the snapshot world")
    return roles


def canonical_custom_values(text):
    """Compare unordered native key/value maps without dropping values or duplicate entries."""
    lines, output, index = text.splitlines(keepends=True), [], 0
    header = re.compile(r"^(\t*)AddCustomValue[ \t]*=[ \t]*(NumberValue|StringValue)(?:\r?\n)?$")
    while index < len(lines):
        first = header.fullmatch(lines[index])
        if first is None:
            output.append(lines[index])
            index += 1
            continue
        entries, identities, depth = [], set(), first[1]
        while index < len(lines):
            match = header.fullmatch(lines[index])
            if match is None or match[1] != depth:
                break
            if index + 1 == len(lines):
                raise ValueError("custom value is missing its key and value")
            value = re.fullmatch(re.escape(depth + "\t") + r"([^\t=\r\n]+?)[ \t]*=[ \t]*([^\r\n]*)(?:\r?\n)?", lines[index + 1])
            if value is None:
                raise ValueError("invalid native custom value entry")
            identity = match[2], value[1].strip()
            if not identity[1] or identity in identities:
                raise ValueError("empty or duplicate native custom value key")
            identities.add(identity)
            entries.append((identity, lines[index] + lines[index + 1]))
            index += 2
        output.extend(entry for _, entry in sorted(entries))
    return "".join(output)


def masked_limb_path_state(value):
    """LimbPath::PackTraversalState writes the two Timer real anchors at fixed offsets, before the trailing segments."""
    body = value.split()[1:]
    if not value.startswith("LP2 ") or len(body) < 46 or not body[45].lstrip("-").isdigit() or len(body) != 46 + 2 * int(body[45]):
        return value
    body[37] = body[40] = "LOCAL"
    return " ".join(("LP2", *body))


def runtime_payload(value):
    """The decoded bytes of a SpecialBehaviour_* value that really is a versioned CheckpointArchive payload."""
    try:
        raw = decode_base64(value)
    except Exception:
        return None
    return raw if re.match(rb"\d+ [A-Za-z0-9_]+ ", raw) else None


def runtime_projection(text, name, shared, cross_process=False):
    text = canonical_custom_values(text)
    output, ancestry, projected = [], [], []
    for line in text.splitlines(keepends=True):
        match = re.match(r"^(\t*)([^=\r\n]+?)([ \t]*=[ \t]*)([^\r\n]*)(\r?\n)?$", line)
        if match:
            tabs, key, separator, value, newline = match.groups()
            key = key.strip()
            ancestry = ancestry[:len(tabs)]
            owner = ancestry[-1] if ancestry else (None, None)
            root = not ancestry and key in ("RuntimeGlobals", "SceneRuntime")
            activity = len(ancestry) == 1 and ancestry[0][0] in ("Activity", "CheckpointStartActivity") and ancestry[0][1] in ("GameActivity", "GAScripted")
            controller = key == "SpecialBehaviour_ControllerCheckpoint" and owner[1] in _ACTOR_CLASSES
            native = any(key == f"SpecialBehaviour_{kind}Runtime" and owner[1] in owners for kind, owners in _NATIVE_RUNTIME_OWNERS.items())
            emission = key == "SpecialBehaviour_EmissionCheckpoint" and owner[1] == "Emission"
            sound = key == "SpecialBehaviour_SoundCheckpoint" and owner[1] == "SoundContainer"
            # Anchors also hide in payloads no other comparison decodes, so reach every one of them here.
            checkpoint = cross_process and key.startswith("SpecialBehaviour_") and runtime_payload(value.strip()) is not None
            if root or (key == "SpecialBehaviour_RuntimeCheckpoint" and activity) or controller or native or emission or sound or checkpoint:
                state = snapshot_runtime.decode(decode_base64(value.strip()))
                masks = []
                roles = inventory_reference_roles(text, state) if shared and activity else None
                state = snapshot_runtime.project(state, shared, name.encode(), masked=masks, local_roles=roles, cross_process=cross_process)
                if shared and key == "SpecialBehaviour_MOSpriteRuntime" and owner == ("Flash", "Attachable") and len(ancestry) > 1 and ancestry[-2][1] == "HDFirearm":
                    state["frame"] = "LOCAL"
                    masks.append(("frame",))
                line = tabs + match[2] + separator + repr(state) + (newline or "")
                projected.extend(".".join(map(str, (key, *path))) for path in masks)
            elif cross_process and key == "LimbPathState":
                line = tabs + match[2] + separator + masked_limb_path_state(value.strip()) + (newline or "")
            ancestry.append((key, value.strip()))
        output.append(line)
    return "".join(output), projected


# Each peer runs as PlayerOne on its own team, so the local player's slot bindings legitimately
# differ per peer. Everything else in the Activity block (ActivityState, team funds, generic saved
# values, CPU team, delivery delay, techs, ...) is sim state and MUST match across peers.
_ACTIVITY_LOCAL_VIEW = {
    "TeamOfPlayer1",
    "FundsContributionOfPlayer1",
    "TeamFundsShareOfPlayer1",
    "Player1IsHuman",
}


def split_activity_props(block: str) -> dict:
    """Split an Activity block into its depth-1 properties (one leading tab). Values are lists to
    tolerate a repeated key."""
    props: dict = {}
    key = None
    lines: list = []
    for line in block.splitlines(keepends=True):
        if line.startswith("\t") and not line.startswith("\t\t"):
            if key is not None:
                props.setdefault(key, []).append("".join(lines))
            key = line[1:].split("=", 1)[0].strip()
            lines = [line]
        elif key is not None:
            lines.append(line)
    if key is not None:
        props.setdefault(key, []).append("".join(lines))
    return props


def compare_main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("snapshot_a")
    parser.add_argument("snapshot_b")
    parser.add_argument("--full", action="store_true", help="require the complete per-peer checkpoint, including local AI and presentation")
    parser.add_argument("--report", type=Path, help="write machine-readable comparison evidence")
    args = parser.parse_args()

    # The save bakes its OWN file name into the scene/terrain PresetNames — metadata, not sim
    # state. Normalize both to a fixed token before comparing.
    import os

    base_a = os.path.splitext(os.path.basename(args.snapshot_a))[0]
    base_b = os.path.splitext(os.path.basename(args.snapshot_b))[0]

    with zipfile.ZipFile(args.snapshot_a) as za, zipfile.ZipFile(args.snapshot_b) as zb:
        names_a = sorted(za.namelist())
        names_b = sorted(zb.namelist())
        if len(names_a) != len(set(names_a)) or len(names_b) != len(set(names_b)):
            print("FAIL: duplicate archive entries")
            return 1
        if "Save.ini" not in names_a or "Save.ini" not in names_b:
            print("FAIL: Save.ini is missing")
            return 1
        if names_a != names_b:
            print(f"FAIL: entry lists differ: {names_a} vs {names_b}")
            return 1

        failures = []
        details = {"mode": "full" if args.full else "shared", "graphs": {}, "projection": {}}
        activity_diff_lines = 0
        for name in names_a:
            data_a = za.read(name)
            data_b = zb.read(name)
            if name == "Save.ini":
                text_a = normalize_snapshot_name(data_a.decode("utf-8"), base_a)
                text_b = normalize_snapshot_name(data_b.decode("utf-8"), base_b)
                text_a, runtime_a = runtime_projection(text_a, base_a, not args.full)
                text_b, runtime_b = runtime_projection(text_b, base_b, not args.full)
                details["runtime_projection"] = {"a": runtime_a, "b": runtime_b}
                blocks_a = split_top_level(text_a)
                blocks_b = split_top_level(text_b)
                actor_uids = set()
                if not args.full:
                    for side, blocks in (("a", blocks_a), ("b", blocks_b)):
                        projected = [native_projection(block) for block in blocks.get("Scene", [])]
                        blocks["Scene"] = [item[0] for item in projected]
                        details["projection"][side] = [item[2] for item in projected]
                        uids = set().union(*(item[1] for item in projected))
                        if side == "a":
                            actor_uids = uids
                        elif uids != actor_uids:
                            failures.append("Scene actor identities differ")
                for side, blocks in (("a", blocks_a), ("b", blocks_b)):
                    for required in ("Scene", "Activity"):
                        if len(blocks.get(required, [])) != 1:
                            failures.append(f"Save.ini {side} needs exactly one '{required}' block")
                    if "HasCheckpointStartActivity" in blocks or "CheckpointStartActivity" in blocks:
                        declaration = blocks.get("HasCheckpointStartActivity", [])
                        present = re.fullmatch(r"HasCheckpointStartActivity\s*=\s*([01])\s*", declaration[0]) if len(declaration) == 1 else None
                        if present is None or len(blocks.get("CheckpointStartActivity", [])) != int(present[1]):
                            failures.append(f"Save.ini {side} has an inconsistent configured start activity")
                if set(blocks_a) != set(blocks_b):
                    failures.append(
                        f"Save.ini top-level properties differ: {sorted(blocks_a)} vs {sorted(blocks_b)}"
                    )
                    continue
                for key in blocks_a:
                    if key == "LuaStateGraph":
                        graphs_a, graphs_b = read_graph_blocks(blocks_a[key]), read_graph_blocks(blocks_b[key])
                        if graphs_a.keys() != graphs_b.keys():
                            failures.append("Lua VM indexes differ")
                        for index in sorted(graphs_a.keys() & graphs_b.keys()):
                            try:
                                details["graphs"][str(index)] = compare_graphs(graphs_a[index], graphs_b[index], None if args.full else actor_uids)
                            except GraphMismatch as error:
                                failures.append(f"Lua VM {index}: {error}")
                        continue
                    if blocks_a[key] == blocks_b[key]:
                        continue
                    if key in ("Activity", "CheckpointStartActivity"):
                        if len(blocks_a[key]) != 1 or len(blocks_b[key]) != 1:
                            continue
                        if blocks_a[key][0].splitlines()[0] != blocks_b[key][0].splitlines()[0]:
                            failures.append(f"{key} class differs")
                        # Compare every Activity property; only the local-player view fields may differ.
                        props_a = split_activity_props(blocks_a[key][0])
                        props_b = split_activity_props(blocks_b[key][0])
                        for prop in sorted(set(props_a) | set(props_b)):
                            if props_a.get(prop, []) == props_b.get(prop, []):
                                continue
                            if prop in _ACTIVITY_LOCAL_VIEW and not args.full and prop in props_a and prop in props_b and len(props_a[prop]) == len(props_b[prop]):
                                activity_diff_lines += 1
                            else:
                                failures.append(
                                    f"Save.ini {key} property '{prop}' differs across peers (sim state)"
                                )
                    else:
                        failures.append(f"Save.ini '{key}' block differs ({details['mode']} state)")
            elif data_a != data_b:
                ha = hashlib.sha256(data_a).hexdigest()[:12]
                hb = hashlib.sha256(data_b).hexdigest()[:12]
                failures.append(
                    f"{name} differs ({len(data_a)}B {ha} vs {len(data_b)}B {hb})"
                )

        details.update(passed=not failures, failures=failures, activity_local_differences=activity_diff_lines,
            snapshots=[{"path": str(Path(path).resolve()), "sha256": hashlib.sha256(Path(path).read_bytes()).hexdigest()} for path in (args.snapshot_a, args.snapshot_b)])
        if args.report:
            args.report.write_text(json.dumps(details, indent=2))
        if failures:
            for failure in failures:
                print(f"FAIL: {failure}")
            return 1
        print(
            f"PASS: {details['mode']} snapshot state matches "
            f"({len(names_a)} entries; {len(details['graphs'])} Lua VMs; activity local differences: {activity_diff_lines})"
        )
        return 0


def main() -> int:
    try:
        return compare_main()
    except (ValueError, OSError, zipfile.BadZipFile) as error:
        print(f"FAIL: {error}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
