"""One pool-backed execution interface for real-network cases with native peers.

Contract (version 2, compatible with version 1 calls)
--------------------
run_case(repo, out, peers, match, *, drive=None, peer_boxes=None,
         dispatcher=None, registry=None) -> dict

``peers`` is a sequence of Peer objects. Each declares its name, OS, engine
count (one), required free memory, display size, quiet/exclusive requirement,
and whether its screen is reviewed. Peer.share_ok defaults to True for screen
peers; quiet, reviewed, held and recorder peers always reserve a box alone.
Peer.held declares any target of a hold or stall lever before allocation.
Peer.recorder requires the controller's private Windows video recorder.
Peer.readback=True lets the pool choose an unpinned reviewed screen's native
readback box; its False default keeps the version 1 controller placement.
An explicitly pinned reviewed screen peer may use its own box's readback;
an unpinned reviewed peer retains the version 1 controller placement.
The
case supplies arguments, environment and fixtures through Peer or by calling
case.make_run() in ``drive(case)``. ``match`` is a Match with the game's port,
the lane's directory port, and optional unchanged case parameters. The pool
chooses and claims fitting machines, freezes committed inputs, verifies the
shipped/native executable hash, and starts native runners/tasks. ``drive``
still stages the case's scripts, orders starts and applies its own assertions.
Without ``drive``, the call starts host first and finishes every declared peer.
It collects verified evidence into ``out`` and returns topology="spread",
peer_boxes, native identities, executable hashes, records and driver_result.
Shareable peers may use one box within the pool's memory, engine and CPU limits;
quiet/timing peers remain on distinct idle boxes. The existing pool chooses
the least loaded fitting box for a shareable group and admits each native claim.
Reviewed and held peers request the pool's whole-box claim to keep their
isolation when an installed pool scopes its screen-sharing rule to one case.

prepare_case(...), the same arguments except drive, exposes the same Case for
drivers whose existing control loop needs runner-compatible handles. Always
use it in a with statement (or in @managed_case). Case.make_run() returns a
handle with start/finish/poll/terminate/close/suspend/resume. Levers execute in
the native runner owning that peer; a Windows suspend requires os="windows".
Case.synchronize() mirrors declared complete JSON and exact .mark/.txt presence
files, logs and video indices. Gameplay travels over real network sockets;
SSH carries signaling and evidence only. No router mapping is requested.
Caller paths are private staging
paths; the helper maps them to the native case root, never to an owner's tree.

Refusals raise SpreadRefusal and write spread-result.json with topology,
peer_boxes, refused_peer, refused_box and the exact reason. Prefixes are
"spread peer <name> on <box>: <pool reason>", "no distinct fitting box",
"match port <port> differs from host port <port>", "native executable hash
differs from preparation", and "Windows process suspension is unavailable".
No fitting box means immediate refusal; the caller may retry after routing.
No case assertion, oracle, timeout or default single-box launch is changed.
Peer.output_name optionally declares an existing non-ASCII output directory;
make_run(..., role=...) also accepts its declared logical peer explicitly.
Text scripts retain their original UTF-8 or legacy Windows byte encoding.
Peer.lane (or Match.parameters["lane"]) supplies the caller's pool priority
lane; the existing pool still decides admission. Its owned pending ticket is
published before probing and is released on refusal or native launch.
Peer.block_udp reserves only
declared discovery ports on that peer's native machine for the case's lever.
The default network is ICE. Match.parameters["network"]="direct" preserves
explicit ICE-Off/Unlisted inputs and substitutes only deliberate loopback
join addresses with the host's existing network address. An optional
Match.parameters["host_address"] supplies that address; otherwise it is read
on the native host. Loopback/unspecified host addresses refuse with the box
named. Explicit ICE-Off in the default ICE mode also refuses instead of being
overwritten. Scratch placement derives from the installed box catalog.
Match.parameters["directory"] may instead supply an already running caller's
DIRECTORY_URL, DIRECTORY_PIN and DIRECTORY_ROOT descriptor. The caller keeps
that service alive and owns its assertions. Optional peer_directory_urls maps
logical peers to the caller's loopback TLS proxies; only those signaling ports
are forwarded. Their staged connection settings and install keys are retained.
Set join_by_session=False when the caller joins through directory rows itself.
The byte-pinned interface is shipped as a pool control input, so callers do
not need it committed into their own branch before using the published call.
Declared .bin protocol inputs in peer arguments retain their exact bytes.
make_run(..., runtime=...) copies owned retained regular files, settings and
Data overlays without following links into the immutable game tree. Native
overlay ancestors are private; other Data directories remain linked inputs.
Incomplete retained runtimes and private tickets refuse before launch.
A multi-peer spread match uses at least two physical boxes even when no
reviewed peer is declared. Pinning every peer to one box refuses by name.
Configured dispatcher discovery uses CORTEX_POOL_DISPATCHER, the installed
box_facts adapter, or --pool-dispatcher. There is no alternate dispatcher.
"""
from __future__ import annotations

import argparse
import base64
import contextlib
import contextvars
import ctypes
from dataclasses import dataclass, field
import functools
import hashlib
import importlib
import ipaddress
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
from urllib.parse import urlsplit
import uuid
import zlib

def engine_executable(repo):
    from run_sim_test import engine_executable as resolve
    return resolve(repo)


def file_sha256(path):
    from run_sim_test import file_sha256 as digest
    return digest(path)


TOPOLOGY_LOCAL = "single-box: not proof"
_options = None
_cases = contextvars.ContextVar("spread_cases", default=None)
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)


class SpreadRefusal(RuntimeError):
    pass


@dataclass(frozen=True)
class Peer:
    name: str
    os: str = "any"
    engines: int = 1
    memory: float = 0
    size: tuple[int, int] | None = None
    quiet: bool = False
    reviewed: bool = False
    args: tuple[str, ...] = ()
    env: dict = field(default_factory=dict)
    timeout: float = 300
    expected: tuple = ()
    fixtures: tuple = ()
    output_name: str | None = None
    block_udp: tuple[int, ...] = ()
    lane: str | None = None
    share_ok: bool = True
    held: bool = False
    recorder: bool = False
    readback: bool = False

    def __post_init__(self):
        if not re.fullmatch(r"[A-Za-z0-9_-]+", self.name):
            raise ValueError("peer name must be a safe path component")
        if self.engines != 1:
            raise ValueError("a spread peer reserves exactly one engine")
        if type(self.share_ok) is not bool:
            raise ValueError("share_ok must be a boolean")
        if self.quiet or self.reviewed or self.held or self.recorder:
            object.__setattr__(self, "share_ok", False)
        if self.output_name is not None and (not self.output_name or self.output_name in (".", "..") or any(char in self.output_name for char in '/\\\0<>:"|?*')):
            raise ValueError("output name must be one safe directory component")
        if any(type(port) is not int or not 1024 <= port <= 65535 for port in self.block_udp):
            raise ValueError("blocked discovery ports must be declared test ports")


@dataclass(frozen=True)
class Match:
    port: int
    directory_port: int | None = None
    parameters: dict = field(default_factory=dict)

    def __post_init__(self):
        if not 1024 <= self.port <= 65535:
            raise ValueError("match port is outside the test range")
        if self.directory_port is not None and (not 1024 <= self.directory_port <= 65535 or self.directory_port == self.port):
            raise ValueError("directory port must be a different lane-owned test port")


def native_address_probe():
    """Read the host's existing network address without changing its network."""
    program = shutil.which("tailscale")
    if not program and sys.platform == "win32":
        candidate = Path("C:/Program Files/Tailscale/tailscale.exe")
        if candidate.is_file():
            program = str(candidate)
    if program:
        result = subprocess.run([program, "ip", "-4"], capture_output=True, text=True, timeout=10, creationflags=NO_WINDOW)
        if result.returncode == 0:
            for line in result.stdout.splitlines():
                address = ipaddress.ip_address(line.strip())
                if not address.is_loopback and not address.is_unspecified:
                    return str(address)
    addresses = {row[4][0] for row in socket.getaddrinfo(socket.gethostname(), None, family=socket.AF_INET)}
    if sys.platform.startswith("linux"):
        try:
            route = subprocess.run(["ip", "-j", "route", "get", "192.0.2.1"], capture_output=True, text=True, timeout=10)
            for item in json.loads(route.stdout) if route.returncode == 0 else ():
                address = ipaddress.ip_address(item.get("prefsrc") or item.get("src") or "127.0.0.1")
                if address.version == 4 and not address.is_loopback and not address.is_link_local and not address.is_unspecified:
                    return str(address)
        except (OSError, ValueError, subprocess.TimeoutExpired):
            pass
        result = subprocess.run(["hostname", "-I"], capture_output=True, text=True, timeout=10)
        if result.returncode == 0:
            for value in result.stdout.split():
                address = ipaddress.ip_address(value)
                if address.version == 4:
                    addresses.add(str(address))
    fitting = sorted(address for address in addresses if not ipaddress.ip_address(address).is_loopback and not ipaddress.ip_address(address).is_link_local)
    if not fitting:
        raise RuntimeError("native host has no routable address")
    return fitting[0]


