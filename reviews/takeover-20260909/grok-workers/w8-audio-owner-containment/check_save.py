"""Parse one .ccsave / Save.ini and report which AudioRuntime voice owners the save carries."""
from __future__ import annotations

import argparse
import base64
import json
import re
import sys
import zipfile
from pathlib import Path

OUT = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w8-audio-owner-containment")


class CheckpointError(Exception):
    pass


class CheckpointReader:
    def __init__(self, text: str):
        self.text = text
        self.i = 0

    def remaining(self) -> str:
        return self.text[self.i :]

    def _need(self) -> None:
        if self.i >= len(self.text):
            raise CheckpointError("truncated runtime checkpoint")

    def read_int(self) -> int:
        self._need()
        end = self.text.find(" ", self.i)
        if end < 0 or end == self.i:
            raise CheckpointError("truncated runtime checkpoint number")
        token = self.text[self.i : end]
        try:
            value = int(token, 10)
        except ValueError as exc:
            raise CheckpointError(f"invalid runtime checkpoint integer '{token}'") from exc
        self.i = end + 1
        return value

    def read_uint(self) -> int:
        value = self.read_int()
        if value < 0:
            raise CheckpointError(f"negative unsigned {value}")
        return value

    def read_bool(self) -> bool:
        return bool(self.read_int())

    def read_string(self) -> str:
        size = self.read_uint()
        if self.i + size >= len(self.text) or self.text[self.i + size] != " ":
            raise CheckpointError("truncated runtime checkpoint string")
        value = self.text[self.i : self.i + size]
        self.i += size + 1
        return value

    def read_float_bits(self) -> int:
        return self.read_uint()

    def expect_version(self, *allowed: str) -> str:
        found = self.read_string()
        if found not in allowed:
            raise CheckpointError(f"unsupported version {found!r}, expected {allowed}")
        return found

    def finish(self) -> None:
        if self.i < len(self.text):
            raise CheckpointError(f"trailing runtime checkpoint data ({len(self.text) - self.i} bytes)")


def decode_b64(text: str) -> bytes:
    raw = "".join(text.split())
    if not raw:
        return b""
    # cpp-base64 url=true: '-' '_' alphabet and '.' padding (ActivityMan.cpp base64_encode(..., true)).
    raw = raw.replace("-", "+").replace("_", "/").replace(".", "=")
    pad = (-len(raw)) % 4
    raw += "=" * pad
    return base64.b64decode(raw)


def decode_b64_text(text: str) -> str:
    # Checkpoint text is a byte string (CheckpointArchive.h); latin-1 keeps 1:1 length.
    return decode_b64(text).decode("latin-1")


def parse_entity(text: str) -> dict:
    r = CheckpointReader(text)
    r.expect_version("Entity1")
    preset = r.read_string()
    copied = r.read_string()
    desc = r.read_string()
    pos = r.read_string()
    original = r.read_bool()
    module = r.read_int()
    weight = r.read_int()
    n = r.read_uint()
    groups = [r.read_string() for _ in range(n)]
    r.finish()
    return {
        "preset": preset,
        "copied_from": copied,
        "description": desc,
        "reader_position": pos,
        "original": original,
        "module": module,
        "weight": weight,
        "groups": groups,
    }


def parse_sound_container_identity(text: str) -> dict:
    r = CheckpointReader(text)
    version = r.expect_version("SoundContainer3", "SoundContainer2", "SoundContainer1")
    entity_text = r.read_string()
    identity = r.read_uint()
    entity = None
    try:
        entity = parse_entity(entity_text)
    except CheckpointError:
        entity = {"preset": None, "copied_from": None, "parse_error": True}
    return {"identity": identity, "version": version, "entity": entity, "raw_head": text[:80]}


def collect_sound_container_identities(text: str, source: str, carriers: dict, clues: dict) -> None:
    if not text:
        return
    idx = 0
    while True:
        pos = text.find("SoundContainer", idx)
        if pos < 0:
            break
        # Archive starts as `<len> SoundContainerN ...`. Walk back to the length token.
        start = pos
        while start > 0 and text[start - 1] != " ":
            start -= 1
        # The length token sits before that space.
        tok_end = start - 1 if start > 0 and text[start - 1] == " " else start
        tok_beg = tok_end
        while tok_beg > 0 and text[tok_beg - 1].isdigit():
            tok_beg -= 1
        try:
            parsed = parse_sound_container_identity(text[tok_beg:])
        except CheckpointError:
            idx = pos + 1
            continue
        ident = parsed["identity"]
        carriers.setdefault(ident, set()).add(source)
        entity = parsed.get("entity") or {}
        clue = []
        if entity.get("preset"):
            clue.append(f"preset={entity['preset']}")
        if entity.get("copied_from"):
            clue.append(f"copied_from={entity['copied_from']}")
        if clue:
            clues.setdefault(ident, [])
            item = f"{source}: " + ", ".join(clue)
            if item not in clues[ident]:
                clues[ident].append(item)
        idx = pos + 1


