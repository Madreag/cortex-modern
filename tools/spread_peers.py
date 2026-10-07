"""One lead-routed execution interface for real-network cases with native peers.

Contract (version 3.2, compatible with every published named-box call)
--------------------
run_case(repo, out, peers, match, *, drive=None, peer_boxes=None,
         dispatcher=None, registry=None) -> dict

``peers`` is a sequence of Peer objects. Each declares its name, OS, engine
count (one), required free memory, display size, quiet/exclusive requirement,
and whether its screen is reviewed. Peer.share_ok defaults to True for screen
peers; reviewed, held and recorder peers cannot share their box with another
peer of this case. Quiet peers reserve the whole box alone. Distinct cases may
use the same named box through ordinary slots, within native capacity guards.
Peer.held declares any target of a hold or stall lever before allocation.
Peer.recorder requires the named Windows runner's private desktop. A remote
Windows session task runs that same recorder and returns its evidence through
the existing collector. Recorder peers remain isolated within their case.
Peer.readback permits the named reviewed screen's native readback.
Every peer must be named in peer_boxes, by actual name or host/seatN alias.
A value may contain the lead's first and second named boxes (BOX|SECOND).
Only those candidates are tried, in that order; the next is tried only after
NO or the first candidate's runner-wait expires. Input/hash/root/port conflicts
remain case defects and never trigger an alternative. Sharing, native OS and
reviewed-recorder policy are checked on the actual assignment. Receipts retain
requested_peer_boxes and route_attempts alongside the actual peer_boxes.
The lead supplies those boxes; this interface never chooses an unnamed box.
Peer.task_slot, Match.parameters["peer_task_slots"], or --peer-task-slot PEER=N
may pin a registered Windows payload slot named by the lead. No slot is claimed
through preparation; its ownership is checked before the payload is changed.
Extra assignments for another arm are unused, as in the published interface.
The case supplies arguments, environment and fixtures through Peer or by calling
case.make_run() in ``drive(case)``. ``match`` is a Match with the game's port,
the lane's directory port, and optional unchanged case parameters. The helper
freezes committed inputs, verifies the
shipped/native executable hash, and starts native runners/tasks. ``drive``
still stages the case's scripts, orders starts and applies its own assertions.
Case.stage_root(out) validates the live owned controller marker before a driver
stages in that already created root; a foreign or closed claim refuses.
Without ``drive``, the call starts host first and finishes every declared peer.
It collects verified evidence into ``out`` and returns topology="spread",
peer_boxes, native identities, executable hashes, records and driver_result.
Shareable peers use one box only when the lead explicitly names it twice.
Quiet peers remain alone; reviewed and held peers remain isolated in their case.
Distinct cases require distinct live result roots and game ports. A matching
executable path does not exclude another case. Existing per-box runner capacity,
memory, CPU and ownership checks remain; no pool queue, admission lock,
candidate ranking or priority arbitration is consulted.

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
Selected retained data files stay private. Inside a modified native .rte module,
directories are real and unchanged files use regular file links or copies;
no directory symlink is placed inside the engine's content identity walk.
Both platforms retain identical complete module contents. Aliases for unmodified
module roots and default single-box staging stay unchanged.

Refusals raise SpreadRefusal and write spread-result.json with topology,
peer_boxes, refused_peer, refused_box and the exact reason. Prefixes are
"spread peer <name> on <box>: <native reason>",
"match port <port> differs from host port <port>", "native executable hash
differs from preparation", and "Windows process suspension is unavailable".
Missing peer names and retired --spread exit 2 with
"NO BOX NAMED: the lead routes every peer (ROUTING.md section 6)".
Forbidden explicit sharing refuses "TWO PEERS ON ONE BOX WITHOUT share_ok".
Live result-root and port conflicts refuse "RUN ROOT CONFLICT" and
"PORT CONFLICT <port>", naming the box and conflicting peer/case. Native
ownership markers use the existing facts writer and capacity mutex; no queue
or placement layer is added. INTERFACE_VERSION is recorded in each result.
Exhausted named candidates return their boxes and exact reasons to the lead.
No case assertion, oracle, timeout or default single-box launch is changed.
Peer.output_name optionally declares an existing non-ASCII output directory;
make_run(..., role=...) also accepts its declared logical peer explicitly.
Text scripts retain their original UTF-8 or legacy Windows byte encoding.
Named line scripts (-menu-script, -input-script, -net-chat-script and
CCCP_TEST_SCREEN_WATCHES) staged for POSIX use LF terminators, so Windows
CRLF never becomes part of a native command's text. Binary/replay inputs and
immutable module content are unchanged; single-box staging is unchanged.
Peer.lane (or Match.parameters["lane"]) supplies the caller's run label.
Match.parameters["label"] may specify the lead's exact native holder label.
Match.parameters["runner_wait"] or --runner-wait may specify that holder's
wait in seconds. It updates only the existing local holder command, without
an enclosing holder or a second slot. Engine/script timeouts remain unchanged.
Admission uses one runner-wait budget on each named box, shared across probes,
shipment and launch. NO is limited to off-limits, incompatible OS, physical
engine ceiling, or physical free memory below the native required floor. Other
admission conditions wait: FIFO/start spacing, exclusive holders (including an
ownerless marker, preserved for the lead), capacity mutexes, CPU, SSH, payload
slots, builds and quiet-box availability. Every retry checks the original
native guard, renews this peer's ownership and releases locks between probes.
Quiet peers launch only alone and below the strict CPU guard.
Timing suitability in the catalog is historical evidence; an explicitly
named quiet route retains the strict native guard and the caller's timing
verdict. It does not change those measurements or their required bounds.
Preparation claims retain root/control-port ownership but consume no engine share, payload
slot or alone marker; those are acquired at native launch. The unchanged
native engine-start mutex checks real engines and memory atomically. Root,
port, hash and ownership conflicts remain fatal case defects. A zero wait
allows the first guard check and no transient retries. The capacity lock uses
at most 180 seconds per attempt within the remaining runner-wait budget.
Match.parameters["wait_for_holder"] / --wait-for-holder remains compatible.
Match.parameters["peer_tasks"] may pin a named Windows task for each peer.
Match.parameters["public_directory"]=True uses the game's default public
directory without a signaling tunnel. The host's registration log supplies
its session code; only deliberate loopback joins are replaced by that code.
Match.parameters["public_menu_start"]=True coordinates concurrently started
menu peers before the host starts. The joiner's code is delivered as a staged
input before its engine starts; original scripts and budgets are unchanged.
No case assertion time is added. Peer.block_udp reserves only
declared discovery ports on that peer's native machine for the case's lever.
The default network is ICE. Match.parameters["network"]="direct" preserves
explicit ICE-Off/Unlisted inputs and substitutes only deliberate loopback
join addresses with the host's existing network address. An optional
Match.parameters["host_address"] supplies that address; otherwise it is read
on the native host's route to each named peer's saved SSH endpoint. This keeps
LAN peers on the LAN and overlay peers on their existing overlay, without
changing either network. Concurrent seats wait for an already requested host
to have its native PID before launching; callers still order their starts.
The same admission barrier precedes session lookup, whose original 80-second
publication budget begins after that admission and remains unchanged.
Loopback/unspecified host addresses refuse with the box
named. Explicit ICE-Off in the default ICE mode also refuses instead of being
overwritten. Scratch placement derives from the installed box catalog.
Match.parameters["directory"] may instead supply an already running caller's
DIRECTORY_URL, DIRECTORY_PIN and DIRECTORY_ROOT descriptor. The caller keeps
that service alive and owns its assertions. Optional peer_directory_urls maps
logical peers to the caller's loopback TLS proxies; only those signaling ports
are forwarded. Their staged connection settings and install keys are retained.
Set join_by_session=False when the caller joins through directory rows itself.
The byte-pinned interface is shipped as a native control input, so callers do
not need it committed into their own branch before using the published call.
Declared .bin protocol inputs in peer arguments retain their exact bytes.
make_run(..., runtime=...) copies owned retained regular files, settings and
Data overlays without following links into the immutable game tree. Native
overlay ancestors are private; other Data directories remain linked inputs.
Incomplete retained runtimes and private tickets refuse before launch.
The lead may explicitly share all eligible peers; box identities remain in
the result so the topology is reviewable. --pool-registry reads box facts only.
The dispatcher argument and --pool-dispatcher remain compatible transport-kit
location hints; their script is never executed. Existing complete peer_boxes
calls, driver staging, levers, collectors and return fields remain supported.

Native control ship set, in import order: pool_cohort.py, box_facts.py,
box_load.py, pool.py, pool_worker.py, spread_peers.py, pool_run.py. The cohort
reader comes from the facts adapter's folder or the installed transport kit.
cross_peers.py is also pinned for native preflight. The native worker extends its existing cache materializer: any exhausted or
failed file hard link falls back to a SHA-verified copy; each materialization
prunes completed per-box snapshots to the latest three. Live claims and live
preparation phases protect in-use snapshots. If more than three are live,
pruning is deferred (recorded in snapshot_prune); unknown/unmarked entries are
preserved. Reparse entries are unlinked without entering their targets.
The immutable repository
shipment includes the driver's tools and runner dependencies; its manifest
hashes every file. Each result publishes native_ship_set with control hashes.
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
    # Cache publication precedes the repository tools' native module path.
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda:stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


TOPOLOGY_LOCAL = "single-box: not proof"
INTERFACE_VERSION = "3.2-named-ranked-waits"
NO_BOX_NAMED = "NO BOX NAMED: the lead routes every peer (ROUTING.md section 6)"
_options = None
_cases = contextvars.ContextVar("spread_cases", default=None)
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)


class SpreadRefusal(RuntimeError):
    pass


class SpreadUsageError(SystemExit):
    def __init__(self, message=NO_BOX_NAMED):
        self.message = message
        print(message, file=sys.stderr, flush=True)
        super().__init__(2)

    def __str__(self):
        return self.message


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
    task_slot: int | None = None

    def __post_init__(self):
        if self.task_slot is not None and (type(self.task_slot) is not int or self.task_slot < 1):
            raise ValueError('task_slot must name a positive native session slot')
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


def native_address_probe(destination=None):
    """Read the host's existing network address without changing its network."""
    if destination:
        destination = str(ipaddress.IPv4Address(destination))
        if sys.platform == "win32":
            command = ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
                       f"Find-NetRoute -RemoteIPAddress {destination} | Where-Object {{ $_.IPAddress }} | Select-Object -ExpandProperty IPAddress"]
            result = subprocess.run(command, capture_output=True, text=True, timeout=10, creationflags=NO_WINDOW)
            candidates = result.stdout.splitlines() if result.returncode == 0 else []
        elif sys.platform.startswith("linux"):
            result = subprocess.run(["ip", "-j", "route", "get", destination], capture_output=True, text=True, timeout=10)
            candidates = [row.get("prefsrc") or row.get("src") for row in json.loads(result.stdout)] if result.returncode == 0 else []
        elif sys.platform == "darwin":
            result = subprocess.run(["/sbin/route", "-n", "get", destination], capture_output=True, text=True, timeout=10)
            interface = re.search(r"(?m)^\s*interface:\s*(\S+)", result.stdout)
            if not interface:
                raise RuntimeError("native host route to the named peer is unavailable")
            result = subprocess.run(["/usr/sbin/ipconfig", "getifaddr", interface[1]], capture_output=True, text=True, timeout=10)
            candidates = result.stdout.splitlines() if result.returncode == 0 else []
        else:
            raise RuntimeError("native route probe is unavailable on this platform")
        for value in candidates:
            if not value:
                continue
            address = ipaddress.IPv4Address(value.strip())
            if not address.is_loopback and not address.is_link_local and not address.is_unspecified:
                return str(address)
        raise RuntimeError("native host has no route address to the named peer")
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


