"""The lobby envelope's constants, read from the tree under test so a driver cannot encode a stale version."""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re

LOBBY_HEADER = "Source/Network/NetLobbyProtocol.h"
CONFIG_HEADER = "Source/Network/NetMatchConfig.h"
# An integer literal with its C++ suffix; anything else (an expression, another constant) is not a wire value.
LITERAL = r"(0[xX][0-9a-fA-F]+|\d+)[uUlL]*"


@dataclass(frozen=True)
class Constant:
    value: int
    site: str


def _scope(text, path, opener, what):
    """The braced body of one class or enum, so two headers' identically named constants never cross."""
    match = re.search(opener, text)
    if not match:
        raise RuntimeError(f"{path}: {what} not found")
    start, depth = text.rindex("{", match.start(), match.end()), 0
    for index in range(start, len(text)):
        depth += (text[index] == "{") - (text[index] == "}")
        if depth == 0:
            return start, text[start:index]
    raise RuntimeError(f"{path}: {what} is never closed")


def _one(text, path, start, body, pattern, what):
    found = list(re.finditer(pattern, body))
    if len(found) != 1:
        raise RuntimeError(f"{path}: expected exactly one integer definition of {what}, found {len(found)}")
    return Constant(int(found[0].group(1), 0), f"{path}:{text.count(chr(10), 0, start + found[0].start()) + 1}")


def _constant(text, path, start, body, owner, name):
    pattern = rf"(?m)^[ \t]*static\s+constexpr\s+\w+\s+{name}\s*=\s*{LITERAL}\s*;"
    return _one(text, path, start, body, pattern, f"{owner}::{name}")


def _enumerator(text, path, start, body, owner, name):
    return _one(text, path, start, body, rf"(?m)^[ \t]*{name}\s*=\s*{LITERAL}\s*,?[ \t]*$", f"{owner}::{name}")


@dataclass(frozen=True)
class LobbyWire:
    magic: Constant
    version: Constant
    header_bytes: Constant
    config_version: Constant
    match_config_type: Constant

    def describe(self):
        return (f"lobby protocol version {self.version.value} ({self.version.site})"
                f" and match config version {self.config_version.value} ({self.config_version.site})")

    def as_json(self):
        return {name: {"value": getattr(self, name).value, "site": getattr(self, name).site}
                for name in ("magic", "version", "header_bytes", "config_version", "match_config_type")}


def read(repo):
    """Every constant a launch config carries, taken from the sources the run is about to launch."""
    lobby_path, config_path = Path(repo) / LOBBY_HEADER, Path(repo) / CONFIG_HEADER
    lobby, config = lobby_path.read_text(encoding="utf-8"), config_path.read_text(encoding="utf-8")
    lobby_start, lobby_body = _scope(lobby, LOBBY_HEADER, r"(?m)^[ \t]*class\s+NetLobbyProtocol\b[^;{]*\{", "class NetLobbyProtocol")
    type_start, type_body = _scope(lobby, LOBBY_HEADER, r"(?m)^[ \t]*enum\s+class\s+NetLobbyMessageType\b[^;{]*\{", "enum NetLobbyMessageType")
    config_start, config_body = _scope(config, CONFIG_HEADER, r"(?m)^[ \t]*class\s+NetMatchConfigUtil\b[^;{]*\{", "class NetMatchConfigUtil")
    return LobbyWire(
        magic=_constant(lobby, LOBBY_HEADER, lobby_start, lobby_body, "NetLobbyProtocol", "c_Magic"),
        version=_constant(lobby, LOBBY_HEADER, lobby_start, lobby_body, "NetLobbyProtocol", "c_Version"),
        header_bytes=_constant(lobby, LOBBY_HEADER, lobby_start, lobby_body, "NetLobbyProtocol", "c_HeaderBytes"),
        config_version=_constant(config, CONFIG_HEADER, config_start, config_body, "NetMatchConfigUtil", "c_Version"),
        match_config_type=_enumerator(lobby, LOBBY_HEADER, type_start, type_body, "NetLobbyMessageType", "MatchConfig"))