def parse_voice(text: str) -> dict:
    r = CheckpointReader(text)
    r.expect_version("AudioVoice1")
    identity = r.read_int()
    owner = r.read_uint()
    path = r.read_string()
    playing = r.read_bool()
    return {"voice_identity": identity, "owner": owner, "path": path, "playing": playing}


def skip_timer(r: CheckpointReader) -> None:
    r.read_int()
    r.read_int()
    r.read_int()
    r.read_int()


def parse_audio_runtime(text: str) -> dict:
    r = CheckpointReader(text)
    version = r.expect_version("AudioRuntime3", "AudioRuntime2", "AudioRuntime1")
    enabled = r.read_bool()
    next_voice = r.read_int()
    next_sound = r.read_uint()
    for _ in range(4):
        r.read_bool()
    for _ in range(7):
        r.read_float_bits()
    r.read_bool()
    r.read_bool()
    npos = r.read_uint()
    for _ in range(npos):
        r.read_float_bits()
        r.read_float_bits()
    nlisteners = r.read_uint()
    for _ in range(nlisteners):
        for _j in range(4 * 3):
            r.read_float_bits()
    for _ in range(4):
        r.read_string()
    nsamples = r.read_uint()
    samples = []
    for _ in range(nsamples):
        sample_text = r.read_string()
        samples.append(sample_text)
    voice_count_at = r.i
    nvoices = r.read_uint()
    voices = []
    for _ in range(nvoices):
        voice_text = r.read_string()
        voices.append(parse_voice(voice_text))
    ndist = r.read_uint()
    for _ in range(ndist):
        r.read_int()
        r.read_float_bits()
    for _ in range(4):
        nevents = r.read_uint()
        for _e in range(nevents):
            r.read_string()
    if version == "AudioRuntime3":
        naud = r.read_uint()
        for _ in range(naud):
            r.read_string()
        r.read_uint()
        r.read_uint()
    elif version == "AudioRuntime2":
        naud = r.read_uint()
        for _ in range(naud):
            r.read_string()
    leftover = len(r.remaining())
    voice_tag_count = text.count("AudioVoice1")
    return {
        "version": version,
        "enabled": enabled,
        "next_voice": next_voice,
        "next_sound_container": next_sound,
        "sample_count": nsamples,
        "voice_count_token": nvoices,
        "voice_count_parsed": len(voices),
        "audio_voice1_tag_count": voice_tag_count,
        "voices": voices,
        "leftover_bytes": leftover,
        "voice_count_offset": voice_count_at,
    }


def parse_gui_identities(text: str, carriers: dict, clues: dict) -> int:
    r = CheckpointReader(text)
    r.expect_version("GUISound1")
    count = 0
    for _ in range(27):
        sound_text = r.read_string()
        sr = CheckpointReader(sound_text)
        sr.expect_version("GUISoundContainer1")
        native = sr.read_string()
        parsed = parse_sound_container_identity(native)
        ident = parsed["identity"]
        carriers.setdefault(ident, set()).add("gui")
        entity = parsed.get("entity") or {}
        clue = []
        if entity.get("preset"):
            clue.append(f"preset={entity['preset']}")
        if entity.get("copied_from"):
            clue.append(f"copied_from={entity['copied_from']}")
        if clue:
            clues.setdefault(ident, [])
            item = "gui: " + ", ".join(clue)
            if item not in clues[ident]:
                clues[ident].append(item)
        count += 1
    return count


def walk_music_sound(text: str, carriers: dict, clues: dict) -> None:
    if not text:
        return
    r = CheckpointReader(text)
    r.expect_version("MusicSound1")
    native = r.read_string()
    parsed = parse_sound_container_identity(native)
    ident = parsed["identity"]
    carriers.setdefault(ident, set()).add("music")
    entity = parsed.get("entity") or {}
    clue = []
    if entity.get("preset"):
        clue.append(f"preset={entity['preset']}")
    if entity.get("copied_from"):
        clue.append(f"copied_from={entity['copied_from']}")
    if clue:
        clues.setdefault(ident, [])
        item = "music: " + ", ".join(clue)
        if item not in clues[ident]:
            clues[ident].append(item)