def named_box_endpoint(box):
    """Read the named box's existing transport endpoint; never select a box."""
    if box["kind"] == "local":
        return native_address_probe("192.0.2.1")
    result = subprocess.run(["ssh", "-G", box["ssh"]], capture_output=True, text=True, timeout=10, creationflags=NO_WINDOW)
    hostname = next((line.split(None, 1)[1] for line in result.stdout.splitlines() if line.startswith("hostname ")), None)
    if result.returncode or not hostname:
        raise RuntimeError("named box's saved SSH endpoint is unavailable")
    for row in socket.getaddrinfo(hostname, None, family=socket.AF_INET):
        address = ipaddress.IPv4Address(row[4][0])
        if not address.is_loopback and not address.is_link_local and not address.is_unspecified:
            return str(address)
    raise RuntimeError("named box's SSH endpoint is not a network address")


def wait_started_host(case, role, wait=0):
    """Keep a concurrently requested seat behind its host's actual start."""
    host = getattr(case, "runs", {}).get(case.names[0])
    if role == case.names[0] or not host or not getattr(host, "launch_attempted", False):
        return
    deadline = time.monotonic() + 1320 + float(wait or 0)
    while not host.started:
        if getattr(host, "start_failure", None):
            raise SpreadRefusal("requested host did not start: " + host.start_failure)
        if time.monotonic() >= deadline:
            raise SpreadRefusal("requested host did not return within its admission wait")
        case.guard()
        time.sleep(.1)


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
    group.add_argument("--spread", action="store_true", help="retired: the lead must name every peer with --peer-boxes")
    group.add_argument("--peer-boxes", help="host=BOX|SECOND,seat2=BOX,... (only lead-named boxes; actual peer names accepted)")
    parser.add_argument("--pool-dispatcher", type=Path, help="compatible hint to the existing per-box transport kit; never executed")
    parser.add_argument("--pool-registry", type=Path, help="box facts for the lead's named peers")
    parser.add_argument("--runner-label", help="the lead's exact label for the native run holder")
    parser.add_argument("--runner-wait", type=float, help="the lead's wait in seconds for the existing local holder")
    parser.add_argument("--wait-for-holder", help="exact holder label the lead explicitly authorized waiting behind")
    parser.add_argument("--peer-task", action="append", default=[], metavar="PEER=TASK", help="the lead's exact registered Windows task for a peer")
    parser.add_argument("--public-directory", action="store_true", help="join through the game's default public directory without a private signaling tunnel")
    parser.add_argument("--peer-port", action="append", default=[], metavar="PEER=PORT", help="explicit peer match port (also supports a wrong-parameter detecting run)")
    parser.add_argument('--peer-task-slot', action='append', default=[], metavar='PEER=SLOT', help='the lead\'s registered Windows session slot')


def enabled(options=None):
    options = _options if options is None else options
    if options and getattr(options, "spread", False):
        raise SpreadUsageError()
    return bool(options and getattr(options, "peer_boxes", None) is not None)


def configure(options):
    global _options
    enabled(options)
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


def named_peer_choices(peers, values):
    """Parse only the lead's ordered named candidates; no catalog ranking."""
    names = [peer.name for peer in peers]
    assignments = {}
    for index, name in enumerate(names):
        aliases = (name.casefold(), "host" if index == 0 else f"seat{index + 1}")
        boxes = {values[key] for key in aliases if key in values}
        if not boxes:
            raise SpreadUsageError()
        if len({box.casefold() for box in boxes}) != 1:
            raise SpreadUsageError(f"conflicting boxes for peer {name}")
        choices = tuple(part.strip() for part in next(iter(boxes)).split('|'))
        if not 1 <= len(choices) <= 2 or any(not part for part in choices) or len({part.casefold() for part in choices}) != len(choices):
            raise SpreadUsageError(f'peer {name} needs one or two distinct named boxes')
        assignments[name] = choices
    return assignments


def sharing_reason(peers, assignments, name, box):
    siblings = [peer for peer in peers if assignments.get(peer.name, '').casefold() == box.casefold() and peer.name != name]
    peer = next(peer for peer in peers if peer.name == name)
    if siblings and not all(other.share_ok for other in [peer, *siblings]):
        return 'TWO PEERS ON ONE BOX WITHOUT share_ok'


def named_peer_boxes(peers, values):
    """Keep complete one-box calls unchanged; ranked candidates resolve at admission."""
    choices = named_peer_choices(peers, values)
    assignments = {name: boxes[0] for name, boxes in choices.items()}
    for peer in peers:
        box = assignments[peer.name]
        if all(len(boxes) == 1 for boxes in choices.values()) and sharing_reason(peers, assignments, peer.name, box):
            error = SpreadRefusal(f"spread peer {peer.name} on {box}: TWO PEERS ON ONE BOX WITHOUT share_ok")
            error.peer, error.box, error.reason = peer.name, box, "TWO PEERS ON ONE BOX WITHOUT share_ok"
            raise error
    return assignments


def installed_pool(dispatcher=None, registry=None):
    """Locate the existing transport and catalog without running placement."""
    path = dispatcher or os.environ.get("CORTEX_POOL_DISPATCHER")
    if not path and registry and Path(registry).with_name("pool_transport.py").is_file():
        path = Path(registry).with_name("pool_transport.py")
    if not path:
        try:
            import box_facts
            path = box_facts.pool_dispatcher()
        except ImportError:
            config = read_json(os.environ.get("CORTEX_BOXES", Path.home()/".cortex-modern/boxes.json"), {})
            path = config.get("pool_dispatcher") or config.get("tool_paths", {}).get("pool_dispatcher")
    if not path or not Path(path).resolve().parent.joinpath("pool_transport.py").is_file():
        raise SpreadRefusal("named peers require the existing per-box transport kit; set --pool-registry or --pool-dispatcher")
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
                path.suffix.lower() in (".ticket", ".key") or
                path.name in (".env", "key.pem", ".spread-case-owner.json", ".spread-run-owner.json") or
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


def line_script_inputs(args, environment):
    """Only files consumed by the native line-oriented command parsers."""
    paths = {Path(args[index + 1]).resolve() for index, argument in enumerate(args[:-1])
             if argument in ('-menu-script', '-input-script', '-net-chat-script')}
    if environment.get('CCCP_TEST_SCREEN_WATCHES'):
        paths.add(Path(environment['CCCP_TEST_SCREEN_WATCHES']).resolve())
    return paths


def native_line_script(data, path, box, inputs):
    """Preserve bytes except CRLF terminators in a declared POSIX line script."""
    path = Path(path)
    module_content = any(part.casefold().endswith('.rte') for part in path.parts)
    if path.resolve() in inputs and box['os'] != 'windows' and not module_content and path.suffix.lower() not in ('.bin', '.ccreplay'):
        return data.replace(b'\r\n', b'\n')
    return data


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
    if not force and peer_boxes is None and not enabled():
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


def launch_native(backend, box, claim, request, wait=0):
    """Keep the installed transport; pass a lead-specified wait to its holder."""
    import math
    wait = float(wait or 0)
    if not math.isfinite(wait) or wait < 0:
        raise ValueError("runner wait must be finite and nonnegative")
    if not wait or box["kind"] != "local":
        return backend.launch(box, claim, request)
    original = backend.rpc
    def rpc(target, action, body, **kwargs):
        if action == "write" and body.get("path") == claim["root"] + "/request.json":
            value = body["value"]
            if value.get("claim", {}).get("token") != claim["token"]:
                raise SpreadRefusal("native holder request changed owner")
            argv = list(value["argv"])
            holder = next((i for i, item in enumerate(argv) if Path(item).name == "box_hold.py"), None)
            if holder is None or "--wait" not in argv[holder:]:
                raise SpreadRefusal("existing native holder has no wait argument")
            index = argv.index("--wait", holder)
            argv[index+1] = f"{wait:g}"
            body = {**body, "value": {**value, "argv": argv}}
        return original(target, action, body, **kwargs)
    backend.rpc = rpc
    try:
        return backend.launch(box, claim, request)
    finally:
        backend.rpc = original


class AdmissionExpired(SpreadRefusal):
    """The named candidate exhausted its caller-supplied admission budget."""


def hard_admission_refusal(reason):
    """NO means that the named machine cannot run the peer now."""
    text = str(reason).casefold()
    return any(value in text for value in (
        'off limits', 'below floor', 'operating system does not fit',
        'live engine capacity is in use', 'engines at its ceiling',
    )) or bool(re.search(r'needs \d+ engines; capacity is \d+', text)
               or re.search(r'free memory .*\bneeds \d+(?:\.\d+)? gb', text))


def transient_admission(reason):
    """Keep input/identity defects fatal; every other admission precondition waits."""
    text = str(reason).casefold()
    if hard_admission_refusal(text):
        return False
    if any(value in text for value in ('run root conflict', 'port conflict', 'hash differs',
                                      'owner changed', 'lost its capacity claim', 'changed owner',
                                      'artifact changed', 'snapshot differs', 'match port ')):
        return False
    return any(value in text for value in (
        'capacity refused:', 'capacity changed before launch:', 'capacity update is busy',
        'capacity lock ', 'another run holds the box alone', 'alone run needs an idle box',
        'earlier work request is waiting:', 'start spacing', 'box launch refused',
        'ownerless exclusive marker', 'ssh does not answer', 'ssh failed', 'connection timed out',
        'connection refused', 'connection reset', 'box does not answer', 'probe failed:',
        'box answered late', 'box returned no response', 'native operation answers late',
        'box returned an unreadable response', 'saved ssh endpoint is unavailable',
        'native engine admission is busy', 'native load check failed',
        'payload slots are busy', 'slot runs another payload', 'payload-submit.lock',
        'a build is running', 'compiler shares are in use', 'test shares are in use',
        'cpu ', 'display probe failed', 'live display is not measured',
        'requested ',
    ))


class AdmissionWait:
    """One deadline for every transient on one lead-named candidate, before its timer."""
    def __init__(self, box, needs, wait=0, *, renew=None):
        import math
        self.wait = float(wait or 0)
        if not math.isfinite(self.wait) or self.wait < 0:
            raise ValueError('runner wait must be finite and nonnegative')
        self.box, self.needs, self.renew = box, needs, renew
        self.deadline = time.monotonic() + self.wait
        self.probed_at, self.announced = None, None

    def remaining(self):
        return max(0, self.deadline-time.monotonic())

    def probe_started(self):
        self.probed_at = time.monotonic()

    def accepts(self, reason, state=None):
        return transient_admission(reason)

    def pause(self, reason):
        remaining = self.remaining()
        if not self.wait:
            raise AdmissionExpired(f"{self.box['name']}; peer {self.needs.peer_id}; "
                                   f"admission wait expired after 0s: {reason}")
        if remaining <= 0:
            raise AdmissionExpired(f"{self.box['name']}; peer {self.needs.peer_id}; "
                                   f"admission wait expired after {self.wait:g}s: {reason}")
        if str(reason) != self.announced:
            print(f"WAITING NAMED: {self.box['name']}; peer {self.needs.peer_id}; {reason}", flush=True)
            self.announced = str(reason)
        if self.renew:
            self.renew()
        interval = 30 if 'cpu ' in str(reason).casefold() else 2
        next_probe = (self.probed_at if self.probed_at is not None else time.monotonic()) + interval
        time.sleep(min(remaining, max(0, next_probe-time.monotonic())))
        self.probed_at = None

    def call(self, operation):
        while True:
            self.probe_started()
            try:
                return operation()
            except AdmissionExpired:
                raise
            except (RuntimeError, subprocess.TimeoutExpired) as error:
                if not isinstance(error, subprocess.TimeoutExpired) and not self.accepts(error):
                    raise
                self.pause(str(error))