def atomic_bytes(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".incoming-" + uuid.uuid4().hex)
    try:
        temporary.write_bytes(data)
        deadline = time.monotonic() + 2
        while True:
            try:
                os.replace(temporary, path)
                break
            except PermissionError:
                # Retry sharing conflicts while retaining the complete incoming file.
                if time.monotonic() >= deadline:
                    raise
                time.sleep(.01)
    finally:
        temporary.unlink(missing_ok=True)


def write_json(path, value):
    atomic_bytes(path, (json.dumps(value, indent=2) + "\n").encode())


def read_json(path, default=None):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8-sig"))
    except (OSError, ValueError):
        return default


def native_refusal(result, log=""):
    """Keep a native capacity reason when the outer task has only a generic code."""
    reason = result.get("routing_reason") or result.get("reason")
    if reason == "native launch refused":
        lines = [line for line in log.splitlines() if line.startswith(("[box-hold] waiting:", "[box-hold] REFUSED:"))]
        if lines:
            return "\n".join(lines)
    return reason or f"native task exited {result.get('exit_code')} before the peer runner started"


def directory_endpoint(value):
    """Accept only a caller-owned loopback TLS test service for forwarding."""
    parsed = urlsplit(value if "://" in value else "https://" + value)
    if (parsed.scheme != "https" or parsed.hostname not in ("127.0.0.1", "localhost") or parsed.username or parsed.password
            or parsed.path not in ("", "/") or parsed.query or parsed.fragment or not parsed.port or not 1024 <= parsed.port <= 65535):
        raise ValueError("caller directory must be a loopback TLS test endpoint")
    return parsed.port


def add_arguments(parser):
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--spread", action="store_true", help="one peer per real machine, selected by the installed pool")
    group.add_argument("--peer-boxes", help="host=BOX,seat2=BOX,... (actual peer names also accepted)")
    parser.add_argument("--pool-dispatcher", type=Path, help="installed run_on_pool.py; defaults to the configured dispatcher")
    parser.add_argument("--pool-registry", type=Path, help="the dispatcher's single box/port registry")
    parser.add_argument("--peer-port", action="append", default=[], metavar="PEER=PORT", help="explicit peer match port (also supports a wrong-parameter detecting run)")


def enabled(options=None):
    options = _options if options is None else options
    return bool(options and (getattr(options, "spread", False) or getattr(options, "peer_boxes", None)))


def configure(options):
    global _options
    _options = options


def topology(options=None, count=2):
    return "spread" if enabled(options) else TOPOLOGY_LOCAL if count > 1 else "single-peer"


def check_port_block(port, block, count=1):
    """Keep every declared case port inside its caller's game-port allocation."""
    if not re.fullmatch(r"\d+-\d+", block):
        raise ValueError("port block must be LO-HI")
    low, high = map(int, block.split("-"))
    if not 1024 <= low <= port <= port + count - 1 <= high <= 65535:
        raise ValueError("case ports are outside the declared lane-owned block")
    return low, high


def managed_case(function):
    """Release all claims even if the caller's unchanged assertion raises."""
    @functools.wraps(function)
    def wrapped(*args, **kwargs):
        members = []
        token = _cases.set(members)
        try:
            return function(*args, **kwargs)
        finally:
            try:
                with contextlib.ExitStack() as cleanup:
                    for case in members:
                        cleanup.callback(case.close)
            finally:
                _cases.reset(token)
    return wrapped


def pairs(value):
    result = {}
    for item in (value.split(",") if isinstance(value, str) else value or ()):
        key, separator, val = item.partition("=")
        if not separator or not key or not val or key.casefold() in result:
            raise ValueError("expected unique PEER=VALUE assignments")
        result[key.casefold()] = val
    return result


def role_value(values, names, name):
    index = names.index(name)
    aliases = (name.casefold(), "host" if index == 0 else f"seat{index + 1}")
    return next((values[key] for key in aliases if key in values), None)


def installed_pool(dispatcher=None):
    path = dispatcher or os.environ.get("CORTEX_POOL_DISPATCHER")
    if not path:
        try:
            import box_facts
            path = box_facts.pool_dispatcher()
        except ImportError:
            config = read_json(os.environ.get("CORTEX_BOXES", Path.home()/".cortex-modern/boxes.json"), {})
            path = config.get("pool_dispatcher") or config.get("tool_paths", {}).get("pool_dispatcher")
    if not path or not Path(path).is_file():
        raise SpreadRefusal("spread requires the installed pool dispatcher; set --pool-dispatcher or CORTEX_POOL_DISPATCHER")
    path = Path(path).resolve()
    sys.path.insert(0, str(path.parent))
    pool = importlib.import_module("pool")
    transport = importlib.import_module("pool_transport")
    return pool, transport, path


def safe_relative(value):
    path = Path(value)
    if path.is_absolute() or any(part in ("..", "") for part in path.parts) or "\\" in str(value) or ":" in str(value):
        raise ValueError("case evidence path escapes its declared root")
    return path


def public_file(path):
    path = Path(path)
    return not (path.is_symlink() or (getattr(path, "is_junction", lambda: False)()) or
                path.suffix.lower() in (".ticket", ".key") or path.name in (".env", "key.pem") or
                path.name.startswith((".env.", "id_")))


def map_text(value, mappings):
    def mapped(text):
        for old, new in sorted(mappings, key=lambda pair: len(pair[0]), reverse=True):
            variants = {old, old.replace("\\", "/"), old.replace("\\", "\\\\")}
            for prefix in sorted(variants, key=len, reverse=True):
                if new.startswith("/"):
                    # Map the whole Windows path tail on a native POSIX peer.
                    pattern = re.escape(prefix) + r'(?P<tail>(?:[\\/]+[^\\/\r\n"\'<>|?]*)*)'
                    text = re.sub(pattern, lambda match: new + re.sub(r"\\+", "/", match["tail"]), text)
                else:
                    text = text.replace(prefix, new)
        return text
    text = str(value)
    if text.lstrip().startswith(("{", "[")):
        try:
            document = json.loads(text)
        except ValueError:
            pass
        else:
            def walk(node):
                if isinstance(node, str):
                    return mapped(node)
                if isinstance(node, list):
                    return [walk(child) for child in node]
                if isinstance(node, dict):
                    return {key: walk(child) for key, child in node.items()}
                return node
            return json.dumps(walk(document))
    return mapped(text)


def complete_signal(data, relative=None):
    """Keep declared presence bytes; publish JSON only after its write completes."""
    if relative and Path(relative).suffix in (".mark", ".txt"):
        return True
    if data == b"":
        return True
    try:
        json.loads(data)
        return True
    except ValueError:
        return False


def map_script(data, mappings, session=None):
    """Map private paths without transcoding the caller's script bytes."""
    try:
        text, encoding = data.decode("utf-8"), "utf-8"
    except UnicodeDecodeError:
        text, encoding = data.decode("cp1252", errors="surrogateescape"), "cp1252"
    text = map_text(text, mappings)
    if session and "TextJoinAddress" in text:
        text = re.sub(r"(?m)^(settext TextJoinAddress)\s+127\.0\.0\.1(?::\d+)?\s*$", lambda match: match[1] + " session:" + session, text)
    return text.encode(encoding, errors="surrogateescape")


