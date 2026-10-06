"""One pool-backed execution interface for cases with peers on separate machines.

Contract (version 1)
--------------------
run_case(repo, out, peers, match, *, drive=None, peer_boxes=None,
         dispatcher=None, registry=None) -> dict

``peers`` is a sequence of Peer objects. Each declares its name, OS, engine
count (one), required free memory, display size, quiet/exclusive requirement,
and whether its screen is reviewed on the controller's Windows machine. The
case supplies arguments, environment and fixtures through Peer or by calling
case.make_run() in ``drive(case)``. ``match`` is a Match with the game's port,
the lane's directory port, and optional unchanged case parameters. The pool
chooses and claims distinct machines, freezes committed inputs, verifies the
shipped/native executable hash, and starts native runners/tasks. ``drive``
still stages the case's scripts, orders starts and applies its own assertions.
Without ``drive``, the call starts host first and finishes every declared peer.
It collects verified evidence into ``out`` and returns topology="spread",
peer_boxes, native identities, executable hashes, records and driver_result.

prepare_case(...), the same arguments except drive, exposes the same Case for
drivers whose existing control loop needs runner-compatible handles. Always
use it in a with statement (or in @managed_case). Case.make_run() returns a
handle with start/finish/poll/terminate/close/suspend/resume. Levers execute in
the native runner owning that peer; a Windows suspend requires os="windows".
Case.synchronize() mirrors only declared JSON rendezvous files, logs and video
indices. Gameplay travels over real ICE sockets; SSH carries signaling and
evidence only. No router mapping is requested. Caller paths are private staging
paths; the helper maps them to the native case root, never to an owner's tree.

Refusals raise SpreadRefusal and write spread-result.json with topology,
peer_boxes, refused_peer, refused_box and the exact reason. Prefixes are
"spread peer <name> on <box>: <pool reason>", "no distinct fitting box",
"match port <port> differs from host port <port>", "native executable hash
differs from preparation", and "Windows process suspension is unavailable".
No fitting box means immediate refusal; the caller may retry after routing.
No case assertion, oracle, timeout or default single-box launch is changed.
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
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import threading
import time
import uuid

from run_sim_test import RUNTIME_SETTINGS, engine_executable, file_sha256


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

    def __post_init__(self):
        if not re.fullmatch(r"[A-Za-z0-9_-]+", self.name):
            raise ValueError("peer name must be a safe path component")
        if self.engines != 1:
            raise ValueError("a spread peer reserves exactly one engine on its own machine")


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


def atomic_bytes(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".incoming-" + uuid.uuid4().hex)
    try:
        temporary.write_bytes(data)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def write_json(path, value):
    atomic_bytes(path, (json.dumps(value, indent=2) + "\n").encode())


def read_json(path, default=None):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8-sig"))
    except (OSError, ValueError):
        return default


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
    text = str(value)
    # Probe JSON may carry Windows paths escaped with a second backslash.
    for old, new in sorted(mappings, key=lambda pair: len(pair[0]), reverse=True):
        text = text.replace(old.replace("\\", "\\\\"), new)
        text = text.replace(old, new).replace(old.replace("\\", "/"), new)
    return text


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
                    if key in ("wait_file", "signal", "file") and isinstance(child, str) and child.endswith(".json"):
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
            if target.is_relative_to(root) and target.suffix == ".json":
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
            handles = [case.make_run(repo, peer.args, Path(out)/peer.name, peer.timeout, env=peer.env,
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
        self.pool, self.transport_module, self.dispatcher = installed_pool(dispatcher)
        self.registry = Path(registry or self.dispatcher.with_name("boxes.json"))
        self.pins, self.peer_ports = pairs(peer_boxes), peer_ports or {}
        self.members, self.runs, self.tunnels, self.refusals, self.identities = {}, {}, [], [], {}
        self.lock = threading.RLock()
        self.stack = contextlib.ExitStack()
        self.closed = False
        self.last_sync = 0
        self.id = uuid.uuid4().hex
        self.control = self.out.parent/(".spread-" + self.id)
        self.control.mkdir(exist_ok=True)
        # The pool owns its own claims/control cache. Engine artifacts belong to
        # this lane's sole scratch root, not to the pool lane's run folders.
        self.lane_root = next((parent for parent in (self.out, *self.out.parents) if parent.parent.as_posix().casefold() == "d:/mx"), self.out.parent)
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
        # Until the pool's portable facts adapter lands, use that installed
        # adapter for control sources and this tree for immutable case inputs.
        source_repo = self.repo if (self.repo/"tools/box_facts.py").is_file() else Path(module.worker.facts.__file__).resolve().parents[1]
        backend = module.Transport(repo=source_repo, work=self.lane_root/".spread-inputs")
        backend.repo = self.repo
        adapter = Path(module.worker.facts.__file__).with_name("pool_run.py")
        if adapter.is_file():
            backend.sources["pool_run.py"] = adapter.read_text(encoding="utf-8")
            backend.control_id = hashlib.sha256(json.dumps(backend.sources, sort_keys=True).encode()).hexdigest()[:20]
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
        from cross_peers import require_distinct_machines
        used = []
        identities = {}
        catalog = self.pool.load_registry(self.registry)["boxes"]
        reviewed_box = next((box["name"] for box in catalog if box["kind"] == "local" and box["os"] == "windows"), None)
        # Reviewed and otherwise constrained peers are allocated first, retaining
        # original seat order for host selection and case scripts.
        ordered = sorted(self.peers, key=lambda peer: (not peer.reviewed, role_value(self.pins, self.names, peer.name) is None, peer.os == "any"))
        for peer in ordered:
            backend = self.backend()
            pin = role_value(self.pins, self.names, peer.name)
            if peer.reviewed:
                if pin and pin.casefold() != (reviewed_box or "").casefold():
                    raise self.refuse(peer.name, pin, "reviewed screen requires the controller's private Windows recorder")
                pin = reviewed_box
                if not pin:
                    raise self.refuse(peer.name, "unassigned", "reviewed screen requires a registered local Windows recorder")
            needs = self.pool.Needs(os=peer.os, engines=peer.engines, gpu=bool(peer.size), memory=peer.memory,
                                    alone=peer.quiet, size=peer.size, only_box=pin, excluded=tuple(used))
            fitting, reasons, states = self.pool.candidates(self.registry, needs, backend)
            request = dict(run_id=uuid.uuid4().hex, token=uuid.uuid4().hex, label=f"spread: {self.out.name}/{peer.name}",
                           owner=dict(pid=os.getpid(), machine=self.transport_module.worker.facts.machine_name(),
                                      process_start=self.transport_module.worker.facts.process_start(os.getpid())),
                           out=str(self.control/peer.name/"results"), command=[], hang_guard=max(600, peer.timeout + 300))
            chosen = None
            for _, box, state in fitting:
                try:
                    claim = backend.claim(box, needs, request)
                except Exception as error:
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
            hostname = chosen[0].get("hostname") or states[chosen[0]["name"]].get("hostname")
            if not hostname:
                raise self.refuse(peer.name, chosen[0]["name"], "native machine identity is unavailable")
            identities[peer.name] = dict(machine_id=hostname.casefold())
        require_distinct_machines(identities)
        for name in self.names:
            port = int(role_value(self.peer_ports, self.names, name) or self.match.port)
            if port != self.match.port:
                raise self.refuse(name, self.members[name][0]["name"], f"match port {port} differs from host port {self.match.port}")

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
                backend.rpc(box, "renew", dict(claim=claim), timeout=15)

    def prepare(self):
        for name in self.names:
            box, claim, request, backend = self.members[name]
            backend.active_claim, backend.active_box = claim, box
            backend.snapshot = getattr(self, "input_snapshot", None)
            backend.prepare(box, claim, request)
            self.input_snapshot = backend.snapshot
            native_root = box["scratch"].rstrip("/") + "/" + self.lane + "/native-" + self.id
            claim["case_root"] = native_root
            # Read the pool's port assignment. Each peer has a separate machine;
            # game ports remain in the driver's own lane block.
            if self.match.port in range(*[claim["ports"][0], claim["ports"][1] + 1]):
                raise self.refuse(name, box["name"], "driver match port overlaps the pool's control port map")
            self.guard()
        windows_hashes = {claim["exe_sha256"] for box, claim, _, _ in self.members.values() if box["os"] == "windows"}
        if len(windows_hashes) > 1:
            raise SpreadRefusal("Windows executable changed between peer shipments")

    def connect_directory(self):
        if len(self.peers) < 2:
            self.directory = None
            return
        from e2e.directory import serve
        directory_port = self.match.directory_port
        if directory_port is None:
            directory_port = self.members[self.names[0]][1]["directory_port"]
        self.directory = self.stack.enter_context(serve(self.control/"directory", directory_port, block=(directory_port, directory_port)))
        self.directory_port = directory_port
        for name in self.names:
            box, claim, _, _ = self.members[name]
            local = directory_port if box["kind"] == "local" else claim["directory_port"]
            claim["signal_port"] = local
            if box["kind"] == "local":
                continue
            log = (self.control/f"tunnel-{name}.log").open("ab")
            process = subprocess.Popen(["ssh", "-N", "-o", "BatchMode=yes", "-o", "ExitOnForwardFailure=yes", "-o", "ServerAliveInterval=15",
                                        "-R", f"127.0.0.1:{local}:127.0.0.1:{directory_port}", box["ssh"]],
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

    def make_run(self, repo, args, out, timeout=120, env=None, expected=None, *, runtime=None, fixtures=None):
        role = Path(out).name
        if role not in self.members:
            raise ValueError(f"undeclared spread peer {role}")
        if role in self.runs:
            raise ValueError(f"spread peer {role} already has a runner")
        handle = Run(self, repo, args, out, timeout, env or {}, expected or (), fixtures or (), runtime)
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
                    continue
                progress = json.loads(raw)
                handle.native_progress = progress
                if progress.get("identity"):
                    self.identities[handle.role] = progress["identity"]
                    from cross_peers import require_distinct_machines
                    require_distinct_machines(self.identities)
                for relative, encoded in progress.get("files", {}).items():
                    target = self.out/safe_relative(relative)
                    atomic_bytes(target, base64.b64decode(encoded, validate=True))
                if progress.get("record"):
                    handle.record.update(progress["record"])
            signals = {}
            for relative in self.signals():
                path = self.out/safe_relative(relative)
                if path.is_file():
                    data = path.read_bytes()
                    try:
                        json.loads(data)
                    except ValueError:
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
                    executable_hashes={name: item[1].get("exe_sha256") for name, item in self.members.items()},
                    identities=self.identities, records={name: run.record for name, run in self.runs.items()},
                    match=dict(port=self.match.port, parameters=self.match.parameters), refusals=self.refusals)

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
                if handle and handle.started and not handle.finished:
                    cleanup.callback(backend.stop, box, claim)
            self.save()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


class Run:
    """A native-runner handle with local staging paths for existing case logic."""
    def __init__(self, case, repo, args, out, timeout, env, expected, fixtures, runtime):
        self.case, self.repo, self.out = case, Path(repo).resolve(), Path(out).resolve()
        self.role, self.timeout, self.env = self.out.name, timeout, dict(env)
        self.expected, self.fixtures, self.retained = list(expected), list(fixtures), runtime
        self.cwd = self.out/"runtime"
        self.started = self.finished = False
        self.record, self.native_progress, self.sent = {}, {}, {}
        for name in ("Userdata", "Mods", "ScreenShots", "Temp"):
            (self.cwd/name).mkdir(parents=True, exist_ok=True)
        # Stage only private settings; the native runner creates its Data link.
        source = self.repo/"Userdata/Settings.ini"
        text = source.read_text(encoding="utf-8-sig") if source.is_file() else "SettingsMan\n"
        (self.cwd/"Userdata/Settings.ini").write_text(text, encoding="utf-8")
        from run_sim_test import seed_settings
        seed_settings(self, RUNTIME_SETTINGS)
        self.argv = [str(engine_executable(repo)), "-headless", *map(str, args)]
        write_json(self.out/"runtime.json", dict(executable=self.argv[0], cwd=str(self.cwd), settings_overrides=RUNTIME_SETTINGS,
                                                topology="spread", box=case.members[self.role][0]["name"]))

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
        session = None
        if self.case.directory and self.role != self.case.names[0]:
            session = self.case.published_session(self.role)
        if self.case.directory:
            if "-net-ice" in args:
                args[args.index("-net-ice") + 1] = "on"
            else:
                args += ["-net-ice", "on"]
            if "-net-join" in args:
                index = args.index("-net-join")
                args[index:index + 2] = ["-net-join-session", session]
        mappings = [(str(self.case.out), claim["case_root"]), (self.case.out.as_posix(), claim["case_root"]),
                    (str(self.repo), claim["repo"]), (self.repo.as_posix(), claim["repo"])]
        files = {}
        roots = [self.cwd, self.case.out/(self.role + "-stage"), self.case.out/(self.role + "-probe"), self.case.out/(self.role + "_probe")]
        paths = set(self.case.out.glob("*.txt"))
        for root in roots:
            if root.is_dir():
                paths.update(path for path in root.rglob("*") if path.is_file())
        for argument in [*args, *self.env.values()]:
            path = Path(str(argument))
            if path.is_file() and path.suffix.lower() in (".txt", ".json", ".lua", ".ini"):
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
                text = map_text(data.decode("utf-8-sig"), mappings)
                if session and "TextJoinAddress" in text:
                    text = re.sub(r"(?m)^(settext TextJoinAddress)\s+127\.0\.0\.1(?::\d+)?\s*$", lambda match: match[1] + " session:" + session, text)
                data = text.encode()
            files[path.relative_to(self.case.out).as_posix()] = base64.b64encode(data).decode()
        signals = self.case.signals()
        self.case.extra_signals = sorted(set(getattr(self.case, "extra_signals", ())) | set(signals))
        native = dict(schema=1, role=self.role, root=claim["case_root"], repo=claim["repo"], box=box,
                      control=claim["control"], claim=self.case.transport_module.Transport.native_claim(claim),
                      args=[map_text(argument, mappings) for argument in args],
                      env={key: map_text(value, mappings) for key, value in self.env.items()},
                      timeout=self.timeout, expected=[map_text(path, mappings) for path in self.expected],
                      fixtures=self.fixtures, files=files, signals=signals,
                      executable_sha256=claim["exe_sha256"], directory=self.case.directory,
                      signal_port=claim.get("signal_port"), session=session)
        if getattr(self, "private_menu", None) or getattr(self, "private_environment", None):
            raise self.case.refuse(self.role, box["name"], "private relay inputs require the existing credential delivery channel")
        # No credentials, directory private key or ticket bytes enter this spec.
        if native["directory"]:
            native["directory"] = {key: value for key, value in native["directory"].items() if key != "DIRECTORY_ROOT"}
        path = claim["root"] + "/peer-spec.json"
        backend.rpc(box, "write", dict(path=path, value=native))
        request["command"] = ["python", "{REPO}/tools/spread_peers.py", "--native", path, "--out", "{OUT}"]
        backend.launch(box, claim, request)
        self.started = True
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
            backend.guarded_run(["scp", "-q", "-o", "BatchMode=yes", box["ssh"] + ":" + manifest["path"], str(archive)], timeout=300)
        if file_sha256(archive) != manifest["sha256"]:
            raise self.case.refuse(self.role, box["name"], "fetched evidence archive hash differs")
        unpack_evidence(archive, destination, manifest["files"])
        for relative in manifest["files"]:
            source = destination/safe_relative(relative)
            # Runtime Data links never travel. Selected writable artifacts are
            # exported by the native owner as ordinary verified files.
            output_relative = relative.replace(self.role + "/runtime-evidence/", self.role + "/runtime/", 1)
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
        if action in ("suspend", "resume") and box["os"] != "windows":
            raise self.case.refuse(self.role, box["name"], "Windows process suspension is unavailable")
        token = uuid.uuid4().hex
        backend.rpc(box, "write", dict(path=claim["root"] + "/action.json", value=dict(action=action, token=token, **values)))
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            self.case.synchronize(force=True)
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


def publish_signals(root, signals, allowed):
    for relative, encoded in signals.items():
        if relative not in allowed:
            raise ValueError("only declared peer gate signals may be mirrored")
        data = base64.b64decode(encoded, validate=True)
        json.loads(data)
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
    out = root/role
    control = Path(spec_path).parent
    result_out = Path(result_out)
    result_out.mkdir(parents=True, exist_ok=True)
    # Existing cross-peer preflight supplies physical identity and input hashes.
    preflight_root = root/"preflights"/role
    preflight_root.mkdir(parents=True, exist_ok=True)
    box = dict(spec["box"], tree=spec["repo"], executable=os.environ["CCCP_TEST_BINARY"], scratch=str(root),
               kind="windows-local" if spec["box"]["kind"] == "local" else spec["box"]["kind"])
    write_json(preflight_root/"payload.json", dict(box=box, specs=[]))
    preflight_payload(preflight_root/"payload.json")
    identity = read_json(preflight_root/"preflight.json")
    if identity["executable_sha256"] != spec["executable_sha256"]:
        raise SpreadRefusal("native executable hash differs from preparation")
    # Keep the full existing hash preflight in the fetched artifact. Live gates
    # need only physical identity and the executable receipt, not every Data hash.
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
    run = make_run(spec["repo"], spec["args"], out, timeout=spec["timeout"], env=environment,
                   expected=spec["expected"], fixtures=spec["fixtures"])
    for relative, data in runtime_files.items():
        atomic_bytes(Path(run.cwd)/relative, data)
    if spec["directory"]:
        seed_settings(run, dict(SessionDirectoryUrl=f"127.0.0.1:{spec['signal_port']}",
                                SessionDirectoryCertSha256=spec["directory"]["DIRECTORY_PIN"],
                                SessionDirectoryInstallKey="spread-" + role, NetworkIceEnable="1",
                                NetworkConnectionMode="DirectOnly", NetworkHostRelayMode="Off", NetworkPortMapEnable="0"))
    for flag in ("-record-video", "-feel-measure"):
        if flag in run.argv:
            Path(run.argv[run.argv.index(flag) + 1]).mkdir(parents=True, exist_ok=True)
    action_receipt, archive_receipt = {}, None
    record = {}
    last_progress, action_token = 0, None
    try:
        # Older runners do not have the portable pool hooks yet. Use the same
        # installed launch scope, preserving exact PID/creation-time ownership.
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
                gameplay_signals(Path(run.argv[run.argv.index("-record-video") + 1]), root/(role + "-stage"))
            if time.monotonic() - last_progress >= .25:
                last_progress = time.monotonic()
                write_json(control/"progress.json", dict(identity=identity, pid=run.record["pid"], action=action_receipt,
                                                        files=native_snapshot(root, out, spec["signals"])))
            time.sleep(.05)
        record = run.finish()
    finally:
        run.close()
    record.update(topology="spread", box=box["name"])
    write_json(out/"record.json", record)
    # Export only private writable evidence, never the linked Data tree or a
    # credential/ticket. The existing acceptance archive verifies every byte.
    for directory, names, files in os.walk(run.cwd, followlinks=False):
        names[:] = [name for name in names if name not in ("Data", "Temp", ".git") and public_file(Path(directory)/name)]
        for name in files:
            source = Path(directory)/name
            if public_file(source):
                target = out/"runtime-evidence"/source.relative_to(run.cwd)
                atomic_bytes(target, source.read_bytes())
    # A peer archive contains only its own outputs and shared probe/stage files.
    # Native private inputs were scrubbed at staging and tickets stay private.
    archive_root = root/"exports"/role
    archive_root.mkdir(parents=True, exist_ok=True)
    candidates = [out, root/(role + "-stage"), root/(role + "-probe"), root/(role + "_probe"), preflight_root]
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
    archive_receipt = dict(path=str(archive), sha256=file_sha256(archive), files=files)
    write_json(result_out/"peer-result.json", dict(topology="spread", peer=role, box=box["name"], record=record,
                                               executable_sha256=spec["executable_sha256"], archive=archive_receipt))
    write_json(control/"progress.json", dict(identity=identity, pid=run.record["pid"], action=action_receipt, record=record,
                                            archive=archive_receipt, files=native_snapshot(root, out, spec["signals"])))
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