class NamedCpuWait(AdmissionWait):
    """Compatible name; CPU waits now use the same runner-wait budget."""


def real_engine_capacity(raw):
    """A preparation claim retains its root/ports, but consumes no engine share."""
    if 'actual_engines' not in raw:
        return raw
    state = dict(raw)
    state['engines'] = state['slots_in_use'] = raw['actual_engines']
    state['builds_in_use'] = raw.get('actual_builds', raw.get('builds_in_use', 0))
    state['jobs'] = []
    for job in raw.get('jobs', []):
        value = dict(job)
        value['preparing'] = job.get('preparing', not bool(job.get('started_engines')))
        if value['preparing']:
            value['alone'] = False
            # This suppresses the kit's reservation-memory deduction. Every
            # actual launch still probes physical memory under its native lock.
            value['started_engines'] = value.get('engines', 0)
        state['jobs'].append(value)
    return state


def named_live_reason(original, box, needs, state):
    state = real_engine_capacity(state)
    state = dict(state, jobs=[job for job in state.get('jobs', []) if not job.get('preparing')])
    if state.get('present'):
        if 'engines_max' in box and getattr(needs, 'engines', 0) + state.get('engines', 0) > box['engines_max']:
            return 'live engine capacity is in use'
        if 'free_floor_gb' in box:
            required = max(box['free_floor_gb'], getattr(needs, 'memory', 0),
                           getattr(needs, 'engines', 0)*box.get('engine_peak_gb', 3.4)+.5)
            if state.get('free_gb', 0) < required:
                return f"free memory {state.get('free_gb', 0):.1f} GB; needs {required:.1f} GB"
    # A compilation is transient, even when it has not spawned an engine.
    if state.get('builds_in_use'):
        return original(box, needs, state) or 'a build is running on the box'
    return original(box, needs, state)


def exclusive_owner(path, value, facts):
    live = bool(value and not getattr(value, 'bare', False) and value.get('pid')
                and value.get('process_start') is not None and value.get('machine'))
    if live and str(value['machine']).casefold() == facts.machine_name().casefold():
        live = facts.process_start(value['pid']) == value['process_start']
    return dict(path=str(path), live=live, **({key:value.get(key) for key in
                ('pid', 'process_start', 'machine', 'label', 'token')} if value else {}))


def native_exclusive_state(probe, box, *, facts, root, **kwargs):
    """Attach read-only exclusive ownership evidence to the native facts probe."""
    holders, ownerless = [], []
    ignore = kwargs.get('ignore_token')
    markers = []
    for kind in ('timing', 'acceptance', 'verification', 'battery', 'exclusive', 'cross-free'):
        paths = box.get('markers', {}).get(kind, [])
        markers.extend(Path(path) for path in ([paths] if isinstance(paths, str) else paths))
    paths = [(path, True) for path in markers]
    for path, marker in paths:
        value = facts.read_reservation(path, archive=False)
        if value and ignore and value.get('token') == ignore:
            continue
        if not marker and not (value and value.get('alone')):
            continue
        if not path.exists():
            continue
        holder = exclusive_owner(path, value, facts)
        holders.append(holder)
        if not holder['live']:
            ownerless.append(f"ownerless exclusive marker {path}; no live recorded owner")
    # An ownerless marker is evidence for the lead; the facts probe must not
    # archive it as a stale reservation. Other capacity checks stay unchanged.
    kwargs = dict(kwargs, read_only=True)
    state = probe(box, **kwargs)
    state['exclusive_holders'] = holders
    if ownerless:
        state['refusal'] = '; '.join(ownerless)
    return real_engine_capacity(state)


class NamedHolderWait(AdmissionWait):
    """Compatible name for the common admission path."""


def claim_native_exclusive_marker(box, claim, marker, label, *, facts, renew):
    """Wait for a POSIX quiet holder before publishing this peer's own marker."""
    from types import SimpleNamespace
    wait = AdmissionWait(box, SimpleNamespace(peer_id=claim['needs']['peer_id']),
                         admission_seconds(claim))
    while True:
        existing = facts.read_reservation(marker, archive=False)
        if not Path(marker).exists():
            try:
                facts.write_reservation(marker, label, token=claim['token'])
            except FileExistsError:
                continue  # the atomic writer lost to a new holder; check its owner
            return
        owner = exclusive_owner(marker, existing, facts)
        if existing and existing.get('token') == claim['token']:
            return
        reason = (f"box launch refused: {marker}; owner={owner.get('label')} "
                  f"(pid={owner.get('pid')}, machine={owner.get('machine')})" if owner['live'] else
                  f'ownerless exclusive marker {marker}; no live recorded owner')
        wait.pause(reason)
        renew(claim)


def capacity_lock_seconds(wait, remaining=None):
    import math
    wait = float(wait or 0)
    if not math.isfinite(wait) or wait < 0:
        raise ValueError('runner wait must be finite and nonnegative')
    if not wait:
        return 0.0
    if remaining is not None:
        remaining = float(remaining)
        if not math.isfinite(remaining):
            raise ValueError('remaining runner wait must be finite')
        wait = min(wait, max(0, remaining))
    return min(180.0, wait)


@contextlib.contextmanager
def named_capacity_mutex(path, *, mutex, box, peer, wait=0, remaining=None):
    """Use the native capacity lock with the lead's bounded contention wait."""
    from types import SimpleNamespace
    retry = AdmissionWait(box, SimpleNamespace(peer_id=peer), wait)
    if remaining is not None:
        retry.deadline = min(retry.deadline, time.monotonic()+max(0, remaining))
    while True:
        budget = capacity_lock_seconds(wait, retry.remaining())
        stack = contextlib.ExitStack()
        try:
            stack.enter_context(mutex(path, wait=budget))
            break
        except RuntimeError as error:
            stack.close()
            if str(error) != 'capacity update is busy; skip this box':
                raise
            retry.pause(f"capacity lock {path}; {error}")
    with stack:
        yield


@contextlib.contextmanager
def named_launch_capacity(box, needs, claim, *, probe, live_reason, mutex, root, renew):
    """Keep the native final guard and release its lock while waiting to retry."""
    retry = AdmissionWait(box, needs, admission_seconds(claim))
    while True:
        with named_capacity_mutex(root/'.capacity.lock', mutex=mutex, box=box, peer=needs.peer_id,
                                  wait=retry.wait, remaining=retry.remaining()):
            retry.probe_started()
            state = probe(box, refresh_display=False, ignore_token=claim["token"])
            reason = named_live_reason(live_reason, box, needs, state)
            if not reason:
                yield state
                return
            if not retry.accepts(reason):
                raise RuntimeError("capacity changed before launch: " + reason)
        retry.pause(reason)
        renew(claim)


def native_cpu_wait_source(source):
    """Extend this case's hashed existing worker kit; never edit its global copy."""
    guard = ("            with mutex(root_for(box)/'.capacity.lock',wait=15):\n"
             "                before=capacity_state(box,refresh_display=False,ignore_token=claim['token'])\n"
             "                if reason:=live_reason(box,Needs(**claim['needs']),before):raise RuntimeError('capacity changed before launch: '+reason)\n")
    if source.count(guard) != 1:
        raise SpreadRefusal("native worker capacity guard differs from the supported kit")
    claim_guard = "    with mutex(root/'.capacity.lock',wait=15):\n"
    if 'def capacity_claim(' in source and source.count(claim_guard) != 1:
        raise SpreadRefusal('native worker capacity claim guard differs from the supported kit')
    source = source.replace(claim_guard,
                "    from spread_peers import named_capacity_mutex\n"
                "    with named_capacity_mutex(root/'.capacity.lock',mutex=mutex,box=box,peer=needs['peer_id'],\n"
                "            wait=request.get('runner_wait',0),remaining=request.get('capacity_wait_remaining')):\n", 1)
    slot_guard = ("        slot=None\n"
                  "        if needs['os']!='compiler' and box.get('kind')=='windows-task' and state.get('slots'):\n"
                  "            occupied={job.get('slot') for job in state['jobs']}\n"
                  "            chosen=next(row for row in state['slots'] if row['state']=='Ready' and row['slot'] not in occupied)\n"
                  "            slot=chosen['slot']\n")
    if slot_guard in source:
        source = source.replace(slot_guard, "        slot=None\n", 1)
        source = source.replace("slot_marker=chosen['owner_marker'] if slot is not None else None", "slot_marker=None", 1)
        source = source.replace("alone_marker=box.get('markers',{}).get('timing') if needs['alone'] and box['kind']!='local' else None",
                                "alone_marker=None", 1)
    source = source.replace("builds_in_use=sum(bool(job.get('compiler')) for job in jobs)+foreign_builds,",
                            "actual_builds=len(builds),builds_in_use=len(builds),", 1)
    marker_guard = ("            existing=facts.read_reservation(marker)\n"
                    "            if not existing:facts.write_reservation(marker,request['label'],token=claim['token'])\n"
                    "            elif existing.get('token')!=claim['token']:raise RuntimeError('another owner reserves this box alone')\n")
    if 'another owner reserves this box alone' in source and source.count(marker_guard) != 1:
        raise SpreadRefusal('native worker exclusive marker guard differs from the supported kit')
    if source.count(marker_guard) == 1:
        source = source.replace(marker_guard,
                 "            from spread_peers import claim_native_exclusive_marker\n"
                 "            claim_native_exclusive_marker(box,claim,marker,request['label'],facts=facts,renew=renew_claim)\n", 1)
    replacement = ("            from spread_peers import named_launch_capacity\n"
                   "            with named_launch_capacity(box,Needs(**claim['needs']),claim,probe=capacity_state,\n"
                   "                    live_reason=live_reason,mutex=mutex,root=root_for(box),renew=renew_claim) as before:\n"
                   "                free=before['free_gb']\n")
    entry = "if __name__=='__main__':raise SystemExit(main())"
    if source.count(entry) != 1:
        raise SpreadRefusal('native worker entry point differs from the supported kit')
    extension = ("\n_native_capacity_state = capacity_state\n"
                 "def capacity_state(box, read_only=False, refresh_display=True, ignore_token=None):\n"
                 "    from spread_peers import native_exclusive_state\n"
                 "    return native_exclusive_state(_native_capacity_state,box,facts=facts,root=root_for(box),\n"
                 "            read_only=read_only,refresh_display=refresh_display,ignore_token=ignore_token)\n")
    if 'def capacity_claim(' in source and 'facts.write_reservation(marker' in source:
        extension += ("\n_native_capacity_claim = capacity_claim\n"
                      "def capacity_claim(box,needs,request):\n"
                      "    from spread_peers import existing_native_claim\n"
                      "    return existing_native_claim(box,needs,request,facts,root_for(box)) or _native_capacity_claim(box,needs,request)\n")
    if 'def submit_task(' in source:
        extension += ("\n_native_submit_task = submit_task\n"
                      "def submit_task(value):\n"
                      "    from spread_peers import submit_named_task\n"
                      "    return submit_named_task(_native_submit_task,value,globals())\n")
    source = source.replace(guard, replacement, 1).replace(entry, extension+'\n'+entry, 1)
    return native_cache_source(source)