def declared_signals(root):
    """Extract actual wait/signal declarations, never mirror arbitrary JSON files."""
    root = Path(root).resolve()
    result = set()
    for path in root.glob("**/*"):
        if not path.is_file() or not public_file(path) or path.suffix not in (".txt", ".json"):
            continue
        if any(part in ("runtime", ".spread", ".native", "video") for part in path.relative_to(root).parts):
            continue
        if path.stat().st_size > 1 << 20:
            continue
        text = path.read_text(encoding="utf-8-sig", errors="replace")
        candidates = re.findall(r"(?m)^wait_file\s+(.+?)\s+\d+\s*$", text)
        candidates += re.findall(r"(?m)^touch_file\s+(.+?)\s*$", text)
        try:
            value = json.loads(text)
        except ValueError:
            value = None
        def walk(node):
            if isinstance(node, dict):
                if node.get("op") == "wait_file" and isinstance(node.get("path"), str):
                    candidates.append(node["path"])
                if node.get("op") == "signal" and isinstance(node.get("name"), str):
                    candidates.append(str(path.parent/(node["name"] + ".json")))
                for key, child in node.items():
                    if key in ("wait_file", "signal", "file") and isinstance(child, str) and Path(child).suffix in (".json", ".mark", ".txt"):
                        candidates.append(child)
                    walk(child)
            elif isinstance(node, list):
                for child in node:
                    walk(child)
        walk(value)
        for candidate in candidates:
            target = Path(candidate)
            if not target.is_absolute():
                target = path.parent / target
            target = target.resolve()
            if target.is_relative_to(root) and target.suffix in (".json", ".mark", ".txt"):
                result.add(target.relative_to(root).as_posix())
    # The existing video and net-ui probe protocols publish these exact names.
    for directory in [*root.glob("*-stage/probe"), *root.glob("*-probe"), *root.glob("*_probe")]:
        result.add((directory/"done.json").relative_to(root).as_posix())
    for directory in root.glob("*-stage"):
        result.add((directory/"gameplay-started.json").relative_to(root).as_posix())
    return sorted(result)


def prepare_case(repo, out, peers, match, *, peer_boxes=None, dispatcher=None, registry=None, force=False):
    if not force and not enabled():
        return None
    options = _options
    case = Case(repo, out, peers, match,
                peer_boxes=peer_boxes or getattr(options, "peer_boxes", None),
                dispatcher=dispatcher or getattr(options, "pool_dispatcher", None),
                registry=registry or getattr(options, "pool_registry", None),
                peer_ports=pairs(getattr(options, "peer_port", [])))
    members = _cases.get()
    if members is not None:
        members.append(case)
    return case


def run_case(repo, out, peers, match, *, drive=None, peer_boxes=None, dispatcher=None, registry=None):
    """The complete, stable one-call interface; assertions remain in drive()."""
    with prepare_case(repo, out, peers, match, peer_boxes=peer_boxes, dispatcher=dispatcher, registry=registry, force=True) as case:
        if drive:
            result = drive(case)
        else:
            handles = [case.make_run(repo, peer.args, Path(out)/(peer.output_name or peer.name), peer.timeout, env=peer.env,
                                     expected=peer.expected, fixtures=peer.fixtures) for peer in peers]
            for handle in handles:
                handle.start()
            result = {handle.role: handle.finish() for handle in handles}
        return {**case.result(), "driver_result": result}