def walk_music_section(text: str, carriers: dict, clues: dict) -> None:
    r = CheckpointReader(text)
    r.expect_version("MusicSection1")
    r.read_string()
    ntrans = r.read_uint()
    for _ in range(ntrans):
        walk_music_sound(r.read_string(), carriers, clues)
    r.read_uint()
    nq = r.read_uint()
    for _ in range(nq):
        r.read_uint()
    nsounds = r.read_uint()
    for _ in range(nsounds):
        walk_music_sound(r.read_string(), carriers, clues)


def walk_music_song(text: str, carriers: dict, clues: dict) -> None:
    r = CheckpointReader(text)
    r.expect_version("MusicSong1")
    r.read_string()
    fallback = r.read_string()
    walk_music_section(fallback, carriers, clues)
    nsec = r.read_uint()
    for _ in range(nsec):
        walk_music_section(r.read_string(), carriers, clues)


def parse_music_identities(text: str, carriers: dict, clues: dict) -> None:
    r = CheckpointReader(text)
    r.expect_version("MusicMan1")
    r.read_bool()
    interrupting = r.read_string()
    song = r.read_string()
    r.read_string()
    r.read_string()
    r.read_int()
    previous = r.read_string()
    current = r.read_string()
    walk_music_sound(interrupting, carriers, clues)
    if song:
        walk_music_song(song, carriers, clues)
    walk_music_sound(previous, carriers, clues)
    walk_music_sound(current, carriers, clues)


def find_archive(text: str, version: str) -> str | None:
    needle = f"{len(version)} {version} "
    pos = text.find(needle)
    if pos < 0:
        return None
    i = pos
    while i > 0 and text[i - 1] == " ":
        i -= 1
    beg = i
    while beg > 0 and text[beg - 1].isdigit():
        beg -= 1
    if beg < i and text[beg:i].isdigit():
        try:
            return CheckpointReader(text[beg:]).read_string()
        except CheckpointError:
            pass
    return text[pos:]


def parse_runtime_globals(text: str) -> dict:
    r = CheckpointReader(text)
    version = r.expect_version(
        "RuntimeGlobals9",
        "RuntimeGlobals8",
        "RuntimeGlobals7",
        "RuntimeGlobals6",
        "RuntimeGlobals5",
        "RuntimeGlobals4",
        "RuntimeGlobals3",
        "RuntimeGlobals2",
        "RuntimeGlobals1",
    )
    r.read_string()
    r.read_string()
    r.read_string()
    r.read_string()
    r.read_string()
    r.read_string()
    r.read_string()
    r.read_string()
    r.read_string()
    for _ in range(6):
        r.read_bool()
    r.read_string()
    r.read_string()
    r.read_string()
    r.read_string()
    gui = r.read_string()
    music = r.read_string()
    audio = r.read_string()
    return {"version": version, "gui": gui, "music": music, "audio": audio}


TOP_PROP = re.compile(r"(?m)^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$")
SOUND_CKPT = re.compile(r"SpecialBehaviour_SoundCheckpoint\s*=\s*(\S+)")
COPYOF = re.compile(r"CopyOf\s*=\s*(.+)$", re.M)
PRESET = re.compile(r"PresetName\s*=\s*(.+)$", re.M)
ADDSOUND = re.compile(r"AddSound\s*=\s*(.+)$", re.M)
SOUND_FILE = re.compile(r"(?i)(?:Data/)?[\w./\\-]+\.(?:flac|wav|ogg)")
OWNER_PROP = re.compile(
    r"(?m)^\s*(SpecialBehaviour_\w*Sound|\w+Sound|EmissionSound|BurstSound|EndSound|GibSound|"
    r"HatchOpenSound|HatchCloseSound|CrashSound|DoorMoveStartSound|DoorMoveSound|"
    r"StrideSound|FireSound|ActiveSound|ReloadSound)\s*=\s*(\S+)"
)