def native_cache_source(source):
    """Extend only the existing worker's artifact operations, not its facts writer."""
    import ast
    changes = []
    lines = source.splitlines(True)
    offsets = [0]
    for line in lines:
        offsets.append(offsets[-1]+len(line))
    for function in ast.parse(source).body:
        if not isinstance(function, ast.FunctionDef) or function.name not in ('publish', 'ingest', '_materialize'):
            continue
        for node in ast.walk(function):
            if (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                    and isinstance(node.func.value, ast.Name) and node.func.value.id == 'os' and node.func.attr == 'link'):
                call = ast.get_source_segment(source, node)
                changes.append((offsets[node.lineno-1]+node.col_offset,
                                offsets[node.end_lineno-1]+node.end_col_offset,
                                'cache_link_or_copy('+call[len('os.link('):-1]+',expected)'))
    for start, end, replacement in sorted(changes, reverse=True):
        source = source[:start]+replacement+source[end:]
    entry = "if __name__=='__main__':raise SystemExit(main())"
    extension = ''
    if changes:
        extension += ("\ndef cache_link_or_copy(source,target,expected):\n"
                      "    from spread_peers import cache_link_or_copy as verified_link\n"
                      "    return verified_link(source,target,expected)\n")
    if 'def materialize(' in source:
        extension += ("\ndef materialize(value):\n"
                      "    from spread_peers import materialize_cached_snapshot\n"
                      "    return materialize_cached_snapshot(_materialize,value,globals())\n")
    return source.replace(entry, extension+'\n'+entry, 1)