class Case:
    def __init__(self, repo, out, peers, match, *, peer_boxes=None, dispatcher=None, registry=None, peer_ports=None):
        self.repo, self.out = Path(repo).resolve(), Path(out).resolve()
        self.out.parent.mkdir(parents=True, exist_ok=True)
        self.peers, self.match = list(peers), match
        self.names = [peer.name for peer in self.peers]
        if not self.names or len(set(name.casefold() for name in self.names)) != len(self.names):
            raise ValueError("a spread case declares unique peers")
        self.output_names = {peer.name: peer.output_name or peer.name for peer in self.peers}
        if len({name.casefold() for name in self.output_names.values()}) != len(self.names):
            raise ValueError("a spread case declares unique output names")
        self.interface_source = Path(__file__).read_text(encoding="utf-8")
        self.interface_sha256 = hashlib.sha256(self.interface_source.encode()).hexdigest()
        self.pool, self.transport_module, self.dispatcher = installed_pool(dispatcher)
        self.registry = Path(registry or self.dispatcher.with_name("boxes.json"))
        self.pins, self.peer_ports = pairs(peer_boxes), peer_ports or {}
        self.members, self.runs, self.tunnels, self.refusals, self.identities, self.pending = {}, {}, [], [], {}, []
        self.lock = threading.RLock()
        self.stack = contextlib.ExitStack()
        self.closed = False
        self.last_sync = 0
        self.id = uuid.uuid4().hex
        self.control = self.out.parent/(".spread-" + self.id)
        self.control.mkdir(exist_ok=True)
        # Keep engine artifacts in the caller's catalog-derived scratch lane.
        catalog = self.pool.load_registry(self.registry)["boxes"]
        local = next((box for box in catalog if box["kind"] == "local"), None)
        scratch = Path(local["scratch"]).resolve() if local else self.out.parent
        self.lane_root = next((parent for parent in (self.out, *self.out.parents) if parent.parent == scratch), self.out.parent)
        self.lane = self.lane_root.name
        try:
            self.allocate()
            self.prepare()
            self.connect_directory()
        except BaseException as error:
            if not isinstance(error, SpreadRefusal):
                error = SpreadRefusal(str(error))
            self.save(error=str(error))
            self.close()
            raise error

    def refuse(self, name, box, reason):
        text = f"spread peer {name} on {box}: {reason}"
        self.refusals.append(dict(peer=name, box=box, reason=reason, text=text))
        print(text, flush=True)
        self.save(error=text)
        return SpreadRefusal(text)

    def backend(self):
        module = self.transport_module
        # Use the installed facts adapter with this caller's immutable inputs.
        source_repo = self.repo if (self.repo/"tools/box_facts.py").is_file() else Path(module.worker.facts.__file__).resolve().parents[1]
        backend = module.Transport(repo=source_repo, work=self.lane_root/".spread-inputs")
        backend.repo = self.repo
        backend.sources["spread_peers.py"] = self.interface_source
        adapter = Path(module.worker.facts.__file__).with_name("pool_run.py")
        if adapter.is_file():
            backend.sources["pool_run.py"] = adapter.read_text(encoding="utf-8")
        preflight = Path(__file__).with_name("cross_peers.py")
        self.preflight_source = preflight.read_text(encoding="utf-8") if preflight.is_file() else None
        backend.control_id = hashlib.sha256(json.dumps(dict(backend.sources, preflight=self.preflight_source), sort_keys=True).encode()).hexdigest()[:20]
        backend.guard = self.guard
        limits = read_json(os.environ.get("CORTEX_SPREAD_LIMITS", ""), {})
        if limits:
            original_probe = backend.probe
            def probe(box, **kwargs):
                constraint = next((value for key, value in limits.items() if key.casefold() == box["name"].casefold()), {})
                if constraint.get("engines_max"):
                    box["engines_max"] = min(box["engines_max"], constraint["engines_max"])
                    box["max_engines"] = box["engines_max"]
                if constraint.get("free_floor_gb"):
                    box["free_floor_gb"] = max(box["free_floor_gb"], constraint["free_floor_gb"])
                    box["min_free_gb"] = box["free_floor_gb"]
                return original_probe(box, **kwargs)
            backend.probe = probe
        return backend

    def allocate(self):
        used, exclusive = [], []
        identities = {}
        catalog = self.pool.load_registry(self.registry)["boxes"]
        reviewed_box = next((box["name"] for box in catalog if box["kind"] == "local" and box["os"] == "windows"), None)
        # Claim constrained peers first while retaining the case's seat order.
        ordered = sorted(self.peers, key=lambda peer: (peer.share_ok, not peer.reviewed, role_value(self.pins, self.names, peer.name) is None, peer.os == "any"))
        share_target = None
        for peer in ordered:
            backend = self.backend()
            pin = role_value(self.pins, self.names, peer.name)
            if peer.recorder or peer.reviewed and not peer.readback and not pin:
                if pin and pin.casefold() != (reviewed_box or "").casefold():
                    raise self.refuse(peer.name, pin, "reviewed screen requires the controller's private Windows recorder")
                pin = reviewed_box
                if not pin:
                    raise self.refuse(peer.name, "unassigned", "reviewed screen requires a registered local Windows recorder")
            if pin and peer.quiet and any(box["name"].casefold() == pin.casefold() and box.get("timing") is False for box in catalog):
                raise self.refuse(peer.name, pin, "catalog does not permit timing measurements on this box")
            excluded = (exclusive if peer.share_ok else used) + [box["name"] for box in catalog if peer.quiet and box.get("timing") is False]
            if not pin:
                excluded += [value for other in ordered if other.name != peer.name and
                             (value := role_value(self.pins, self.names, other.name)) and not (peer.share_ok and other.share_ok)]
            if peer.share_ok and len(ordered) > 1 and len(self.members) == len(ordered) - 1 and len(set(used)) == 1:
                if pin and pin.casefold() == used[0].casefold():
                    raise self.refuse(peer.name, pin, "spread match requires at least two physical boxes")
                excluded += used
            needs = self.pool.Needs(os=peer.os, engines=peer.engines, gpu=bool(peer.size), memory=peer.memory,
                                    alone=peer.quiet or peer.reviewed or peer.held or peer.recorder, size=peer.size, only_box=pin, excluded=tuple(excluded),
                                    case_id=self.id, peer_id=peer.name, share_ok=peer.share_ok, reviewed=peer.reviewed or peer.recorder, held=peer.held)
            caller_lane = peer.lane or self.match.parameters.get("lane")
            label = f"{caller_lane}: spread" if caller_lane else "spread"
            request = dict(run_id=uuid.uuid4().hex, token=uuid.uuid4().hex, label=f"{label}: {self.out.name}/{peer.name}",
                           lane=caller_lane or self.lane, case_id=self.id, peer_id=peer.name,
                           owner=dict(pid=os.getpid(), machine=self.transport_module.worker.facts.machine_name(),
                                      process_start=self.transport_module.worker.facts.process_start(os.getpid())),
                           out=str(self.control/peer.name/"results"), command=[], hang_guard=max(600, peer.timeout + 300), enqueued_at=time.time())
            with self.transport_module.worker.mutex(self.pool.queue_root(self.registry)/".admission.lock", wait=60):
                ticket = self.pool.write_ticket(self.registry, needs, request)
            self.pending.append((ticket, request["token"]))
            self.pending_peers = getattr(self, "pending_peers", {})
            self.pending_peers[request["token"]] = peer.name
            self.stack.callback(self.transport_module.worker.facts.release_reservation, ticket, request["token"])
            if peer.share_ok and not pin and share_target is None:
                group = [item for item in ordered if item.share_ok and item.name not in self.members and
                         role_value(self.pins, self.names, item.name) is None and item.os in (peer.os, "any")]
                if len(group) > 1:
                    sizes = [item.size for item in group if item.size]
                    together = self.pool.Needs(os=peer.os, engines=len(group), gpu=bool(sizes), memory=max(item.memory for item in group),
                                               size=tuple(map(max, zip(*sizes))) if sizes else None, excluded=tuple(excluded),
                                               case_id=self.id, share_ok=True)
                    grouped, _, _ = self.pool.candidates(self.registry, together, backend)
                    if grouped:
                        share_target = grouped[0][1]["name"]
            fitting, reasons, states = self.pool.candidates(self.registry, needs, backend)
            if peer.share_ok and not pin and share_target:
                fitting.sort(key=lambda item: item[1]["name"].casefold() != share_target.casefold())
            chosen = None
            for _, box, state in fitting:
                try:
                    with self.transport_module.worker.mutex(self.pool.queue_root(self.registry)/".admission.lock", wait=60):
                        blocker = self.pool.priority_blocker(self.registry, box, state, request)
                        if blocker:
                            reasons[box["name"]] = "yielded to " + blocker["label"]
                            continue
                        claim = backend.claim(box, needs, request)
                except Exception as error:
                    reasons[box["name"]] = str(error)
                    self.refuse(peer.name, box["name"], str(error))
                    continue
                print(f"ROUTED: {box['name']} - engines {state.get('engines', 0)}/{box['engines_max']}, free {state['free_gb']:.2f} GB; peer {peer.name}", flush=True)
                chosen = (box, claim, request, backend)
                break
            if chosen is None:
                details = "; ".join(f"{name}: {reason}" for name, reason in reasons.items() if reason not in ("excluded by the caller", "another box was pinned"))
                raise self.refuse(peer.name, pin or "unassigned", "no distinct fitting box; " + details)
            self.members[peer.name] = chosen
            used.append(chosen[0]["name"])
            if not peer.share_ok:
                exclusive.append(chosen[0]["name"])
            hostname = chosen[0].get("hostname") or states[chosen[0]["name"]].get("hostname")
            if not hostname:
                raise self.refuse(peer.name, chosen[0]["name"], "native machine identity is unavailable")
            identities[peer.name] = dict(machine_id=hostname.casefold())
        self.check_identities(identities)
        for name in self.names:
            port = int(role_value(self.peer_ports, self.names, name) or self.match.port)
            if port != self.match.port:
                raise self.refuse(name, self.members[name][0]["name"], f"match port {port} differs from host port {self.match.port}")

    def check_identities(self, identities):
        from cross_peers import require_distinct_machines
        groups = {}
        for name, identity in identities.items():
            box = self.members[name][0]["name"]
            if box in groups and groups[box]["machine_id"] != identity["machine_id"]:
                raise self.refuse(name, box, "native peers assigned one box report different machines")
            groups[box] = identity
        try:
            require_distinct_machines(groups)
        except RuntimeError as error:
            name = next(reversed(identities))
            raise self.refuse(name, self.members[name][0]["name"], str(error)) from error

    def guard(self):
        now = time.monotonic()
        if now - getattr(self, "last_renew", 0) < 8:
            return
        self.last_renew = now
        boxes = {box["name"]: box for box in self.pool.load_registry(self.registry)["boxes"]}
        for name, (box, claim, _, backend) in list(self.members.items()):
            if boxes[box["name"]]["off_limits"]:
                raise self.refuse(name, box["name"], "went off limits during the case")
            if not any(handle.started for handle in self.runs.values()):
                request = self.members[name][2]
                state = getattr(backend, "last_states", {}).get(box["name"])
                if state and (blocker := self.pool.priority_blocker(self.registry, box, state, request)):
                    raise self.refuse(name, box["name"], "yielded preparation to " + blocker["label"])
            if (name not in self.runs or not self.runs[name].finished) and not (name in self.runs and self.runs[name].native_progress.get("record")):
                try:
                    backend.rpc(box, "renew", dict(claim=claim), timeout=15)
                except RuntimeError:
                    # Recognize completed claims only through their matching terminal receipt.
                    raw = backend.rpc(box, "text", dict(path=claim["root"] + "/finished.json"), timeout=15)["text"]
                    if not raw or json.loads(raw).get("token") != claim["token"]:
                        raise

    def prepare(self):
        self.input_snapshot = self.members[self.names[0]][3].committed_inputs()
        for name in self.names:
            box, claim, request, backend = self.members[name]
            backend.active_claim, backend.active_box = claim, box
            backend.snapshot = getattr(self, "input_snapshot", None)
            backend.prepare(box, claim, request)
            if getattr(self, "preflight_source", None):
                backend.rpc(box, "install", dict(root=claim["control"], sources={"cross_peers.py": self.preflight_source}))
            self.input_snapshot = backend.snapshot
            if claim["head"] != self.input_snapshot["head"]:
                raise self.refuse(name, box["name"], "source changed after the case input snapshot was frozen")
            native_root = box["scratch"].rstrip("/") + "/" + self.lane + "/native-" + self.id
            claim["case_root"] = native_root
            # Keep the driver's game port outside the pool's control assignment.
            if self.match.port in range(*[claim["ports"][0], claim["ports"][1] + 1]):
                raise self.refuse(name, box["name"], "driver match port overlaps the pool's control port map")
            self.guard()
        windows_hashes = {claim["exe_sha256"] for box, claim, _, _ in self.members.values() if box["os"] == "windows"}
        if len(windows_hashes) > 1:
            raise SpreadRefusal("Windows executable changed between peer shipments")

    def connect_directory(self):
        self.network = self.match.parameters.get("network", "ice")
        if self.network not in ("ice", "direct"):
            raise self.refuse(self.names[0], self.members[self.names[0]][0]["name"], "unknown declared network mode")
        if self.network == "direct":
            box, claim, _, backend = self.members[self.names[0]]
            address = self.match.parameters.get("host_address")
            if not address:
                from cross_peers import remote_command
                script = "import sys;sys.path.insert(0,sys.argv[1]);from spread_peers import native_address_probe;print(native_address_probe())"
                command = [box["python"], "-c", script, claim["control"]]
                if box["kind"] != "local":
                    command = remote_command(box, command)
                    command[1:1] = ["-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5"]
                try:
                    address = backend.guarded_run(command, timeout=20).decode().strip()
                except Exception as error:
                    raise self.refuse(self.names[0], box["name"], str(error))
            try:
                parsed = ipaddress.ip_address(address)
            except ValueError:
                raise self.refuse(self.names[0], box["name"], "direct host address must be a native network address")
            if parsed.is_loopback or parsed.is_unspecified:
                raise self.refuse(self.names[0], box["name"], "direct host address must identify its real machine")
            self.host_address = str(parsed)
            if not self.match.parameters.get("directory"):
                self.directory = None
                return
        caller_directory = self.match.parameters.get("directory")
        if len(self.peers) < 2 and not caller_directory:
            self.directory = None
            return
        if caller_directory:
            try:
                directory_port = directory_endpoint(caller_directory["DIRECTORY_URL"])
                pin, root = caller_directory["DIRECTORY_PIN"], Path(caller_directory["DIRECTORY_ROOT"])
                if not re.fullmatch(r"[a-fA-F0-9]{64}", pin) or not root.is_dir():
                    raise ValueError("caller directory needs its exact certificate pin and existing result root")
            except (KeyError, TypeError, ValueError) as error:
                raise self.refuse(self.names[0], self.members[self.names[0]][0]["name"], str(error))
            self.directory = dict(caller_directory, DIRECTORY_ROOT=str(root), preserve_settings=True)
        else:
            from e2e.directory import serve
            directory_port = self.match.directory_port
            if directory_port is None:
                directory_port = self.members[self.names[0]][1]["directory_port"]
            self.directory = self.stack.enter_context(serve(self.control/"directory", directory_port, block=(directory_port, directory_port)))
        self.directory_port = directory_port
        peer_urls = self.match.parameters.get("peer_directory_urls", {})
        for name in self.names:
            box, claim, _, _ = self.members[name]
            target = role_value(peer_urls, self.names, name) or self.directory["DIRECTORY_URL"]
            try:
                target_port = directory_endpoint(target)
            except ValueError as error:
                raise self.refuse(name, box["name"], str(error))
            local = target_port if box["kind"] == "local" else claim["directory_port"]
            claim["signal_port"] = local
            if box["kind"] == "local":
                continue
            log = (self.control/f"tunnel-{name}.log").open("ab")
            process = subprocess.Popen(["ssh", "-N", "-o", "BatchMode=yes", "-o", "ExitOnForwardFailure=yes", "-o", "ServerAliveInterval=15",
                                        "-R", f"127.0.0.1:{local}:127.0.0.1:{target_port}", box["ssh"]],
                                       stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
            self.tunnels.append((process, log))
        time.sleep(1)
        for (process, _), name in zip(self.tunnels, [n for n in self.names if self.members[n][0]["kind"] != "local"]):
            if process.poll() is not None:
                raise self.refuse(name, self.members[name][0]["name"], "private directory signaling tunnel refused")

    def published_session(self, name):
        from e2e_video import directory_session
        port = int(role_value(self.peer_ports, self.names, name) or self.match.port)
        deadline = time.monotonic() + 80
        while time.monotonic() < deadline:
            self.synchronize()
            session = directory_session(self.directory["DIRECTORY_ROOT"], port)
            if session:
                return session
            host = self.runs.get(self.names[0])
            if host and host.finished:
                break
            time.sleep(.25)
        raise self.refuse(name, self.members[name][0]["name"], f"host published no session on match port {port}")

    def make_run(self, repo, args, out, timeout=120, env=None, expected=None, *, runtime=None, fixtures=None, role=None):
        component = Path(out).name
        role = role or next((name for name, output in self.output_names.items() if output == component), component)
        if role not in self.members:
            raise ValueError(f"undeclared spread peer {role}")
        if Path(out).resolve().parent != self.out or component != self.output_names[role]:
            raise ValueError("runner output must be the peer's declared directory under the case folder")
        if role in self.runs:
            raise ValueError(f"spread peer {role} already has a runner")
        handle = Run(self, repo, args, out, timeout, env or {}, expected or (), fixtures or (), runtime, role=role)
        self.runs[role] = handle
        return handle

    def synchronize(self, *, force=False):
        with self.lock:
            if not force and time.monotonic() - self.last_sync < .3:
                return
            self.last_sync = time.monotonic()
            self.guard()
            for handle in list(self.runs.values()):
                if not handle.started or handle.finished:
                    continue
                box, claim, _, backend = self.members[handle.role]
                raw = backend.rpc(box, "text", dict(path=claim["root"] + "/progress.json"), timeout=20)["text"]
                if not raw:
                    terminal = backend.rpc(box, "text", dict(path=claim["root"] + "/finished.json"), timeout=20)["text"]
                    if terminal:
                        result = json.loads(terminal)
                        if result.get("token") == claim["token"] and result.get("exit_code") != 0:
                            handle.finished = True
                            log = backend.rpc(box, "text", dict(path=claim["root"] + "/driver.log"), timeout=20)["text"] if result.get("reason") == "native launch refused" else ""
                            reason = native_refusal(result, log)
                            write_json(self.control/handle.role/"native-refusal.json", dict(topology="spread", box=box["name"], reason=reason,
                                                                                         exit_code=result.get("exit_code"), native_root=claim["root"]))
                            raise self.refuse(handle.role, box["name"], reason)
                    continue
                progress = json.loads(raw)
                handle.native_progress = progress
                if progress.get("identity"):
                    self.identities[handle.role] = progress["identity"]
                    self.check_identities(self.identities)
                files = (json.loads(zlib.decompress(base64.b64decode(progress["packed_files"], validate=True)))
                         if progress.get("packed_files") else progress.get("files", {}))
                for relative, encoded in files.items():
                    target = self.out/safe_relative(relative)
                    atomic_bytes(target, base64.b64decode(encoded, validate=True))
                if progress.get("record"):
                    handle.record.update(progress["record"])
            signals = {}
            for relative in self.signals():
                path = self.out/safe_relative(relative)
                if path.is_file():
                    data = path.read_bytes()
                    if not complete_signal(data, relative):
                        continue
                    signals[relative] = base64.b64encode(data).decode()
            for handle in list(self.runs.values()):
                if not handle.started or handle.finished or signals == handle.sent:
                    continue
                box, claim, _, backend = self.members[handle.role]
                backend.rpc(box, "write", dict(path=claim["root"] + "/signals.json", value=signals), timeout=20)
                handle.sent = dict(signals)

    def signals(self):
        return sorted(set(declared_signals(self.out)) | set(getattr(self, "extra_signals", ())))

    def result(self):
        return dict(schema=1, topology="spread", peer_boxes={name: item[0]["name"] for name, item in self.members.items()},
                    interface_sha256=self.interface_sha256,
                    preflight_sha256=hashlib.sha256(self.preflight_source.encode()).hexdigest() if getattr(self, "preflight_source", None) else None,
                    sharing={peer.name: dict(share_ok=peer.share_ok, reviewed=peer.reviewed, held=peer.held, quiet=peer.quiet,
                                              recorder=peer.recorder, readback=peer.readback) for peer in self.peers},
                    executable_hashes={name: item[1].get("exe_sha256") for name, item in self.members.items()},
                    identities=self.identities, records={name: run.record for name, run in self.runs.items()},
                    match=dict(port=self.match.port, parameters=json.loads(json.dumps(self.match.parameters, default=str))), refusals=self.refusals)

    def save(self, error=None):
        if error:
            self.failure = error
        error = getattr(self, "failure", None)
        value = self.result()
        if error:
            value.update(error=error, passed=False)
            if self.refusals:
                value.update(refused_peer=self.refusals[-1]["peer"], refused_box=self.refusals[-1]["box"], reason=self.refusals[-1]["reason"])
        write_json(self.out/"spread-result.json", value)

    def close(self):
        if self.closed:
            return
        self.closed = True
        with contextlib.ExitStack() as cleanup:
            cleanup.callback(self.stack.close)
            for process, log in self.tunnels:
                cleanup.callback(log.close)
                if process.poll() is None:
                    process.terminate()
                    cleanup.callback(process.wait, timeout=15)
            for name, (box, claim, _, backend) in self.members.items():
                cleanup.callback(backend.release, box, claim)
                handle = self.runs.get(name)
                if handle and (handle.started or getattr(handle, "launch_attempted", False)) and not handle.finished:
                    cleanup.callback(backend.stop, box, claim)
            self.save()

    def release_pending(self, role=None):
        remaining = []
        for ticket, token in self.pending:
            if role is None or getattr(self, "pending_peers", {}).get(token) == role:
                self.transport_module.worker.facts.release_reservation(ticket, token)
            else:
                remaining.append((ticket, token))
        self.pending[:] = remaining

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


def retained_files(runtime):
    """Read owned regular state and exclude links to the immutable game tree."""
    runtime = Path(runtime).resolve()
    if not runtime.is_dir() or not (runtime / "Userdata/Settings.ini").is_file():
        raise ValueError(f"retained runtime is incomplete: {runtime}")
    result = {}
    for directory, names, files in os.walk(runtime, followlinks=False):
        names[:] = [name for name in names if public_file(Path(directory) / name)]
        for name in files:
            path = Path(directory) / name
            if path.is_symlink():
                continue
            if not public_file(path):
                raise SpreadRefusal("retained private credentials or tickets require the existing credential delivery channel")
            result[path.relative_to(runtime)] = path.read_bytes()
    return result


def link_directory(source, target):
    """Link immutable directories without modifying their files."""
    if sys.platform == "win32":
        quoted = lambda path: "'" + str(path).replace("'", "''") + "'"
        subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command",
                        "New-Item -ItemType Junction -Path " + quoted(target) + " -Target " + quoted(source) + " | Out-Null"],
                       check=True, creationflags=subprocess.CREATE_NO_WINDOW)
    else:
        target.symlink_to(source, target_is_directory=True)


