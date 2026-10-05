"""The facts about the machines the multi-machine tools drive, read from one file outside every work tree.

The file is the one CORTEX_BOXES names, else ~/.cortex-modern/boxes.json. It lives outside the repository, so it is
never committed and no ignore list has to name it. tools/boxes.example.json shows its shape with neutral names and
documentation addresses; the unit tests run against that example (use_example()).

Each box has a role, which is what the tools ask for:
    pc        the Windows machine the coordinating tools run on
    remote    a second Windows machine on another network, reached over ssh, engines through one session task
    mac       the macOS machine (arm64)
    linux     the Linux machine
    laptop    a Windows laptop on the local network, present only some of the time
    handheld  a Windows handheld on the local network, one engine at a time
The values (names, aliases, trees, scratch roots, markers, limits) are whatever the file says; receipts and reports use
each box's `name` and `instance` exactly as written there.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any

ENV = 'CORTEX_BOXES'
DEFAULT = Path.home() / '.cortex-modern' / 'boxes.json'
EXAMPLE = Path(__file__).resolve().parent / 'boxes.example.json'
ROLES = ('pc', 'remote', 'mac', 'linux', 'laptop', 'handheld')


class BoxFileMissing(RuntimeError):
    pass


class Facts(dict):
    """A JSON object with attribute access: facts.box('remote').trees.acceptance."""

    def __getattr__(self, key: str) -> Any:
        try:
            return self[key]
        except KeyError:
            raise AttributeError(key) from None

    def __getitem__(self, key: str) -> Any:
        return wrap(dict.__getitem__(self, key))

    def get(self, key: str, default: Any = None) -> Any:
        return wrap(dict.get(self, key, default))


def wrap(value: Any) -> Any:
    if isinstance(value, dict) and not isinstance(value, Facts):
        return Facts(value)
    if isinstance(value, list):
        return [wrap(item) for item in value]
    return value


_cache: dict[str, Facts] = {}


def file() -> Path:
    """The box file in use: CORTEX_BOXES, else the default under the home directory."""
    return Path(os.environ.get(ENV) or DEFAULT)


def present() -> bool:
    return file().is_file()


def load(path: Path | str | None = None) -> Facts:
    """The parsed box file (cached per path); raises BoxFileMissing with the way to make one."""
    target = Path(path) if path else file()
    key = str(target)
    if key not in _cache:
        if not target.is_file():
            raise BoxFileMissing(f'no box file at {target}: copy {EXAMPLE} there (or point {ENV} at a copy) '
                                 'and fill in your machines')
        facts = Facts(json.loads(target.read_text(encoding='utf-8')))
        roles = [box.get('role') for box in facts.get('boxes', [])]
        unknown = sorted({role for role in roles if role not in ROLES})
        if unknown or len(roles) != len(set(roles)):
            raise ValueError(f'{target}: every box needs one distinct role of {", ".join(ROLES)}; got {roles}')
        _cache[key] = facts
    return _cache[key]


def use_example() -> Facts:
    """Point this process at the tracked example (the unit tests' machines) and return it."""
    os.environ[ENV] = str(EXAMPLE)
    _cache.pop(str(EXAMPLE), None)
    return load(EXAMPLE)


def box(role: str) -> Facts:
    """The box with this role."""
    for entry in load().boxes:
        if entry.role == role:
            return entry
    raise KeyError(f'{file()} declares no {role!r} box')


def has(role: str) -> bool:
    return any(entry.role == role for entry in load().boxes)


def name(role: str) -> str:
    return box(role).name


def instance(role: str) -> str:
    return box(role).instance


def ssh(role: str) -> str | None:
    return box(role).get('ssh')


def role_of(box_name: str) -> str | None:
    """The role of the box with this name, or None."""
    return next((entry.role for entry in load().boxes if entry.name == box_name), None)


def path(key: str) -> str:
    """A named path outside any box entry (paths.<key>), as written in the file."""
    return load().paths[key]


def optional_path(key: str) -> Path | None:
    """A named path from the box file, or None without a box file or that entry: a tool's default, never its only source."""
    if not present():
        return None
    value = (load().get('paths') or {}).get(key)
    return Path(value) if value else None


def this_box() -> Facts | None:
    """The box this process runs on (its hostname matches the entry's), or None without a box file or a match."""
    if not present():
        return None
    import socket
    here = {value.lower() for value in (os.environ.get('COMPUTERNAME'), socket.gethostname()) if value}
    return next((entry for entry in load().boxes if str(entry.get('hostname') or '').lower() in here), None)


def markers(kind: str) -> list[Path]:
    """This box's reservation markers of one kind (each box entry's markers.<kind>: one path or a list); a box the file
    does not describe has none, so a run there waits on nothing."""
    entry = this_box()
    value = (entry.get('markers') or {}).get(kind) if entry else None
    return [Path(item) for item in ([value] if isinstance(value, str) else value or [])]


def scratch_root() -> Path | None:
    """This box's scratch root (the entry's scratch), or None."""
    entry = this_box()
    return Path(entry.scratch) if entry and entry.get('scratch') else None


def scratch_dir(name: str) -> Path:
    """A default output directory for one tool: under this box's scratch root, else under the system temp directory."""
    import tempfile
    return (scratch_root() or Path(tempfile.gettempdir()) / 'cortex-scratch') / name


def marker_path(kind: str, default_name: str) -> Path:
    """This box's marker of one kind (the first, when the file lists several), else one under the default scratch."""
    found = markers(kind)
    return found[0] if found else scratch_dir(default_name)


def held(*kinds: str) -> list[Path]:
    """The markers of these kinds that exist now on this box (an empty list lets a run start)."""
    return [path for kind in kinds for path in markers(kind) if path.exists()]


def identifying_names(facts: Facts | None = None) -> set[str]:
    """Every name, instance, hostname and alias the file gives a machine, minus plain platform and role words."""
    facts = facts or load()
    generic = {'pc', 'mac', 'linux', 'windows', 'local', 'remote', 'laptop', 'handheld', 'host', 'client'}
    names = set()
    for entry in facts.get('boxes', []):
        for key in ('name', 'instance', 'hostname', 'ssh'):
            if entry.get(key):
                names.add(str(entry[key]))
        names.update(str(alias) for alias in entry.get('aliases', []))
    return {value for value in names if value.lower() not in generic}


def user_names(facts: Facts | None = None) -> set[str]:
    facts = facts or load()
    return {str(entry['user']) for entry in facts.get('boxes', []) if entry.get('user')} | set(facts.get('users', []))