def nearby_ini_clues(ini: str, match_start: int, window: int = 2500) -> list[str]:
    start = max(0, match_start - window)
    chunk = ini[start:match_start]
    clues = []
    for rx, label in ((COPYOF, "CopyOf"), (PRESET, "PresetName")):
        hits = rx.findall(chunk)
        for hit in hits[-4:]:
            clues.append(f"{label}={hit.strip()}")
    for hit in OWNER_PROP.findall(chunk)[-6:]:
        clues.append(f"prop={hit[0]}={hit[1]}")
    for hit in SOUND_FILE.findall(chunk)[-6:]:
        clues.append(f"file={hit}")
    return clues


def extract_ini_props(ini: str) -> dict:
    props = {"RuntimeGlobals": [], "LuaStateGraph": [], "SoundCheckpoints": []}
    for m in TOP_PROP.finditer(ini):
        name, value = m.group(1), m.group(2).strip()
        if name in ("RuntimeGlobals", "LuaStateGraph"):
            props[name].append(value)
    for m in SOUND_CKPT.finditer(ini):
        props["SoundCheckpoints"].append((m.start(), m.group(1)))
    return props


def load_save_ini(path: Path) -> tuple[str, str]:
    if path.suffix.lower() == ".ccsave":
        with zipfile.ZipFile(path, "r") as zf:
            names = zf.namelist()
            if "Save.ini" not in names:
                raise FileNotFoundError(f"Save.ini missing in {path}; entries={names[:20]}")
            data = zf.read("Save.ini")
        return data.decode("latin-1"), "zip:Save.ini"
    text = path.read_text(encoding="latin-1")
    return text, "file"