def overlay_data(source, target, paths):
    """Give each overlay a private ancestor while linking other data directories."""
    target.mkdir(parents=True, exist_ok=False)
    for child in source.iterdir():
        destination = target / child.name
        below = [path for path in paths if path.parts[0] == child.name]
        if child.is_dir():
            if below:
                overlay_data(child, destination, [Path(*path.parts[1:]) for path in below if len(path.parts) > 1])
            else:
                link_directory(child, destination)
        else:
            shutil.copyfile(child, destination)


def native_retained_runtime(repo, out, files, fixtures):
    """Materialize retained state with private data overlays for the native runner."""
    runtime = out.parent / (out.name + "-retained-runtime")
    runtime.mkdir(parents=True, exist_ok=False)
    for name in ("Userdata", "Mods", "ScreenShots", "Temp"):
        (runtime / name).mkdir()
    overlay_data(Path(repo) / "Data", runtime / "Data", [Path(*path.parts[1:]) for path in files if path.parts[0] == "Data"])
    staged = ["preview_window_modcompat.lua", *fixtures]
    for name in staged:
        source = Path(repo) / "tools/fixtures" / name
        if source.is_file():
            atomic_bytes(runtime / "tools/fixtures" / Path(name).name, source.read_bytes())
    for relative, data in files.items():
        atomic_bytes(runtime / relative, data)
    return runtime