def cache_link_or_copy(source, target, expected):
    """A failed or exhausted hard link becomes a byte-verified regular copy."""
    import tempfile
    source, target = Path(source), Path(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.link(source, target)
        return
    except FileExistsError:
        if target.is_symlink() or file_sha256(target) != expected:
            raise SpreadRefusal(f'immutable cache artifact differs: {target}')
        return
    except OSError:
        if target.exists() or target.is_symlink():
            if target.is_symlink() or not target.is_file() or file_sha256(target) != expected:
                raise SpreadRefusal(f'immutable cache artifact differs: {target}')
            return
    descriptor, name = tempfile.mkstemp(prefix=target.name+'.copy-', dir=target.parent)
    staging = Path(name)
    try:
        with os.fdopen(descriptor, 'wb') as output, source.open('rb') as original:
            shutil.copyfileobj(original, output)
            output.flush()
            os.fsync(output.fileno())
        if file_sha256(staging) != expected:
            raise SpreadRefusal(f'cache copy hash differs: {target}')
        shutil.copymode(source, staging)
        if target.exists() and file_sha256(target) != expected:
            raise SpreadRefusal(f'immutable cache artifact differs: {target}')
        try:
            os.rename(staging, target)
        except FileExistsError:
            if file_sha256(target) != expected:
                raise SpreadRefusal(f'concurrent cache artifact differs: {target}')
        if file_sha256(target) != expected:
            raise SpreadRefusal(f'cache publication hash differs: {target}')
    finally:
        staging.unlink(missing_ok=True)


def unlink_readonly_cache_file(path):
    """Remove this Windows link without changing shared file attributes."""
    from ctypes import wintypes
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    create = kernel.CreateFileW
    create.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
                       wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    create.restype = wintypes.HANDLE
    dispose = kernel.SetFileInformationByHandle
    dispose.argtypes = [wintypes.HANDLE, ctypes.c_int, wintypes.LPVOID, wintypes.DWORD]
    dispose.restype = wintypes.BOOL
    close = kernel.CloseHandle
    close.argtypes, close.restype = [wintypes.HANDLE], wintypes.BOOL
    native = str(Path(path).absolute())
    if not native.startswith('\\\\?\\'):
        native = '\\\\?\\UNC\\'+native[2:] if native.startswith('\\\\') else '\\\\?\\'+native
    handle = create(native, 0x00010000, 7, None, 3, 0x00200000, None)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        # FileDispositionInfoEx: DELETE | POSIX_SEMANTICS | IGNORE_READONLY.
        # https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/2e860264-018a-47b3-8555-565a13b35a45
        flags = wintypes.DWORD(0x01 | 0x02 | 0x10)
        if not dispose(handle, 21, ctypes.byref(flags), ctypes.sizeof(flags)):
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        close(handle)


def remove_snapshot_tree(path, snapshots):
    """Unlink reparse entries; never traverse an immutable input's link target."""
    import stat
    path, snapshots = Path(path), Path(snapshots).resolve()
    if path.parent.resolve() != snapshots or path.resolve().parent != snapshots:
        raise SpreadRefusal(f'cache prune target escapes snapshots: {path}')
    def remove(entry):
        info = entry.lstat()
        linked = stat.S_ISLNK(info.st_mode) or bool(getattr(info, 'st_file_attributes', 0) & 1024)
        if linked:
            if stat.S_ISDIR(info.st_mode) and not entry.is_symlink():
                entry.rmdir()
            else:
                entry.unlink()
        elif stat.S_ISDIR(info.st_mode):
            for child in entry.iterdir():
                remove(child)
            entry.rmdir()
        else:
            try:
                entry.unlink()
            except PermissionError:
                if sys.platform != 'win32' or not (getattr(info, 'st_file_attributes', 0) & stat.FILE_ATTRIBUTE_READONLY):
                    raise
                unlink_readonly_cache_file(entry)
    remove(path)


def prune_snapshots(root, current=None, *, protected=()):
    """Keep the latest three completed snapshots, all live ones, and unknown entries."""
    root = Path(root)
    result = dict(removed=[], deferred_active=[], unmarked=[])
    if not root.exists():
        return result
    if root.is_symlink() or getattr(root, 'is_junction', lambda:False)():
        raise SpreadRefusal(f'cache snapshots root is a directory link: {root}')
    protected = {Path(path).resolve() for path in protected}
    if current is not None:
        protected.add(Path(current).resolve())
    candidates = []
    for path in root.iterdir():
        if not path.is_dir() or path.is_symlink() or getattr(path, 'is_junction', lambda:False)():
            continue
        receipt = path/'pool-inputs.json'
        if receipt.is_symlink():
            result['unmarked'].append(path.name)
            continue
        record = read_json(receipt, {})
        if not record or not isinstance(record.get('manifest'), dict) or not record.get('head'):
            result['unmarked'].append(path.name)
            continue
        candidates.append((receipt.stat().st_mtime_ns, path.name, path))
    latest = {path.resolve() for _, _, path in sorted(candidates, reverse=True)[:3]}
    for _, _, path in sorted(candidates):
        if path.resolve() in latest:
            continue
        if path.resolve() in protected:
            result['deferred_active'].append(path.name)
            continue
        remove_snapshot_tree(path, root)
        result['removed'].append(path.name)
    return result


def live_snapshot_paths(box, worker):
    """Read claims and live preparation phases without modifying their owners."""
    root, facts = worker['root_for'](box), worker['facts']
    runs = set()
    for path in (root/'claims').glob('run-*.json'):
        owner = facts.read_reservation(path, archive=False)
        if owner and owner.get('run_id'):
            runs.add(root/'runs'/owner['run_id'])
    for path in (root/'runs').glob('*/phases/phase-*.json'):
        if facts.read_reservation(path, archive=False):
            runs.add(path.parent.parent)
    paths = []
    for run in runs:
        for name in ('snapshot-use.json', 'request.json', 'assignment.json', 'build-request.json'):
            record = read_json(run/name, {}) or {}
            if record.get('repo'):
                paths.append(record['repo'])
    return paths


def materialize_cached_snapshot(original, value, worker):
    box = value['box']
    cache, snapshot = worker['cache_root'](box), worker['snapshot_root'](value)
    claim = value.get('phase_claim', {})
    from types import SimpleNamespace
    needs = SimpleNamespace(peer_id=claim.get('needs', {}).get('peer_id', value.get('run_id', 'snapshot')))
    retry = AdmissionWait(box, needs, admission_seconds(claim))
    with named_capacity_mutex(cache/'.snapshot-materialize.lock', mutex=worker['mutex'], box=box,
                              peer=needs.peer_id, wait=retry.wait, remaining=retry.remaining()), \
         named_capacity_mutex(snapshot/'.snapshot.lock', mutex=worker['mutex'], box=box,
                              peer=needs.peer_id, wait=retry.wait, remaining=retry.remaining()):
        if claim and value.get('run_id'):
            owner = worker['facts'].read_reservation(claim['claim'], archive=False)
            if not owner or owner.get('token') != claim['token']:
                raise SpreadRefusal('snapshot materializer claim changed owner')
            worker['atomic_json'](worker['root_for'](box)/'runs'/value['run_id']/'snapshot-use.json',
                                 dict(repo=str(snapshot), token=claim['token']))
        prepared = dict(value)
        if value.get('pack') and not Path(value['pack']).is_file():
            receipt = read_json(snapshot/'pool-inputs.json', {}) or {}
            if receipt.get('head') != value['head'] or receipt.get('manifest') != value['manifest']:
                raise SpreadRefusal('snapshot metadata pack is missing before publication')
            prepared.pop('pack')  # a lost reply after verified publication is idempotent
        result = original(prepared)
        prune = prune_snapshots(cache/'snapshots', None if value.get('mutable') else snapshot,
                                protected=live_snapshot_paths(box, worker))
        worker['atomic_json'](cache/'snapshot-prune.json', dict(box=box['name'], **prune))
        return dict(result, snapshot_prune=prune)


def native_pool_source(source):
    """Preserve native limits while correcting preparation-only accounting."""
    extension = ("\n_native_live_reason = live_reason\n"
                 "def live_reason(box,needs,state):\n"
                 "    from spread_peers import named_live_reason\n"
                 "    return named_live_reason(_native_live_reason,box,needs,state)\n")
    return source + extension


def existing_native_claim(box, needs, request, facts, root):
    """An SSH reply may be lost after the atomic native claim succeeded."""
    marker = Path(root)/'claims'/('run-'+request['run_id']+'.json')
    owner = facts.read_reservation(marker, archive=False)
    if not owner:
        if marker.exists():
            raise SpreadRefusal(f'RUN ROOT CONFLICT {marker}; native claim has no live recorded owner')
        return None
    if (owner.get('token') != request.get('token')
            or any(owner.get(key) != needs.get(key) for key in ('case_id', 'peer_id'))):
        raise SpreadRefusal(f'RUN ROOT CONFLICT {marker}; native claim changed owner')
    return dict(claim=str(marker).replace('\\', '/'), token=owner['token'],
                root=str(Path(root)/'runs'/request['run_id']).replace('\\', '/'), box=box['name'],
                slot=owner.get('slot'), slot_marker=None, alone_marker=None,
                ports=owner['ports'], directory_port=owner['directory_port'])


def admission_seconds(claim):
    if claim.get('admission_deadline') is not None:
        return max(0, float(claim['admission_deadline'])-time.time())
    return claim.get('runner_wait_remaining', claim.get('runner_wait', 0))


def bind_admission_transport(backend, box, needs, wait):
    """Adapt the installed transport's waits; retain its hash, task and RPC paths."""
    retry = backend.admission_wait = AdmissionWait(box, needs, wait)
    original_rpc = backend.rpc
    def rpc(target, action, body, **kwargs):
        active = getattr(backend, 'active_claim', None)
        if active:
            active.update(runner_wait_remaining=retry.remaining(), admission_deadline=time.time()+retry.remaining())
        if action == 'write' and body.get('path', '').endswith(('/request.json', '/peer-spec.json')):
            value = body.get('value', {})
            if 'claim' in value:
                value['claim']['runner_wait_remaining'] = retry.remaining()
                value['claim']['admission_deadline'] = time.time()+retry.remaining()
        result = retry.call(lambda:original_rpc(target, action, body, **kwargs))
        if action == 'submit-task':
            body['claim'].update(slot=result['slot'], slot_marker=result['slot_marker'])
        return result
    backend.rpc = rpc
    if hasattr(backend, 'stop'):
        original_stop = backend.stop
        def stop(target, claim):
            # A failed preparation SSH operation is retried on its own root;
            # cancelling that root would poison the eventual native launch.
            if getattr(backend, '_admission_operation', False) and not claim.get('launched'):
                return
            return original_stop(target, claim)
        backend.stop = stop
    if hasattr(backend, 'guarded_run'):
        original_run = backend.guarded_run
        def guarded_run(argv, **kwargs):
            def attempt():
                backend._admission_operation = True
                try:
                    return original_run(argv, **kwargs)
                except RuntimeError as error:
                    if Path(str(argv[0])).name in ('ssh', 'scp') and 'command failed (255)' in str(error):
                        raise RuntimeError('SSH does not answer or the saved connection is unavailable') from error
                    raise
                finally:
                    backend._admission_operation = False
            return retry.call(attempt)
        backend.guarded_run = guarded_run
    return retry


def submit_named_task(original, value, worker):
    """Claim a ready native payload slot only at this owned launch."""
    from pool import Needs, live_reason
    box, claim = value['box'], value['claim']
    facts, root = worker['facts'], worker['root_for'](box)
    needs = Needs(**claim['needs'])
    retry = AdmissionWait(box, needs, admission_seconds(claim))
    while True:
        with named_capacity_mutex(root/'.capacity.lock', mutex=worker['mutex'], box=box,
                                  peer=needs.peer_id, wait=retry.wait, remaining=retry.remaining()):
            slots = worker['task_slots'](box)
            selected, reason = None, 'all registered payload slots are busy or unavailable'
            for row in slots:
                if box.get('requested_task_slot') and row['slot'] != box['requested_task_slot']:
                    continue
                owner = facts.read_reservation(row['owner_marker'], archive=False)
                owned = owner and owner.get('token') == claim['token']
                if owned and row['state'] == 'Running':
                    return dict(started=True, slot=row['slot'], slot_marker=row['owner_marker'],
                                free_gb=worker['memory_gb'](), floor_gb=box['free_floor_gb'])
                if row['state'] in ('Ready', 'Reserved') and (owned or not Path(row['owner_marker']).exists()):
                    selected = row
                    break
            if selected:
                retry.probe_started()
                state = worker['capacity_state'](box, ignore_token=claim['token'])
                reason = live_reason(box, needs, state)
                if not reason:
                    claim['slot'] = selected['slot']
                    path = Path(value['request'])
                    request = json.loads(path.read_text(encoding='utf-8-sig'))
                    if request['claim']['token'] != claim['token']:
                        raise SpreadRefusal('native task request changed owner')
                    request['claim']['slot'] = selected['slot']
                    worker['atomic_json'](path, request)
                    try:
                        return original(value)
                    except RuntimeError as error:
                        if not retry.accepts(error):
                            raise
                        reason = str(error)
            if not retry.accepts(reason):
                raise SpreadRefusal('capacity changed before launch: '+reason)
        retry.pause(reason)
        worker['renew_claim'](claim)


def native_adapter_sources(facts_path, existing=None, *, kit=None):
    """Ship the existing facts adapter's route reader before its launch adapter."""
    folder = Path(facts_path).parent
    adapters = {name: (folder/name).read_text(encoding="utf-8")
                for name in ("pool_cohort.py", "pool_run.py") if (folder/name).is_file()}
    if 'pool_cohort.py' not in adapters and kit is not None and (Path(kit)/'pool_cohort.py').is_file():
        adapters['pool_cohort.py'] = (Path(kit)/'pool_cohort.py').read_text(encoding='utf-8')
    return {**({"pool_cohort.py": adapters["pool_cohort.py"]} if "pool_cohort.py" in adapters else {}),
            **(existing or {}),
            **({"pool_run.py": adapters["pool_run.py"]} if "pool_run.py" in adapters else {})}


@contextlib.contextmanager
def named_engine_cpu_wait(load, box, needs, renew, wait=0):
    """Retry the runner's unchanged admission, only in this native peer process."""
    original = load.admission
    @contextlib.contextmanager
    def admission(argv, environment, record, save):
        retry = AdmissionWait(box, needs, wait)
        while True:
            stack = contextlib.ExitStack()
            retry.probe_started()
            try:
                value = stack.enter_context(original(argv, environment, record, save))
            except load.LoadRefusal as error:
                stack.close()
                values = record.get("refusal", {}).get("values", {})
                if record.get('pid') or (values and (values['engines'] + 1 > values['max_engines']
                                         or values['free_gb'] < values['free_floor_gb'])):
                    raise
                reason = str(error)
                if values:
                    reason = ('capacity refused: another run holds the box alone' if values.get('alone') else
                              f"CPU {values['cpu_busy_percent']:.1f}% exceeds {values['cpu_busy_limit']:g}% "
                              f"over the last {values['cpu_sample_s']:g} seconds")
                if not retry.accepts(reason):
                    raise
                record.setdefault("admission_retries", []).append(dict(reason=reason, values=values))
                save()
                retry.pause(reason)
                renew()
                continue
            if getattr(needs, 'alone', False) and value and value.get('engines'):
                stack.close()
                reason = 'alone run needs an idle box'
                record.setdefault('admission_retries', []).append(dict(reason=reason, values=value))
                save()
                retry.pause(reason)
                renew()
                continue
            # Only a successful unchanged guard clears this attempt's temporary
            # not-started fields. All refused probes remain in admission_retries.
            if record.get("admission_retries"):
                for key in ("not_started", "result_status", "exit_code", "refusal"):
                    record.pop(key, None)
                save()
            with stack:
                yield value
            return
    load.admission = admission
    try:
        yield
    finally:
        load.admission = original


def wait_native_admission(backend, box, claim, wait=0, *, on_prepared=None):
    """Finish native admission before starting a case timer; require its PID."""
    deadline = time.monotonic() + float(wait or 0) + 1320
    while True:
        state = backend.rpc(box, "run-state", dict(claim=claim), timeout=20).get("state")
        if state and state.get("token") != claim["token"]:
            raise SpreadRefusal("native launch state changed owner")
        raw = backend.rpc(box, "text", dict(path=claim["root"] + "/progress.json"), timeout=20)["text"]
        if raw:
            progress = json.loads(raw)
            if progress.get("public_prepared") and on_prepared:
                if progress.get("token") != claim["token"]:
                    raise SpreadRefusal("native preparation changed owner")
                on_prepared()
            if progress.get("pid") and progress.get("identity"):
                return
            if progress.get("record", {}).get("error"):
                raise SpreadRefusal(progress["record"]["error"])
        raw = backend.rpc(box, "text", dict(path=claim["root"] + "/finished.json"), timeout=20)["text"]
        if raw:
            result = json.loads(raw)
            if result.get("token") == claim["token"]:
                raise SpreadRefusal(native_refusal(result))
        if time.monotonic() >= deadline:
            raise SpreadRefusal("native admission did not return within its CPU/quiet wait budget")
        time.sleep(2)


def claim_named_peer(backend, box, needs, request, *, wait=0, wait_for_holder=None):
    """Retry every native admission transient within one named candidate's budget."""
    retry = getattr(backend, 'admission_wait', None) or AdmissionWait(box, needs, wait)
    def attempt():
        remaining = retry.remaining()
        native_request = dict(request, runner_wait=retry.wait, capacity_wait_remaining=remaining)
        return backend.rpc(box, 'claim', dict(box=box, needs=needs.__dict__, request=native_request),
                           timeout=30 + capacity_lock_seconds(retry.wait, remaining))
    return retry.call(attempt)


def wait_named_launch(worker, pool, box, claim, *, wait=0, wait_for_holder=None):
    """Recheck the existing native guard after shipping, before case timers."""
    if not wait or box['kind'] != 'local':
        return
    needs = pool.Needs(**claim['needs'])
    retry = AdmissionWait(box, needs, wait)
    while True:
        retry.probe_started()
        probe = worker.capacity_state
        if hasattr(worker, 'facts'):
            state = native_exclusive_state(probe, box, facts=worker.facts, root=worker.root_for(box),
                                           read_only=True, refresh_display=False, ignore_token=claim['token'])
        else:
            state = probe(box, read_only=True, refresh_display=False, ignore_token=claim['token'])
        reason = named_live_reason(pool.live_reason, box, needs, state)
        if not reason:
            return
        if not retry.accepts(reason):
            raise SpreadRefusal('capacity changed before launch: '+reason)
        retry.pause(reason)
        worker.renew_claim(claim)






def named_task_box(box, tasks, names, name):
    """Narrow this peer's task list without changing the shared catalog."""
    task = role_value({key.casefold(): value for key, value in tasks.items()}, names, name)
    if task is None:
        return box
    slots = [slot for slot in box.get("task_slots", ()) if slot["task"] == task]
    if box["kind"] != "windows-task" or len(slots) != 1:
        raise SpreadRefusal(f"named task {task} is not registered on {box['name']}")
    return dict(box, task_slots=slots)


def public_directory_settings(repo, case_id, role):
    header = (Path(repo) / "Source/Managers/SettingsMan.h").read_text(encoding="utf-8")
    default = re.search(r'c_DefaultSessionDirectoryUrl\s*=\s*"([^"]+)"', header)
    if not default:
        raise RuntimeError("the build names no default public directory")
    key = hashlib.sha256((case_id + "/" + role).encode()).hexdigest()[:32]
    return dict(SessionDirectoryUrl=default[1], SessionDirectoryCertSha256="",
                SessionDirectoryInstallKey=key, NetworkIceEnable="1", NetworkPortMapEnable="0",
                NetworkHostGameListing="unlisted")


class Case:
    def __init__(self, repo, out, peers, match, *, peer_boxes=None, dispatcher=None, registry=None, peer_ports=None):
        self.repo, self.out = Path(repo).resolve(), Path(out).resolve()
        self.out.parent.mkdir(parents=True, exist_ok=True)
        self.peers, self.match = list(peers), match
        self.public_launch_lock = threading.Lock()
        self.public_prepared = {}
        self.public_host_released = False
        self.public_released = set()
        self.names = [peer.name for peer in self.peers]
        if not self.names or len(set(name.casefold() for name in self.names)) != len(self.names):
            raise ValueError("a spread case declares unique peers")
        self.output_names = {peer.name: peer.output_name or peer.name for peer in self.peers}
        if len({name.casefold() for name in self.output_names.values()}) != len(self.names):
            raise ValueError("a spread case declares unique output names")
        self.interface_source = Path(__file__).read_text(encoding="utf-8")
        self.interface_sha256 = hashlib.sha256(self.interface_source.encode()).hexdigest()
        self.pins, self.peer_ports = pairs(peer_boxes), peer_ports or {}
        try:
            self.assigned_boxes = named_peer_boxes(self.peers, self.pins)
        except (SpreadUsageError, SpreadRefusal) as error:
            value = dict(schema=1, topology="spread", interface_version=INTERFACE_VERSION, passed=False, error=str(error),
                         peer_boxes={peer.name: role_value(self.pins, self.names, peer.name) for peer in self.peers},
                         requested_peer_boxes=self.pins, refusals=[])
            if getattr(error, "peer", None):
                value.update(refused_peer=error.peer, refused_box=error.box, reason=error.reason,
                             refusals=[dict(peer=error.peer, box=error.box, reason=error.reason, text=str(error))])
            write_json(self.out/"spread-result.json", value)
            raise
        self.pool, self.transport_module, self.dispatcher = installed_pool(dispatcher, registry)
        self.registry = Path(registry or self.dispatcher.with_name("boxes.json"))
        self.members, self.runs, self.tunnels, self.refusals, self.identities, self.pending = {}, {}, [], [], {}, []
        self.lock = threading.RLock()
        self.stack = contextlib.ExitStack()
        self.closed = False
        self.last_sync = 0
        self.id = uuid.uuid4().hex
        self.control = self.out.parent/(".spread-" + self.id)
        self.control.mkdir(exist_ok=True)
        marker = self.out/".spread-case-owner.json"
        facts = self.transport_module.worker.facts
        if marker.exists():
            owner = read_json(marker, {}) or {}
            name = self.names[0]
            box = self.assigned_boxes[name]
            reason = f"RUN ROOT CONFLICT {self.out}; peer {owner.get('peer_id', 'unknown')} case {owner.get('case_id', 'unknown')}"
            value = dict(schema=1, topology="spread", interface_version=INTERFACE_VERSION,
                         peer_boxes=self.assigned_boxes, passed=False, refused_peer=name, refused_box=box, reason=reason)
            # Never overwrite the live run's receipt to report a conflict.
            write_json(self.control/"spread-result.json", value)
            raise SpreadRefusal(f"spread peer {name} on {box}: {reason}")
        try:
            owner = facts.write_reservation(marker, "named case "+self.id, token=self.id,
                                            extra=dict(case_id=self.id, peer_id=self.names[0],
                                                       run_root=str(self.out), peer_boxes=self.assigned_boxes))
        except FileExistsError as error:
            # The facts writer arbitrates two simultaneous attempts atomically.
            raise SpreadRefusal(f"spread peer {self.names[0]} on {self.assigned_boxes[self.names[0]]}: "
                                f"RUN ROOT CONFLICT {self.out}; {error}") from error
        self.stack.callback(facts.release_reservation, marker, owner["token"])
        # Keep engine artifacts in the caller's catalog-derived scratch lane.
        try:
            catalog = self.pool.load_registry(self.registry)["boxes"]
            local = next((box for box in catalog if box["kind"] == "local"), None)
            scratch = Path(local["scratch"]).resolve() if local else self.out.parent
            self.lane_root = next((parent for parent in (self.out, *self.out.parents) if parent.parent == scratch), self.out.parent)
            self.lane = self.lane_root.name
            self.allocate()
            self.prepare()
            self.connect_directory()
        except BaseException as error:
            if not isinstance(error, (SpreadRefusal, SpreadUsageError)):
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

    def stage_root(self, root):
        """Let a driver stage in this case's already claimed controller root."""
        root = Path(root).resolve()
        owner = read_json(root/".spread-case-owner.json", {}) or {}
        if self.closed or root != self.out or owner.get("token") != self.id or owner.get("case_id") != self.id:
            raise SpreadRefusal(f"RUN ROOT CONFLICT {root}; staging does not own the live case marker")

    def backend(self):
        module = self.transport_module
        # Use the installed facts adapter with this caller's immutable inputs.
        source_repo = self.repo if (self.repo/"tools/box_facts.py").is_file() else Path(module.worker.facts.__file__).resolve().parents[1]
        backend = module.Transport(repo=source_repo, work=self.lane_root/".spread-inputs", registry=self.registry)
        backend.repo = self.repo
        backend.sources["spread_peers.py"] = self.interface_source
        backend.sources["pool_worker.py"] = native_cpu_wait_source(backend.sources["pool_worker.py"])
        backend.sources['pool.py'] = native_pool_source(backend.sources['pool.py'])
        # The existing facts reader lazily imports pool_cohort under a named
        # assignment. It reads that assignment only; it does not select boxes.
        backend.sources = native_adapter_sources(module.worker.facts.__file__, backend.sources, kit=module.LEAD)
        preflight = Path(__file__).with_name("cross_peers.py")
        self.preflight_source = preflight.read_text(encoding="utf-8") if preflight.is_file() else None
        self.native_ship_set = {name: hashlib.sha256(source.encode()).hexdigest() for name, source in backend.sources.items()}
        if self.preflight_source:
            self.native_ship_set['cross_peers.py'] = hashlib.sha256(self.preflight_source.encode()).hexdigest()
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
        self.peer_choices = named_peer_choices(self.peers, self.pins)
        self.assigned_boxes = named_peer_boxes(self.peers, self.pins)
        self.route_attempts, self.route_indices, self.retired_members = {}, {}, []
        for peer in self.peers:
            port = int(role_value(self.peer_ports, self.names, peer.name) or self.match.port)
            if port != self.match.port:
                raise self.refuse(peer.name, self.assigned_boxes[peer.name],
                                  f'match port {port} differs from host port {self.match.port}')
        for peer in self.peers:
            self.allocate_peer(peer)
        self.check_identities({name:dict(machine_id=item[0]['hostname'].casefold()) for name,item in self.members.items()})

    def allocate_peer(self, peer, start=0):
        catalog = {box['name'].casefold():box for box in self.pool.load_registry(self.registry)['boxes']}
        for index, pin in enumerate(self.peer_choices[peer.name][start:], start):
            box = catalog.get(pin.casefold())
            if not box:
                raise self.refuse(peer.name, pin, 'named box is absent from the catalog')
            box = named_task_box(dict(box), self.match.parameters.get("peer_tasks", {}), self.names, peer.name)
            slots = self.match.parameters.get('peer_task_slots', pairs(getattr(_options, 'peer_task_slot', [])))
            task_slot = peer.task_slot or role_value(slots, self.names, peer.name)
            if task_slot is not None:
                box['requested_task_slot'] = int(task_slot)
            other_assignments = {name:item[0]['name'] for name,item in self.members.items() if name != peer.name}
            reason = sharing_reason(self.peers, other_assignments, peer.name, box['name'])
            if peer.recorder and not (box['os'] == 'windows' and box['kind'] in ('local', 'windows-task')):
                reason = "reviewed screen requires the named runner's private Windows recorder"
            needs = self.pool.Needs(os=peer.os, engines=peer.engines, gpu=bool(peer.size), memory=peer.memory,
                                    alone=peer.quiet, size=peer.size, only_box=box['name'],
                                    case_id=self.id, peer_id=peer.name, share_ok=peer.share_ok,
                                    reviewed=peer.reviewed or peer.recorder, held=peer.held)
            reason = reason or self.pool.static_reason(box, needs)
            attempt = dict(box=box['name'], rank=index+1, status='NO' if reason else 'CHECKING')
            self.route_attempts.setdefault(peer.name, []).append(attempt)
            if reason:
                attempt['reason'] = reason
                continue
            caller_lane = peer.lane or self.match.parameters.get('lane')
            label = self.match.parameters.get('label') or getattr(_options, 'runner_label', None) or (f'{caller_lane}: spread' if caller_lane else 'spread')
            request = dict(run_id=uuid.uuid4().hex, token=uuid.uuid4().hex, label=label,
                           lane=caller_lane or self.lane, case_id=self.id, peer_id=peer.name,
                           owner=dict(pid=os.getpid(), machine=self.transport_module.worker.facts.machine_name(),
                                      process_start=self.transport_module.worker.facts.process_start(os.getpid())))
            if hasattr(self, 'control'):
                request.update(out=str(self.control/peer.name/'results'), command=[], hang_guard=max(600, peer.timeout+300))
            backend = self.backend()
            wait = self.match.parameters.get('runner_wait', getattr(_options, 'runner_wait', 0))
            retry = bind_admission_transport(backend, box, needs, wait)
            try:
                state = retry.call(lambda:backend.probe(box, read_only=True))
                claim = claim_named_peer(backend, box, needs, request, wait=wait,
                                        wait_for_holder=self.match.parameters.get('wait_for_holder', getattr(_options, 'wait_for_holder', None)))
            except RuntimeError as error:
                attempt.update(status='WAIT EXPIRED' if isinstance(error, AdmissionExpired) else 'NO', reason=str(error))
                if isinstance(error, AdmissionExpired) or hard_admission_refusal(error):
                    continue
                raise self.refuse(peer.name, box['name'], str(error)) from error
            claim.update(control=backend.control(box), started=time.time(), needs=needs.__dict__, runner_wait=wait)
            box['hostname'] = box.get('hostname') or state.get('hostname')
            if not box['hostname']:
                raise self.refuse(peer.name, box['name'], 'native machine identity is unavailable')
            self.members[peer.name] = (box, claim, request, backend)
            self.assigned_boxes[peer.name], self.route_indices[peer.name] = box['name'], index
            attempt['status'] = 'ADMITTED'
            print(f"NAMED: {box['name']}; peer {peer.name}; rank {index+1}", flush=True)
            return
        attempted = self.route_attempts[peer.name]
        reasons = '; '.join(f"{row['box']} -> {row.get('reason', row['status'])}" for row in attempted)
        raise self.refuse(peer.name, attempted[-1]['box'], reasons if len(attempted)>1 else attempted[-1]['reason'])

    def fallback_peer(self, name, error):
        if not (isinstance(error, AdmissionExpired) or hard_admission_refusal(error)):
            return False
        index = self.route_indices[name]+1
        if index >= len(self.peer_choices[name]):
            return False
        old = self.members.pop(name)
        self.retired_members.append(old)
        self.route_attempts[name][-1].update(status='WAIT EXPIRED' if isinstance(error, AdmissionExpired) else 'NO', reason=str(error))
        self.allocate_peer(next(peer for peer in self.peers if peer.name == name), index)
        return True

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
            while True:
                try:
                    self.prepare_peer(name)
                    break
                except RuntimeError as error:
                    if hasattr(self, 'peer_choices') and self.fallback_peer(name, error):
                        continue
                    box = self.members[name][0]
                    if hasattr(self, 'refuse'):
                        raise self.refuse(name, box['name'], str(error)) from error
                    raise
        self.check_prepared_hashes()
        if hasattr(self, 'peer_choices'):
            self.check_identities({name:dict(machine_id=item[0]['hostname'].casefold()) for name,item in self.members.items()})

    def check_prepared_hashes(self):
        windows_hashes = {claim['exe_sha256'] for box,claim,_,_ in self.members.values()
                          if box['os'] == 'windows' and claim.get('exe_sha256')}
        if len(windows_hashes) > 1:
            raise SpreadRefusal('Windows executable changed between peer shipments')

    def prepare_peer(self, name):
        box, claim, request, backend = self.members[name]
        backend.active_claim, backend.active_box = claim, box
        backend.snapshot = self.input_snapshot
        backend.prepare(box, claim, request)
        if getattr(self, 'preflight_source', None):
            backend.rpc(box, 'install', dict(root=claim['control'], sources={'cross_peers.py':self.preflight_source}))
        if claim['head'] != self.input_snapshot['head']:
            raise SpreadRefusal('source changed after the case input snapshot was frozen')
        claim['case_root'] = box['scratch'].rstrip('/')+'/'+self.lane+'/native-'+self.id
        if self.match.port in range(claim['ports'][0], claim['ports'][1]+1):
            raise SpreadRefusal('driver match port overlaps the pool control port map')
        if hasattr(backend, 'admission_wait') and hasattr(self.pool, 'live_reason'):
            needs, retry = self.pool.Needs(**claim['needs']), backend.admission_wait
            while True:
                retry.probe_started()
                state = backend.probe(box, read_only=True, refresh_display=False)
                reason = named_live_reason(self.pool.live_reason, box, needs, state)
                if not reason:
                    break
                if not retry.accepts(reason):
                    raise SpreadRefusal('capacity changed before launch: '+reason)
                retry.pause(reason)
                self.guard()
        self.guard()

    def connect_directory(self):
        self.network = self.match.parameters.get("network", "ice")
        if self.network not in ("ice", "direct"):
            raise self.refuse(self.names[0], self.members[self.names[0]][0]["name"], "unknown declared network mode")
        self.public_directory = bool(self.match.parameters.get("public_directory"))
        if self.public_directory:
            if self.network != "ice" or self.match.parameters.get("directory"):
                raise SpreadRefusal("public directory requires ICE without a private directory descriptor")
            self.directory = None
            return
        if self.network == "direct":
            box, claim, _, backend = self.members[self.names[0]]
            address = self.match.parameters.get("host_address")
            self.host_addresses = {}
            if address:
                self.host_addresses = {name: address for name in self.names}
            else:
                from cross_peers import remote_command
                script = "import sys;sys.path.insert(0,sys.argv[1]);from spread_peers import native_address_probe;print(native_address_probe(sys.argv[2]))"
                for name in self.names[1:]:
                    try:
                        endpoint = named_box_endpoint(self.members[name][0])
                        command = [box["python"], "-c", script, claim["control"], endpoint]
                        if box["kind"] != "local":
                            command = remote_command(box, command)
                            command[1:1] = ["-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5"]
                        self.host_addresses[name] = backend.guarded_run(command, timeout=20).decode().strip()
                    except Exception as error:
                        raise self.refuse(name, self.members[name][0]["name"], str(error)) from error
                address = next(iter(self.host_addresses.values()), None) or native_address_probe()
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
        for name in self.names:
            self.connect_peer_directory(name)

    def connect_peer_directory(self, name):
        if not self.directory:
            return
        box, claim, _, backend = self.members[name]
        urls = self.match.parameters.get('peer_directory_urls', {})
        target = role_value(urls, self.names, name) or self.directory['DIRECTORY_URL']
        try:
            target_port = directory_endpoint(target)
        except ValueError as error:
            raise self.refuse(name, box['name'], str(error)) from error
        local = target_port if box['kind'] == 'local' else claim['directory_port']
        claim['signal_port'] = local
        if box['kind'] == 'local':
            return
        retry = getattr(backend, 'admission_wait', None)
        while True:
            log = (self.control/f'tunnel-{name}.log').open('ab')
            process = subprocess.Popen(['ssh', '-N', '-o', 'BatchMode=yes', '-o', 'ExitOnForwardFailure=yes', '-o', 'ServerAliveInterval=15',
                                        '-R', f'127.0.0.1:{local}:127.0.0.1:{target_port}', box['ssh']],
                                       stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
            time.sleep(1)
            if process.poll() is None:
                self.tunnels.append((process, log))
                return
            log.close()
            reason = 'SSH does not answer: private directory signaling tunnel refused'
            if not retry:
                raise self.refuse(name, box['name'], reason)
            retry.pause(reason)

    def ensure_peer_ready(self, name):
        while True:
            box, claim, _, backend = self.members[name]
            if not hasattr(backend, 'admission_wait') or not hasattr(self.pool, 'live_reason'):
                return
            needs, retry = self.pool.Needs(**claim['needs']), backend.admission_wait
            try:
                while True:
                    retry.probe_started()
                    state = backend.probe(box, read_only=True, refresh_display=False)
                    reason = named_live_reason(self.pool.live_reason, box, needs, state)
                    if not reason:
                        return
                    if not retry.accepts(reason):
                        raise SpreadRefusal('capacity changed before launch: '+reason)
                    retry.pause(reason)
                    self.guard()
            except RuntimeError as error:
                if not self.fallback_peer(name, error):
                    raise self.refuse(name, box['name'], str(error)) from error
                self.prepare_peer(name)
                self.check_prepared_hashes()
                self.check_identities({peer:dict(machine_id=item[0]['hostname'].casefold()) for peer,item in self.members.items()})
                if getattr(self, 'network', 'ice') == 'direct' and getattr(self, 'runs', {}):
                    # Re-evaluate the existing host route for this named seat.
                    host, host_claim, _, host_backend = self.members[self.names[0]]
                    from cross_peers import remote_command
                    endpoint = named_box_endpoint(self.members[name][0])
                    script = 'import sys;sys.path.insert(0,sys.argv[1]);from spread_peers import native_address_probe;print(native_address_probe(sys.argv[2]))'
                    command = [host['python'], '-c', script, host_claim['control'], endpoint]
                    if host['kind'] != 'local':
                        command = remote_command(host, command)
                    self.host_addresses[name] = host_backend.guarded_run(command, timeout=20).decode().strip()
                self.connect_peer_directory(name)

    def release_public_launch(self, name):
        """Keep native queue time ahead of the original host menu sequence."""
        self.guard()
        with self.public_launch_lock:
            for role in self.names:
                handle = self.runs.get(role)
                if handle and getattr(handle, "start_failure", None):
                    raise SpreadRefusal(f"requested peer {role} did not start: {handle.start_failure}")
                if role in self.public_prepared:
                    continue
                box, claim, _, backend = self.members[role]
                raw = backend.rpc(box, "text", dict(path=claim["root"] + "/progress.json"), timeout=20).get("text")
                progress = json.loads(raw) if raw else {}
                if not progress.get("public_prepared"):
                    continue
                if progress.get("token") != claim["token"] or progress.get("role") != role:
                    raise SpreadRefusal(f"native preparation changed owner for {role}")
                identity = progress.get("identity", {})
                if identity.get("executable_sha256") != claim["exe_sha256"]:
                    raise SpreadRefusal(f"native preparation changed executable for {role}")
                self.public_prepared[role] = identity
            if len(self.public_prepared) != len(self.names):
                return
            if not self.public_host_released:
                self.write_public_release(self.names[0])
                self.public_host_released = True
            if name == self.names[0] or name in self.public_released:
                return
        session = self.published_session(name)
        with self.public_launch_lock:
            if name not in self.public_released:
                self.write_public_release(name, session)

    def write_public_release(self, name, session=None):
        box, claim, _, backend = self.members[name]
        value = dict(case_id=self.id, role=name, token=claim["token"], session=session,
                     executable_sha256=claim["exe_sha256"])
        backend.rpc(box, "write", dict(path=claim["root"] + "/public-launch.json", value=value,
                                      phase_claim=self.transport_module.Transport.native_claim(claim)))
        self.public_released.add(name)

    def published_session(self, name):
        from e2e_video import directory_session
        port = int(role_value(self.peer_ports, self.names, name) or self.match.port)
        host = self.runs.get(self.names[0])
        if host and getattr(host, 'launch_attempted', False):
            wait = self.match.parameters.get('runner_wait', getattr(_options, 'runner_wait', 0))
            try:
                wait_started_host(self, name, wait)
            except SpreadRefusal as error:
                host_box = self.members[self.names[0]][0]['name']
                reason = getattr(host, 'start_failure', None) or str(error)
                raise self.refuse(name, self.members[name][0]['name'],
                                  f'requested host on {host_box} did not start: {reason}') from error
        deadline = time.monotonic() + 80
        while time.monotonic() < deadline:
            host = self.runs.get(self.names[0])
            if host and getattr(host, 'start_failure', None):
                host_box = self.members[self.names[0]][0]['name']
                raise self.refuse(name, self.members[name][0]['name'],
                                  f'requested host on {host_box} did not start: {host.start_failure}')
            if getattr(self, "public_directory", False):
                self.guard()
                host = self.runs.get(self.names[0])
                if not host:
                    raise self.refuse(name, self.members[name][0]["name"], "the declared host has no native run")
                box, claim, _, backend = self.members[self.names[0]]
                path = claim["case_root"] + "/" + host.output_name + "/stdout.log"
                text = backend.rpc(box, "text", dict(path=path), timeout=15).get("text") or ""
                registered = re.findall(r"\[net-directory\] registered session_id=([A-Za-z0-9_-]+) heartbeat_s=", text)
                session = registered[-1] if registered else None
            else:
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
                    if progress["record"].get("error"):
                        raise self.refuse(handle.role, box["name"], progress["record"]["error"])
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
        return dict(schema=1, topology="spread", interface_version=INTERFACE_VERSION,
                    native_ship_set=getattr(self, 'native_ship_set', {}),
                    peer_boxes=getattr(self, "assigned_boxes", {name: item[0]["name"] for name, item in self.members.items()}),
                    requested_peer_boxes=getattr(self, 'pins', {}),
                    route_attempts=getattr(self, 'route_attempts', {}),
                    interface_sha256=self.interface_sha256,
                    preflight_sha256=hashlib.sha256(self.preflight_source.encode()).hexdigest() if getattr(self, "preflight_source", None) else None,
                    sharing={peer.name: dict(share_ok=peer.share_ok, reviewed=peer.reviewed, held=peer.held, quiet=peer.quiet,
                                              recorder=peer.recorder, readback=peer.readback) for peer in self.peers},
                    peer_tasks={name: item[1].get("slot") for name, item in self.members.items()},
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
            for box, claim, _, backend in getattr(self, 'retired_members', []):
                cleanup.callback(backend.release, box, claim)
            self.save()

    def release_pending(self, role=None):
        """Retain the old callable without publishing or reading queue tickets."""
        return None

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


def overlay_data(source, target, paths, *, module_content=False):
    """Give overlays private ancestors and identical regular module contents."""
    target.mkdir(parents=True, exist_ok=False)
    for child in source.iterdir():
        destination = target / child.name
        below = [path for path in paths if path.parts[0] == child.name]
        if module_content and (child.is_symlink() or getattr(child, 'is_junction', lambda:False)()):
            raise SpreadRefusal(f"private module input contains a symlink: {child}")
        if child.is_dir():
            if below or module_content:
                overlay_data(child, destination, [Path(*path.parts[1:]) for path in below if len(path.parts) > 1],
                             module_content=module_content or child.suffix.casefold() == ".rte")
            else:
                link_directory(child, destination)
        elif module_content and not below:
            import errno
            try:
                os.link(child, destination)
            except OSError as error:
                if error.errno not in (errno.EXDEV, errno.EPERM, errno.EACCES, errno.ENOTSUP, errno.EMLINK) and getattr(error, 'winerror', None) != 1142:
                    raise
                cache_link_or_copy(child, destination, file_sha256(child))
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
        if hasattr(self.case, 'ensure_peer_ready'):
            self.case.ensure_peer_ready(self.role)
        box, claim, request, backend = self.case.members[self.role]
        host_address = getattr(self.case, "host_addresses", {}).get(self.role, getattr(self.case, "host_address", None))
        port = int(role_value(self.case.peer_ports, self.case.names, self.role) or self.case.match.port)
        if port != self.case.match.port:
            raise self.case.refuse(self.role, box["name"], f"match port {port} differs from host port {self.case.match.port}")
        args = list(self.argv[2:])
        if "-net-port" in args and int(args[args.index("-net-port") + 1]) != port:
            raise self.case.refuse(self.role, box["name"], f"match port {args[args.index('-net-port') + 1]} differs from host port {port}")
        session = None
        session_routing = (self.case.directory or getattr(self.case, "public_directory", False)) and getattr(self.case, "network", "ice") == "ice" and self.case.match.parameters.get("join_by_session", True)
        public_gate = bool(session_routing and getattr(self.case, "public_directory", False)
                           and self.case.match.parameters.get("public_menu_start", False))
        if session_routing and self.role != self.case.names[0]:
            session = PUBLIC_SESSION_INPUT if public_gate else self.case.published_session(self.role)
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
            args[args.index("-net-join") + 1] = host_address
        mappings = [(str(self.case.out), claim["case_root"]), (self.case.out.as_posix(), claim["case_root"]),
                    (str(self.repo), claim["repo"]), (self.repo.as_posix(), claim["repo"])]
        if self.retained is not None:
            mappings += [(str(self.retained), claim["case_root"] + "/" + self.output_name + "/runtime"),
                         (self.retained.as_posix(), claim["case_root"] + "/" + self.output_name + "/runtime")]
        files = {}
        session_files = []
        line_inputs = line_script_inputs(args, self.env)
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
                    files[relative] = base64.b64encode(native_line_script(path.read_bytes(), path, box, line_inputs)).decode()
                    mappings.append((str(path.resolve()), claim["case_root"] + "/" + relative))
        for path in paths:
            if not public_file(path):
                raise self.case.refuse(self.role, box["name"], "private credentials or tickets cannot be staged as case inputs")
            data = path.read_bytes()
            if public_gate and PUBLIC_SESSION_INPUT.encode() in data:
                raise self.case.refuse(self.role, box["name"], "case input contains the reserved public session placeholder")
            data = native_line_script(data, path, box, line_inputs)
            if path.suffix.lower() in (".txt", ".json", ".ini", ".lua"):
                data = map_script(data, mappings, session)
                if getattr(self.case, "network", "ice") == "direct":
                    data = re.sub(rb"(?m)^(settext TextJoinAddress)\s+(?:127\.0\.0\.1|localhost)\s*$",
                                  lambda match: match[1] + b" " + host_address.encode(), data)
            files[path.relative_to(self.case.out).as_posix()] = base64.b64encode(data).decode()
            if public_gate and PUBLIC_SESSION_INPUT.encode() in data:
                session_files.append(path.relative_to(self.case.out).as_posix())
        signals = self.case.signals()
        self.case.extra_signals = sorted(set(getattr(self.case, "extra_signals", ())) | set(signals))
        native = dict(schema=1, role=self.role, output_name=self.output_name, root=claim["case_root"], repo=claim["repo"], box=box,
                      case_id=self.case.id, lane=self.case.lane, controller_root=str(self.case.out),
                      case_ports=sorted({port, *([claim["signal_port"]] if claim.get("signal_port") else [])}),
                      control=claim["control"], claim=self.case.transport_module.Transport.native_claim(claim),
                      args=[map_text(argument, mappings) for argument in args],
                      env={key: map_text(value, mappings) for key, value in self.env.items()},
                      timeout=self.timeout, expected=[map_text(path, mappings) for path in self.expected],
                      fixtures=self.fixtures, files=files, signals=signals, retained_runtime=self.retained is not None,
                      executable_sha256=claim["exe_sha256"], directory=self.case.directory,
                      public_directory=getattr(self.case, "public_directory", False),
                      signal_port=claim.get("signal_port"), session=session)
        if public_gate:
            native["public_launch"] = dict(host=self.case.names[0], wait=float(
                self.case.match.parameters.get("runner_wait", getattr(_options, "runner_wait", 0)) or 0) + 1320,
                files=session_files, args=[i for i, argument in enumerate(native["args"]) if PUBLIC_SESSION_INPUT in argument])
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
            wait = self.case.match.parameters.get("runner_wait", getattr(_options, "runner_wait", 0))
            if not public_gate:
                wait_started_host(self.case, self.role, wait)
            if wait and box["kind"] == "local":
                wait_named_launch(self.case.transport_module.worker, self.case.pool, box, claim,
                                  wait=backend.admission_wait.remaining() if hasattr(backend, 'admission_wait') else wait,
                                  wait_for_holder=self.case.match.parameters.get("wait_for_holder", getattr(_options, "wait_for_holder", None)))
            request["hang_guard"] = max(request.get("hang_guard", 600), self.timeout + 1500 + float(wait or 0))
            remaining = backend.admission_wait.remaining() if hasattr(backend, 'admission_wait') else wait
            launch_native(backend, box, claim, request, remaining)
            if public_gate:
                wait_native_admission(backend, box, claim, remaining,
                                      on_prepared=lambda:self.case.release_public_launch(self.role))
            else:
                wait_native_admission(backend, box, claim, remaining)
        except SpreadRefusal as error:
            self.start_failure = str(error)
            if str(error).startswith(f"spread peer {self.role} on {box['name']}: "):
                raise
            raise self.case.refuse(self.role, box["name"], str(error)) from error
        except Exception as error:
            self.start_failure = str(error)
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


def stage_native_run(factory, ownership, box, peer, *args, **kwargs):
    """Stage with the existing runner, then claim its root before any start."""
    from cross_peers import peer_run_scope
    run = factory(*args, **kwargs)
    try:
        ownership.enter_context(peer_run_scope(box, peer))
    except BaseException:
        run.close()
        raise
    return run


PUBLIC_SESSION_INPUT = "__PUBLIC_SESSION_INPUT__"


def receive_public_launch(spec, control, identity, renew):
    """Bind the real public code after every native peer has prepared."""
    gate = spec.get("public_launch")
    if not gate:
        return spec
    token = spec["claim"]["token"]
    write_json(control / "progress.json", dict(public_prepared=True, token=token,
                                               role=spec["role"], identity=identity))
    deadline = time.monotonic() + gate["wait"]
    last_renewal = 0
    while True:
        value = read_json(control / "public-launch.json")
        if value:
            if any(value.get(key) != expected for key, expected in (
                ("case_id", spec["case_id"]), ("role", spec["role"]), ("token", token),
                ("executable_sha256", spec["executable_sha256"]))):
                raise SpreadRefusal("public launch input changed native owner or executable")
            break
        now = time.monotonic()
        if now >= deadline:
            raise SpreadRefusal("public launch did not complete within native admission budget")
        if now - last_renewal >= 8:
            renew()
            last_renewal = now
        time.sleep(.25)
    if spec["role"] == gate["host"]:
        return spec
    session = value.get("session", "")
    if not isinstance(session, str) or not re.fullmatch(r"[0-9A-Fa-f]{8}(?:-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}", session):
        raise SpreadRefusal("public launch has no full registered session code")
    result = dict(spec, session=session)
    result["args"], result["files"] = list(spec["args"]), dict(spec["files"])
    for index in gate["args"]:
        if not isinstance(index, int) or not 0 <= index < len(result["args"]) or PUBLIC_SESSION_INPUT not in result["args"][index]:
            raise SpreadRefusal("public launch has an invalid staged argument binding")
        result["args"][index] = result["args"][index].replace(PUBLIC_SESSION_INPUT, session)
    for path in gate["files"]:
        if path not in result["files"]:
            raise SpreadRefusal("public launch has an invalid staged file binding")
        data = base64.b64decode(result["files"][path], validate=True)
        if PUBLIC_SESSION_INPUT.encode() not in data:
            raise SpreadRefusal("public launch has no placeholder in its staged file binding")
        result["files"][path] = base64.b64encode(data.replace(PUBLIC_SESSION_INPUT.encode(), session.encode())).decode()
    return result


def native_execute(spec_path, result_out):
    spec = read_json(spec_path)
    if not spec or spec.get("schema") != 1:
        raise ValueError("invalid native peer specification")
    sys.path.insert(0, spec["control"])
    root = Path(spec["root"]).resolve()
    peer = dict(case_id=spec["case_id"], peer_id=spec["role"], lane=spec.get("lane"),
                run_root=str(root/spec.get("output_name", spec["role"])), controller_root=spec.get("controller_root"),
                ports=spec["case_ports"], executable_sha256=spec["executable_sha256"])
    with contextlib.ExitStack() as ownership:
        return _native_execute(spec_path, result_out, peer, ownership)


def _native_execute(spec_path, result_out, peer, ownership):
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
        preflight_payload(preflight_root/"payload.json", pool_peer=peer)
    else:
        preflight_payload(preflight_root/"payload.json")
    identity = read_json(preflight_root/"preflight.json")
    if identity["executable_sha256"] != spec["executable_sha256"]:
        raise SpreadRefusal("native executable hash differs from preparation")
    # Retain complete hash evidence while publishing a small live identity receipt.
    identity = {key: identity[key] for key in ("machine_id", "hostname", "os", "head", "executable_sha256")}
    spec = receive_public_launch(spec, control, identity, lambda:pool_worker.renew_claim(spec["claim"]))
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
    run = stage_native_run(make_run, ownership, box, peer, spec["repo"], spec["args"], out,
                           timeout=spec["timeout"], env=environment,
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
    elif spec.get("public_directory"):
        seed_settings(run, public_directory_settings(spec["repo"], spec["case_id"], role))
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
        # Older branch runners still get the installed native ceiling/CPU/floor
        # admission check, under its existing engine-start mutex.
        import box_load as native_load
        from pool import Needs
        with named_engine_cpu_wait(native_load, spec["box"], Needs(**spec["claim"]["needs"]),
                                   lambda:pool_worker.renew_claim(spec["claim"]),
                                   admission_seconds(spec['claim'])), \
                contextlib.nullcontext() if hooked else pool_run.launch_scope(run.argv, run.env) as scope, \
                contextlib.nullcontext() if hooked else native_load.admission(run.argv, run.env, run.record, run._save):
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
    record.update(topology="spread", box=box["name"], interface_version=INTERFACE_VERSION)
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
    if "--spread" in (sys.argv[1:] if argv is None else argv):
        raise SpreadUsageError()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    options = parser.parse_args(argv)
    try:
        return native_execute(options.native, options.out)
    except (Exception, SystemExit) as error:
        if isinstance(error, SystemExit) and error.code == 0:
            raise
        spec = read_json(options.native, {})
        record = dict(topology="spread", interface_version=INTERFACE_VERSION,
                      peer=spec.get("role"), box=spec.get("box", {}).get("name"),
                      exit_code=1, error=f"{type(error).__name__}: {error}")
        write_json(options.out/"peer-result.json", record)
        write_json(options.native.parent/"progress.json", dict(record=record))
        raise


if __name__ == "__main__":
    raise SystemExit(main())