def analyze(path: Path) -> dict:
    ini, source = load_save_ini(path)
    props = extract_ini_props(ini)
    carriers: dict[int, set[str]] = {}
    clues: dict[int, list[str]] = {}
    ini_identities = []
    for start, b64 in props["SoundCheckpoints"]:
        try:
            decoded = decode_b64_text(b64)
            parsed = parse_sound_container_identity(decoded)
            ident = parsed["identity"]
            carriers.setdefault(ident, set()).add("ini")
            ini_identities.append(ident)
            near = nearby_ini_clues(ini, start)
            entity = parsed.get("entity") or {}
            extra = []
            if entity.get("preset"):
                extra.append(f"preset={entity['preset']}")
            if entity.get("copied_from"):
                extra.append(f"copied_from={entity['copied_from']}")
            bag = clues.setdefault(ident, [])
            for item in extra + near:
                if item not in bag:
                    bag.append(item)
        except Exception as exc:
            clues.setdefault(-1, []).append(f"ini_checkpoint_error:{exc}")
    graph_notes = []
    for i, entry in enumerate(props["LuaStateGraph"]):
        bar = entry.find("|")
        payload = entry[bar + 1 :] if bar >= 0 else entry
        try:
            graph = decode_b64_text(payload)
        except Exception as exc:
            graph_notes.append(f"graph[{i}] decode error: {exc}")
            continue
        before = set(carriers)
        collect_sound_container_identities(graph, "script", carriers, clues)
        for m in SOUND_CKPT.finditer(graph):
            try:
                parsed = parse_sound_container_identity(decode_b64_text(m.group(1)))
                ident = parsed["identity"]
                carriers.setdefault(ident, set()).add("script")
                entity = parsed.get("entity") or {}
                bag = clues.setdefault(ident, [])
                for key in ("preset", "copied_from"):
                    if entity.get(key):
                        item = f"script: {key}={entity[key]}"
                        if item not in bag:
                            bag.append(item)
            except Exception:
                pass
        added = set(carriers) - before
        copy_hits = re.findall(r"(copy|preset)[\s\"',:=]+SoundContainer|SoundContainer[\s\"',:=]+(copy|preset)", graph)
        name_hits = re.findall(r"SoundContainer.{0,80}", graph)
        graph_notes.append(
            {
                "index": i,
                "bytes": len(graph),
                "identities_added": sorted(added),
                "copy_preset_hits": len(copy_hits),
                "soundcontainer_snippets": name_hits[:8],
            }
        )
    audio = None
    gui_count = 0
    globals_version = None
    parse_errors = []
    globals_text = ""
    if not props["RuntimeGlobals"]:
        parse_errors.append("no RuntimeGlobals property")
    else:
        try:
            globals_text = decode_b64_text(props["RuntimeGlobals"][0])
            gr = CheckpointReader(globals_text)
            globals_version = gr.read_string()
        except Exception as exc:
            parse_errors.append(f"runtime globals header: {exc}")
            globals_text = ""
        if globals_text:
            gui_blob = find_archive(globals_text, "GUISound1")
            music_blob = find_archive(globals_text, "MusicMan1")
            audio_blob = None
            for ver in ("AudioRuntime3", "AudioRuntime2", "AudioRuntime1"):
                audio_blob = find_archive(globals_text, ver)
                if audio_blob:
                    break
            if gui_blob:
                try:
                    gui_count = parse_gui_identities(gui_blob, carriers, clues)
                except Exception as exc:
                    parse_errors.append(f"gui parse: {exc}")
            if music_blob:
                try:
                    parse_music_identities(music_blob, carriers, clues)
                except Exception as exc:
                    parse_errors.append(f"music parse: {exc}")
            if audio_blob:
                try:
                    audio = parse_audio_runtime(audio_blob)
                except Exception as exc:
                    parse_errors.append(f"audio parse: {exc}")
            elif globals_version in (
                "RuntimeGlobals1",
                "RuntimeGlobals2",
                "RuntimeGlobals3",
                "RuntimeGlobals4",
            ):
                pass
            elif globals_version:
                parse_errors.append(f"{globals_version} has no AudioRuntime tag")
    voices_out = []
    missing = []
    if audio:
        for voice in audio["voices"]:
            owner = voice["owner"]
            carried = sorted(carriers.get(owner, set())) if owner else []
            status = ",".join(carried) if carried else ("none" if owner == 0 else "MISSING")
            row = {
                "voice_identity": voice["voice_identity"],
                "owner": owner,
                "playing": voice["playing"],
                "path": voice["path"],
                "carried_by": status,
                "carrier_clues": clues.get(owner, []),
            }
            voices_out.append(row)
            if status == "MISSING":
                missing.append(row)
    result = {
        "path": str(path),
        "ini_source": source,
        "ini_bytes": len(ini),
        "runtime_globals_version": globals_version,
        "audio": None
        if audio is None
        else {
            "version": audio["version"],
            "enabled": audio["enabled"],
            "next_voice": audio["next_voice"],
            "next_sound_container": audio["next_sound_container"],
            "sample_count": audio["sample_count"],
            "voice_count_token": audio["voice_count_token"],
            "voice_count_parsed": audio["voice_count_parsed"],
            "audio_voice1_tag_count": audio["audio_voice1_tag_count"],
            "leftover_bytes": audio["leftover_bytes"],
        },
        "ini_sound_checkpoint_count": len(props["SoundCheckpoints"]),
        "ini_identities": sorted(set(ini_identities)),
        "gui_sound_count": gui_count,
        "carried_identity_count": len(carriers),
        "graph_notes": graph_notes,
        "parse_errors": parse_errors,
        "voices": voices_out,
        "missing": missing,
        "validation": {
            "voice_count_matches_token": (
                audio is not None and audio["voice_count_token"] == audio["voice_count_parsed"]
            ),
            "voice_count_matches_audiovoice1_tags": (
                audio is not None and audio["voice_count_token"] == audio["audio_voice1_tag_count"]
            ),
            "audio_leftover_empty": audio is not None and audio["leftover_bytes"] == 0,
        },
    }
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("save")
    parser.add_argument("--json-out", default="")
    args = parser.parse_args(argv)
    path = Path(args.save)
    result = analyze(path)
    text_lines = [
        f"path={result['path']}",
        f"ini_bytes={result['ini_bytes']} source={result['ini_source']}",
        f"audio={result['audio']}",
        f"ini_sound_checkpoints={result['ini_sound_checkpoint_count']}",
        f"gui_sounds={result['gui_sound_count']}",
        f"carried={result['carried_identity_count']}",
        f"validation={result['validation']}",
        f"parse_errors={result['parse_errors']}",
        f"voices={len(result['voices'])} missing={len(result['missing'])}",
    ]
    for row in result["voices"]:
        text_lines.append(
            f"  voice {row['voice_identity']} owner={row['owner']} playing={int(row['playing'])} "
            f"carried_by={row['carried_by']} path={row['path']!r} clues={row['carrier_clues'][:6]}"
        )
    report = "\n".join(text_lines) + "\n"
    print(report, end="")
    if args.json_out:
        out = Path(args.json_out)
        out.parent.mkdir(parents=True, exist_ok=True)
        serial = json.loads(json.dumps(result, default=lambda o: sorted(o) if isinstance(o, set) else o))
        out.write_text(json.dumps(serial, indent=2), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