class Run:
    """A native-runner handle with local staging paths for existing case logic."""
    def __init__(self, case, repo, args, out, timeout, env, expected, fixtures, runtime, *, role=None):
        self.case, self.repo, self.out = case, Path(repo).resolve(), Path(out).resolve()
        self.role, self.output_name, self.timeout, self.env = role or self.out.name, self.out.name, timeout, dict(env)
        self.expected, self.fixtures, self.retained = list(expected), list(fixtures), runtime
        self.cwd = self.out/"runtime"
        self.started = self.finished = False
        self.record, self.native_progress, self.sent = {}, {}, {}
        for name in ("Userdata", "Mods", "ScreenShots", "Temp"):
            (self.cwd/name).mkdir(parents=True, exist_ok=True)
        # Stage only private settings; the native runner creates its Data link.
        from run_sim_test import RUNTIME_SETTINGS, seed_settings
        if runtime is not None:
            self.retained = Path(runtime).resolve()
            for relative, data in retained_files(self.retained).items():
                atomic_bytes(self.cwd / relative, data)
        else:
            source = self.repo/"Userdata/Settings.ini"
            text = source.read_text(encoding="utf-8-sig") if source.is_file() else "SettingsMan\n"
            (self.cwd/"Userdata/Settings.ini").write_text(text, encoding="utf-8")
            seed_settings(self, RUNTIME_SETTINGS)
        self.argv = [str(engine_executable(repo)), "-headless", *map(str, args)]
        write_json(self.out/"runtime.json", dict(executable=self.argv[0], cwd=str(self.cwd), settings_overrides=RUNTIME_SETTINGS if runtime is None else {},
                                                topology="spread", box=case.members[self.role][0]["name"],
                                                retained_runtime=str(self.retained) if self.retained is not None else None, copied=self.retained is not None))

    @property
    def process(self):
        return self.native_progress.get("pid")

    def start(self):
        if self.started:
            raise RuntimeError("spread runner was already started")
        box, claim, request, backend = self.case.members[self.role]
        port = int(role_value(self.case.peer_ports, self.case.names, self.role) or self.case.match.port)
        if port != self.case.match.port:
            raise self.case.refuse(self.role, box["name"], f"match port {port} differs from host port {self.case.match.port}")
        args = list(self.argv[2:])
        if "-net-port" in args and int(args[args.index("-net-port") + 1]) != port:
            raise self.case.refuse(self.role, box["name"], f"match port {args[args.index('-net-port') + 1]} differs from host port {port}")
        session = None
        session_routing = self.case.directory and getattr(self.case, "network", "ice") == "ice" and self.case.match.parameters.get("join_by_session", True)
        if session_routing and self.role != self.case.names[0]:
            session = self.case.published_session(self.role)
        if session_routing:
            if "-net-ice" in args:
                if args[args.index("-net-ice") + 1].lower() == "off":
                    raise self.case.refuse(self.role, box["name"], "explicit ICE-Off requires the declared direct network mode")
            else:
                args += ["-net-ice", "on"]
            if "-net-join" in args and args[args.index("-net-join") + 1] in ("127.0.0.1", "localhost"):
                index = args.index("-net-join")
                args[index:index + 2] = ["-net-join-session", session]
        elif getattr(self.case, "network", "ice") == "direct" and "-net-join" in args and args[args.index("-net-join") + 1] in ("127.0.0.1", "localhost"):
            args[args.index("-net-join") + 1] = self.case.host_address
        mappings = [(str(self.case.out), claim["case_root"]), (self.case.out.as_posix(), claim["case_root"]),
                    (str(self.repo), claim["repo"]), (self.repo.as_posix(), claim["repo"])]
        if self.retained is not None:
            mappings += [(str(self.retained), claim["case_root"] + "/" + self.output_name + "/runtime"),
                         (self.retained.as_posix(), claim["case_root"] + "/" + self.output_name + "/runtime")]
        files = {}
        roots = [self.cwd, self.case.out/(self.output_name + "-stage"), self.case.out/(self.output_name + "-probe"), self.case.out/(self.output_name + "_probe")]
        paths = set(self.case.out.glob("*.txt"))
        for root in roots:
            if root.is_dir():
                paths.update(path for path in root.rglob("*") if path.is_file())
        for argument in [*args, *self.env.values()]:
            path = Path(str(argument))
            if path.is_file() and path.suffix.lower() in (".txt", ".json", ".lua", ".ini", ".ccreplay", ".bin"):
                if not public_file(path):
                    raise self.case.refuse(self.role, box["name"], "private credentials or tickets cannot be staged as case inputs")
                if path.resolve().is_relative_to(self.case.out):
                    paths.add(path.resolve())
                elif path.resolve().is_relative_to(self.repo):
                    continue  # immutable fixture shipped by the pool
                else:
                    # Feel input schedules are staged one level above each arm.
                    relative = ".inputs/" + path.name
                    files[relative] = base64.b64encode(path.read_bytes()).decode()
                    mappings.append((str(path.resolve()), claim["case_root"] + "/" + relative))
        for path in paths:
            if not public_file(path):
                raise self.case.refuse(self.role, box["name"], "private credentials or tickets cannot be staged as case inputs")
            data = path.read_bytes()
            if path.suffix.lower() in (".txt", ".json", ".ini", ".lua"):
                data = map_script(data, mappings, session)
                if getattr(self.case, "network", "ice") == "direct":
                    data = re.sub(rb"(?m)^(settext TextJoinAddress)\s+(?:127\.0\.0\.1|localhost)\s*$",
                                  lambda match: match[1] + b" " + self.case.host_address.encode(), data)
            files[path.relative_to(self.case.out).as_posix()] = base64.b64encode(data).decode()
        signals = self.case.signals()
        self.case.extra_signals = sorted(set(getattr(self.case, "extra_signals", ())) | set(signals))
        native = dict(schema=1, role=self.role, output_name=self.output_name, root=claim["case_root"], repo=claim["repo"], box=box,
                      case_id=self.case.id,
                      control=claim["control"], claim=self.case.transport_module.Transport.native_claim(claim),
                      args=[map_text(argument, mappings) for argument in args],
                      env={key: map_text(value, mappings) for key, value in self.env.items()},
                      timeout=self.timeout, expected=[map_text(path, mappings) for path in self.expected],
                      fixtures=self.fixtures, files=files, signals=signals, retained_runtime=self.retained is not None,
                      executable_sha256=claim["exe_sha256"], directory=self.case.directory,
                      signal_port=claim.get("signal_port"), session=session)
        native["block_udp"] = next(peer.block_udp for peer in self.case.peers if peer.name == self.role)
        if getattr(self, "private_menu", None) or getattr(self, "private_environment", None):
            raise self.case.refuse(self.role, box["name"], "private relay inputs require the existing credential delivery channel")
        # No credentials, directory private key or ticket bytes enter this spec.
        if native["directory"]:
            native["directory"] = {key: value for key, value in native["directory"].items() if key != "DIRECTORY_ROOT"}
        path = claim["root"] + "/peer-spec.json"
        try:
            backend.rpc(box, "write", dict(path=path, value=native))
            request["command"] = ["python", claim["control"] + "/spread_peers.py", "--native", path, "--out", "{OUT}"]
            self.launch_attempted = True
            backend.launch(box, claim, request)
        except SpreadRefusal:
            raise
        except Exception as error:
            raise self.case.refuse(self.role, box["name"], str(error)) from error
        self.started = True
        self.case.release_pending(self.role)
        self.deadline = time.monotonic() + self.timeout + 180
        return self

    def finish(self):
        if not self.started:
            raise RuntimeError("spread runner has not started")
        box, claim, request, backend = self.case.members[self.role]
        while time.monotonic() < self.deadline:
            self.case.synchronize()
            raw = backend.rpc(box, "text", dict(path=claim["root"] + "/finished.json"), timeout=20)["text"]
            if raw:
                result = json.loads(raw)
                break
            time.sleep(.3)
        else:
            self.terminate(reason="native runner exceeded its declared completion budget")
            raise self.case.refuse(self.role, box["name"], "native runner did not finish")
        self.case.synchronize(force=True)
        self.finished = True
        backend.end()
        backend.fetch(box, claim, request, result)
        if result.get("reroute") or result.get("lost") or result.get("refused"):
            raise self.case.refuse(self.role, box["name"], result.get("routing_reason") or result.get("reason") or "native capacity invalidated the run")
        from acceptance_remote import unpack_evidence
        manifest = self.native_progress.get("archive")
        if not manifest:
            reason = self.native_progress.get("record", {}).get("error") or "native runner ended without a verified evidence archive"
            raise self.case.refuse(self.role, box["name"], reason)
        destination = self.case.control/self.role/"evidence"
        destination.mkdir(parents=True, exist_ok=True)
        archive = destination/"evidence.tar"
        if box["kind"] == "local":
            shutil.copyfile(manifest["path"], archive)
        else:
            backend.guarded_run(["scp", "-q", "-o", "BatchMode=yes", box["ssh"] + ":" + manifest["path"].replace("\\", "/"), str(archive)], timeout=300)
        if file_sha256(archive) != manifest["sha256"]:
            raise self.case.refuse(self.role, box["name"], "fetched evidence archive hash differs")
        unpack_evidence(archive, destination, manifest["files"])
        for relative in manifest["files"]:
            source = destination/safe_relative(relative)
            # Collect the native owner's verified writable evidence.
            output_relative = relative.replace(self.output_name + "/runtime-evidence/", self.output_name + "/runtime/", 1)
            atomic_bytes(self.case.out/safe_relative(output_relative), source.read_bytes())
        self.record = read_json(self.out/"record.json", {})
        self.record.update(topology="spread", box=box["name"], native_root=claim["case_root"], native_task_exit=result.get("exit_code"))
        if self.record.get("exe_sha256") != claim["exe_sha256"]:
            raise self.case.refuse(self.role, box["name"], "native executable hash differs from preparation")
        write_json(self.out/"record.json", self.record)
        self.case.save()
        return self.record

    def poll(self):
        self.case.synchronize()
        return self.native_progress.get("record", {}).get("exit_code")

    def action(self, action, **values):
        if not self.started or self.finished:
            return
        box, claim, _, backend = self.case.members[self.role]
        if self.native_progress.get("record", {}).get("exit_code") is not None:
            return dict(action=action, box=box["name"], pid=self.process, already_finished=True)
        if action in ("suspend", "resume") and box["os"] != "windows":
            raise self.case.refuse(self.role, box["name"], "Windows process suspension is unavailable")
        if action in ("suspend", "resume") and next(peer for peer in self.case.peers if peer.name == self.role).share_ok:
            raise self.case.refuse(self.role, box["name"], "hold lever requires held=True or share_ok=False before allocation")
        token = uuid.uuid4().hex
        backend.rpc(box, "write", dict(path=claim["root"] + "/action.json", value=dict(action=action, token=token, **values)))
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            self.case.synchronize(force=True)
            if self.native_progress.get("record", {}).get("exit_code") is not None:
                return dict(action=action, box=box["name"], pid=self.process, already_finished=True)
            receipt = self.native_progress.get("action", {})
            if receipt.get("token") == token:
                if receipt.get("error"):
                    raise self.case.refuse(self.role, box["name"], receipt["error"])
                return receipt
            time.sleep(.1)
        raise self.case.refuse(self.role, box["name"], f"native {action} lever was not acknowledged")

    def suspend(self):
        return self.action("suspend")

    def resume(self):
        return self.action("resume")

    def terminate(self, code=137, reason="scenario drop"):
        return self.action("terminate", code=code, reason=reason)

    def close(self):
        if self.started and not self.finished:
            self.terminate(reason="case cleanup")


def native_snapshot(root, out, signals):
    from acceptance_peer_session import snapshot
    result = snapshot(out)
    paths = [root/safe_relative(relative) for relative in signals]
    paths += [out/"stderr.log", out/"video/frames.jsonl", out/"video/events.jsonl"]
    paths += list(root.glob("*-probe/*.json")) + list(root.glob("*_probe/*.json"))
    for path in paths:
        if path.is_file() and public_file(path) and path.stat().st_size <= 16 << 20:
            result[path.relative_to(root).as_posix()] = base64.b64encode(path.read_bytes()).decode()
    return result


def packed_snapshot(root, out, signals):
    """Lossless transport of the existing snapshot; all asserted bytes survive."""
    return base64.b64encode(zlib.compress(json.dumps(native_snapshot(root, out, signals)).encode())).decode()


def publish_signals(root, signals, allowed):
    for relative, encoded in signals.items():
        if relative not in allowed:
            raise ValueError("only declared peer gate signals may be mirrored")
        data = base64.b64decode(encoded, validate=True)
        if not complete_signal(data, relative):
            raise ValueError("peer gate signal is incomplete JSON")
        target = root/safe_relative(relative)
        if not target.is_file() or target.read_bytes() != data:
            atomic_bytes(target, data)


def native_execute(spec_path, result_out):
    spec = read_json(spec_path)
    if not spec or spec.get("schema") != 1:
        raise ValueError("invalid native peer specification")
    sys.path.insert(0, spec["control"])
    import pool_worker
    import pool_run
    from run_sim_test import make_run, seed_settings
    from feel.launch_budget import install_memory_guard
    from cross_peers import preflight_payload, refuse_mixed_build
    from acceptance_remote import pack_evidence
    root = Path(spec["root"]).resolve()
    role = spec["role"]
    output_name = spec.get("output_name", role)
    out = root/output_name
    control = Path(spec_path).parent
    result_out = Path(result_out)
    result_out.mkdir(parents=True, exist_ok=True)
    # Existing cross-peer preflight supplies physical identity and input hashes.
    preflight_root = root/"preflights"/role
    preflight_root.mkdir(parents=True, exist_ok=True)
    box = dict(spec["box"], tree=spec["repo"], executable=os.environ["CCCP_TEST_BINARY"], scratch=str(root),
               kind="windows-local" if spec["box"]["kind"] == "local" else spec["box"]["kind"])
    write_json(preflight_root/"payload.json", dict(box=box, specs=[]))
    if "pool_peer" in preflight_payload.__code__.co_varnames:
        preflight_payload(preflight_root/"payload.json", pool_peer=dict(case_id=spec.get("case_id"), peer_id=role))
    else:
        preflight_payload(preflight_root/"payload.json")
    identity = read_json(preflight_root/"preflight.json")
    if identity["executable_sha256"] != spec["executable_sha256"]:
        raise SpreadRefusal("native executable hash differs from preparation")
    # Retain complete hash evidence while publishing a small live identity receipt.
    identity = {key: identity[key] for key in ("machine_id", "hostname", "os", "head", "executable_sha256")}
    runtime_files = {}
    for relative, encoded in spec["files"].items():
        data = base64.b64decode(encoded, validate=True)
        target = root/safe_relative(relative)
        if target.is_relative_to(out/"runtime"):
            runtime_files[target.relative_to(out/"runtime")] = data
        else:
            atomic_bytes(target, data)
    install_memory_guard()
    environment = dict(spec["env"], CCCP_HEADLESS="1", CC_RUNNER_IGNORE_FULLSCREEN="1")
    if "-record-video" in spec["args"]:
        from e2e_video import find_ffmpeg, encoder_codec
        encoder = find_ffmpeg()
        if encoder:
            environment.update(CCCP_TEST_RECORD_ENCODER=str(encoder), CCCP_TEST_RECORD_CODEC=encoder_codec(encoder))
    retained = None
    if spec.get("retained_runtime") or any(path.parts[0] == "Data" for path in runtime_files):
        retained = native_retained_runtime(spec["repo"], out, runtime_files, spec["fixtures"])
    run = make_run(spec["repo"], spec["args"], out, timeout=spec["timeout"], env=environment,
                   expected=spec["expected"], fixtures=spec["fixtures"], runtime=retained)
    if retained is not None:
        link_directory(retained, out / "runtime")
    else:
        for relative, data in runtime_files.items():
            atomic_bytes(Path(run.cwd)/relative, data)
    if spec["directory"]:
        settings = dict(SessionDirectoryUrl=f"127.0.0.1:{spec['signal_port']}", SessionDirectoryCertSha256=spec["directory"]["DIRECTORY_PIN"])
        if not spec["directory"].get("preserve_settings"):
            settings.update(SessionDirectoryInstallKey="spread-" + role + "-install", NetworkIceEnable="1", NetworkPortMapEnable="0")
        seed_settings(run, settings)
    for flag in ("-record-video", "-feel-measure"):
        if flag in run.argv:
            Path(run.argv[run.argv.index(flag) + 1]).mkdir(parents=True, exist_ok=True)
    action_receipt, archive_receipt = {}, None
    record = {}
    last_progress, action_token = 0, None
    blockers = []
    try:
        for port in spec.get("block_udp", ()):
            blocker = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            blockers.append(blocker)
            if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
                blocker.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            blocker.bind(("", port))
        # Preserve process ownership through the installed scope on older runners.
        hooked = hasattr(sys.modules[run.__class__.__module__], "launch_scope")
        with contextlib.nullcontext() if hooked else pool_run.launch_scope(run.argv, run.env) as scope:
            run.start()
            if not hooked:
                pool_run.record_launch(scope, run.record["pid"], run.record)
                run._save()
        refuse_mixed_build(dict(executable_sha256=spec["executable_sha256"]), box, dict(peer=role), run)
        deadline = time.monotonic() + spec["timeout"]
        while run.poll() is None:
            inbox = read_json(control/"signals.json", {})
            publish_signals(root, inbox, set(spec["signals"]))
            action = read_json(control/"action.json", {})
            if action and action["token"] != action_token:
                action_token = action["token"]
                action_receipt = dict(token=action_token, action=action["action"], box=box["name"], pid=run.record["pid"])
                try:
                    if action["action"] in ("suspend", "resume"):
                        if sys.platform != "win32":
                            raise RuntimeError("Windows process suspension is unavailable")
                        call = getattr(ctypes.WinDLL("ntdll"), "NtSuspendProcess" if action["action"] == "suspend" else "NtResumeProcess")
                        status = call(ctypes.c_void_p(run.process))
                        if status:
                            raise RuntimeError(f"{action['action']} failed with native status {status}")
                    elif action["action"] == "terminate":
                        run.terminate(code=action.get("code", 137), reason=action.get("reason", "scenario drop"))
                    else:
                        raise ValueError("unknown peer lever")
                except Exception as error:
                    action_receipt["error"] = str(error)
            if time.monotonic() >= deadline:
                run.terminate(reason="engine runner timeout")
                run.record["timed_out"] = True
            if "-record-video" in run.argv:
                from e2e_video import gameplay_signals
                gameplay_signals(Path(run.argv[run.argv.index("-record-video") + 1]), root/(output_name + "-stage"))
            if time.monotonic() - last_progress >= .25:
                last_progress = time.monotonic()
                write_json(control/"progress.json", dict(identity=identity, pid=run.record["pid"], action=action_receipt,
                                                        packed_files=packed_snapshot(root, out, spec["signals"])))
            time.sleep(.05)
        record = run.finish()
    finally:
        run.close()
        for blocker in blockers:
            blocker.close()
    record.update(topology="spread", box=box["name"])
    write_json(out/"record.json", record)
    # Export writable evidence through the existing verified acceptance archive.
    for directory, names, files in os.walk(run.cwd, followlinks=False):
        names[:] = [name for name in names if name not in ("Data", "Temp", ".git") and public_file(Path(directory)/name)]
        for name in files:
            source = Path(directory)/name
            if public_file(source):
                target = out/"runtime-evidence"/source.relative_to(run.cwd)
                atomic_bytes(target, source.read_bytes())
    # Limit each archive to the peer's outputs and public shared staging.
    archive_root = root/"exports"/role
    archive_root.mkdir(parents=True, exist_ok=True)
    candidates = [out, root/(output_name + "-stage"), root/(output_name + "-probe"), root/(output_name + "_probe"), preflight_root]
    for candidate in candidates:
        if not candidate.is_dir():
            continue
        for directory, names, files in os.walk(candidate, followlinks=False):
            names[:] = [name for name in names if name != "runtime" and public_file(Path(directory)/name)]
            for name in files:
                source = Path(directory)/name
                if public_file(source):
                    atomic_bytes(archive_root/source.relative_to(root), source.read_bytes())
    for path in root.iterdir():
        if path.is_file() and public_file(path) and path.name not in ("key.pem", "cert.pem"):
            atomic_bytes(archive_root/path.name, path.read_bytes())
    archive = control/"peer-evidence.tar"
    files = pack_evidence(archive_root, archive)
    archive_receipt = dict(path=archive.as_posix(), sha256=file_sha256(archive), files=files)
    write_json(result_out/"peer-result.json", dict(topology="spread", peer=role, box=box["name"], record=record,
                                               executable_sha256=spec["executable_sha256"], archive=archive_receipt))
    write_json(control/"progress.json", dict(identity=identity, pid=run.record["pid"], action=action_receipt, record=record,
                                            archive=archive_receipt, packed_files=packed_snapshot(root, out, spec["signals"])))
    return 0 if record.get("exit_code") == 0 and not record.get("timed_out") else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    options = parser.parse_args(argv)
    try:
        return native_execute(options.native, options.out)
    except Exception as error:
        spec = read_json(options.native, {})
        record = dict(topology="spread", box=spec.get("box", {}).get("name"), error=f"{type(error).__name__}: {error}")
        write_json(options.out/"peer-result.json", record)
        write_json(options.native.parent/"progress.json", dict(record=record))
        raise


if __name__ == "__main__":
    raise SystemExit(main())
