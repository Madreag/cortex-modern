"""Play a scene on the named boxes with the existing game runners.

    python tools/e2e_peers.py --scenario fight15-s02 --out <capture> \
        --peer-boxes host=<BOX>,joiner=<BOX> --boxes-file <box facts>
    python tools/e2e_peers.py --scenario fight15-s11 --out <capture> \
        --peer-boxes host=<BOX>,joiner1=<BOX>,joiner2=<BOX>,joiner3=<BOX> --boxes-file <box facts>
    python tools/e2e_peers.py --check-spread <capture>
    python tools/e2e_peers.py --scenario mp-resume-from-disk-relay --out <capture> \
        --peer-boxes host=<BOX>,client=<BOX> --boxes-file <box facts> --one-box

Box facts stay outside the game tree. Each box supplies name, ssh, os, hostname,
repo, scratch, python and build_receipt. Windows also supplies session_dir and
task; a Mac or Linux player supplies binary and plays in a visible window.
The host is a Windows box; every other player may be Windows, a Mac or Linux.
Builds come from each box's own checkout. With --one-box every player plays on the
collector's own box (each box entry's ssh is "local"): a detector, never proof.
"""
from __future__ import annotations

import argparse
import base64
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import socket
import stat
import subprocess
import sys
import tarfile
import threading
import time
from types import SimpleNamespace
import urllib.parse
import urllib.request
import uuid

import e2e_video as video

POSIX_SYSTEMS = ("darwin", "linux")
PLAYER_SYSTEMS = ("win32", *POSIX_SYSTEMS)
PLAYER_SCRIPTS = {"darwin": ("/bin/zsh", "tools/e2e/peer_mac.sh"), "linux": ("/bin/bash", "tools/e2e/peer_linux.sh")}
# A player's name is a scene token's suffix ({PROBE_DIR_joiner2}) and a folder name on every box.
PLAYER_NAME = r"[a-z][a-z0-9]*"
SIGNAL_FILE = r"\{PROBE_DIR_(" + PLAYER_NAME + r")\}/([A-Za-z0-9_-]+\.(?:json|txt|mark))"


def headed_box(box):
    return box["os"] in POSIX_SYSTEMS or box.get("headed", False)


def read(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def scene(name):
    path = Path(name)
    if not path.is_file():
        path = video.SCENARIO_DIR / (name + ".json")
    document = read(path)
    if document.get("schema") != 1:
        raise ValueError("scene needs schema 1")
    document["path"] = str(path.resolve())
    return document


def committed_scene_sha256(repo, commit, path):
    path = Path(path).resolve()
    relative = path.relative_to(Path(repo).resolve()).as_posix()
    committed = subprocess.run(["git", "-C", str(repo), "show", commit + ":" + relative],
                               check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout
    if path.read_bytes().replace(b"\r\n", b"\n") != committed.replace(b"\r\n", b"\n"):
        raise ValueError("scene working file differs from its committed blob")
    return hashlib.sha256(committed).hexdigest()


def scene_runs(document):
    return document.get("runs") or [{"name": "run0", "peers": document["peers"]}]


def scene_roles(document, run):
    """The run's players, the host first: the host listens before anyone joins."""
    names = [peer["name"] for peer in run.get("peers", document.get("peers", []))]
    return ["host"] + [name for name in names if name != "host"]


def player_files(document, run):
    files = set()
    roles = scene_roles(document, run)
    for peer in run.get("peers", document.get("peers", [])):
        for key in ("menu_script", "probe", "input_script"):
            if peer.get(key):
                text = video.scenario_text(document, peer[key])
                for role, name in re.findall(SIGNAL_FILE, text):
                    if role in roles:
                        files.add(role + "-stage/probe/" + name)
    return sorted(files)


# A joiner names the lobby its own run's host wrote down; a row picked by port alone can be another host's lobby.
HOST_LOBBY = "{PROBE_DIR_host}/lobby.json"
DIRECTORY_AGENT = "cccp-scene-harness/1"
PORT_CHECK_KEY = "scene-harness-port-check"


def fetch_listing(url):
    request = urllib.request.Request(url, headers={"X-Install-Key": PORT_CHECK_KEY, "User-Agent": DIRECTORY_AGENT})
    with urllib.request.urlopen(request, timeout=20) as reply:
        return json.loads(reply.read().decode("utf-8"))


def directory_sessions(directory, fetch=None):
    """Every session the directory lists now, page by page."""
    rows, cursor = [], None
    for _ in range(50):
        query = "limit=200" + ("&cursor=" + urllib.parse.quote(cursor) if cursor else "")
        body = (fetch or fetch_listing)(f"https://{directory}/v1/sessions?{query}")
        rows += body.get("sessions", [])
        cursor = body.get("next_cursor")
        if not cursor:
            break
    return rows


def require_free_port(directory, port, fetch=None, wait_s=90, sleep=time.sleep, clock=time.monotonic):
    """A run starts only on a port no other lobby lists: a lobby that is closing is waited out, a live one refuses the run."""
    deadline = clock() + wait_s
    while True:
        held = [row for row in directory_sessions(directory, fetch) if row.get("listen_port") == port]
        if not held:
            return
        if clock() >= deadline:
            names = ", ".join(f"{row.get('name', '?')} ({row.get('activity', '?')}, session {row.get('session_id', '?')})" for row in held[:3])
            raise ValueError(f"port {port} is held by another lobby in {directory}: {names}")
        sleep(10)


def run_directory(document, run):
    """The directory the run's host lists its lobby in."""
    host = next(peer for peer in run.get("peers", document.get("peers", [])) if peer["name"] == "host")
    url = host.get("settings", {}).get("SessionDirectoryUrl", "")
    return urllib.parse.urlsplit(url if "://" in url else "https://" + url).netloc


def relay_by_hand(menu):
    """Whether a menu script picks Relay only on Settings > Network before it enters Multiplayer."""
    lines = menu.splitlines()
    picked = [index for index, line in enumerate(lines) if line.strip() == "combo_select ComboNetworkConnection Relay only"]
    entered = [index for index, line in enumerate(lines) if line.strip() == "activate ButtonMainToMultiplayer"]
    return bool(picked) and (not entered or picked[0] < entered[0])


# A gate names one fact of one player; a gate on another player's fact is met by the facts that player's box reports.
GATE_KEYS = ("start_when", "kill_when", "suspend_when")


def gate_list(value):
    return value if isinstance(value, list) else [value] if value else []


def gate_facts(document, run):
    """Every single fact a player's gate waits for on another player, by the player it names."""
    wanted = {}
    for peer in run.get("peers", document.get("peers", [])):
        for key in GATE_KEYS:
            for gate in gate_list(peer.get(key)):
                for part in video.gate_parts(gate):
                    if part.get("peer") and part["peer"] != peer["name"] and not part.get("run"):
                        wanted.setdefault(part["peer"], {})[fact_key(part)] = part
    return {role: list(parts.values()) for role, parts in wanted.items()}


def fact_key(part):
    return json.dumps(part, sort_keys=True)


def check_process_control(document, run, index, names):
    """A peer is dropped, hung and restarted where it runs: each control names only what its own box or a reported fact can answer."""
    earlier = [other.get("name", "run" + str(position)) for position, other in enumerate(scene_runs(document)[:index])]
    for reference in (run.get("resume_from"), run.get("start_when") if (run.get("start_when") or {}).get("run") else None):
        if reference and reference.get("run") not in earlier:
            raise ValueError("a run may wait for or resume from an earlier run only")
    for peer in run.get("peers", document.get("peers", [])):
        reference = peer.get("retain_runtime_from")
        if reference and reference.get("run") == run.get("name", "run" + str(index)):
            # A player restarted in its own run takes over an earlier player's runtime, on that player's box, once it has ended.
            if reference.get("peer") not in names or names.index(reference["peer"]) >= names.index(peer["name"]) or \
                    peer.get("start_when") != {"peer": reference["peer"], "ended": True}:
                raise ValueError("a player restarts from an earlier player of its run only once that player has ended")
        elif reference and (reference.get("run") not in earlier or reference.get("peer") != peer["name"]):
            raise ValueError("a player restarts only from its own runtime of an earlier run, or an earlier player's in its own run; "
                             "its save stays on its box")
        for key in GATE_KEYS:
            for gate in gate_list(peer.get(key)):
                for part in video.gate_parts(gate):
                    if part.get("run"):
                        if part["run"] not in earlier:
                            raise ValueError("a player may wait for an earlier run only")
                    elif part.get("peer") not in names:
                        raise ValueError("a gate names a player the run does not have")
                    elif part["peer"] != peer["name"] and key == "start_when" and peer["name"] == "host":
                        raise ValueError("the host starts first; its start cannot wait for another player")


def player_seats(document):
    """The seat each player plays: a player restarted in its run from an earlier player's runtime plays that player's seat, on its box."""
    seats = {}
    for index, run in enumerate(scene_runs(document) if document.get("runs") or document.get("peers") else []):
        for peer in run.get("peers", document.get("peers", [])):
            reference = peer.get("retain_runtime_from") or {}
            same_run = reference.get("run") == run.get("name", "run" + str(index))
            seats[peer["name"]] = seats.get(reference["peer"], reference["peer"]) if same_run else seats.get(peer["name"], peer["name"])
    return seats


def check_scene(document):
    if not document.get("public_directory"):
        raise ValueError("scene must use its public directory")
    roles = None
    for index, run in enumerate(scene_runs(document)):
        peers = run.get("peers", document.get("peers", []))
        names = [peer["name"] for peer in peers]
        if names.count("host") != 1 or len(peers) < 2 or len(set(names)) != len(names) or \
                not all(re.fullmatch(PLAYER_NAME, name) for name in names):
            raise ValueError("scene needs one host and at least one joiner per run, each with its own lowercase name")
        if roles is not None and set(names) != roles:
            raise ValueError("every run of the scene needs the same players")
        roles = set(names)
        for peer in peers:
            settings = peer.get("settings", {})
            scripts = [video.scenario_text(document, peer[key]) for key in ("menu_script", "probe", "input_script") if peer.get(key)]
            # A newcomer's scene starts on the shipped Automatic and picks Relay only on Settings > Network by hand.
            by_hand = "NetworkConnectionMode" not in settings and relay_by_hand(video.scenario_text(document, peer["menu_script"]) if peer.get("menu_script") else "")
            if (settings.get("NetworkConnectionMode") != "RelayOnly" and not by_hand) or settings.get("NetworkPortMapEnable") != 0:
                raise ValueError("scene must select RelayOnly and disable router mapping")
            if settings.get("NetworkHostRelayMode") != "Directory" or not settings.get("SessionDirectoryUrl"):
                raise ValueError("scene must get its relay from the directory")
            if any(re.search(r"\{(?:STAGE|VIDEO)_" + PLAYER_NAME + r"\}", text) for text in scripts):
                raise ValueError("scene needs another player's runtime files; this launcher cannot run it")
            for text in scripts:
                remaining = re.sub(SIGNAL_FILE, '', text)
                if re.search(r"\{PROBE_DIR_" + PLAYER_NAME + r"\}", remaining):
                    raise ValueError("another player's probe file must name one declared marker")
                if "GameRowPort" in text:
                    raise ValueError("a joiner picks its lobby by port alone; it must click its own host's lobby, PublicGameRowLobby:" + HOST_LOBBY)
        named = any("PublicGameRowLobby:" + HOST_LOBBY in video.scenario_text(document, peer[key])
                    for peer in peers for key in ("menu_script", "probe") if peer.get(key))
        host = next(peer for peer in peers if peer["name"] == "host")
        # A host with no menu (a dedicated server) names its listed lobby from its own listing line instead.
        writes = host.get("lobby_file") == "{PROBE_DIR}/lobby.json" or \
            (host.get("menu_script") and "write_lobby {PROBE_DIR}/lobby.json" in video.scenario_text(document, host["menu_script"]))
        if named and not writes:
            raise ValueError("the joiners name the host's lobby, but the host's menu never writes it (write_lobby {PROBE_DIR}/lobby.json)")
        check_process_control(document, run, index, names)


FACTS_FILE = "gate-facts.json"


class PlayerRun:
    def __init__(self, request, started):
        self.request, self.started = request, started

    def peer_fact(self, gate):
        """Whether another player's box has reported this fact of its player (the collector relays every report)."""
        try:
            facts = json.loads((Path(self.request["out"]) / FACTS_FILE).read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return False
        return fact_key(gate) in facts.get(gate["peer"], [])

    def make_run(self, repo, args, out, timeout, env=None, **kwargs):
        environment = dict(env or {})
        if self.request["box"]["os"] == "darwin":
            # A Mac engine started from a GUI job asks for local network consent; the ssh hop starts it outside that job.
            environment.update(CCCP_POSIX_HOP="ssh")
        run = video.make_run(repo, args, out, timeout, env=environment, **kwargs)
        run.record.update(box=self.request["box"]["name"], engine_hostname=socket.gethostname(), topology="spread", proof=False,
                          source_commit=self.request["commit"], scene_sha256=self.request["scene_sha256"])
        if headed_box(self.request["box"]):
            run.record.update(headed=True, headless_render_settings_applied=False)
        start = run.start

        def start_player():
            result = start()
            video.write_json(self.started, {"peer": self.request["role"], "box": self.request["box"]["name"],
                                           "engine_hostname": socket.gethostname(), "pid": run.record.get("pid")})
            return result

        run.start = start_player
        return run


def check_build(request):
    box = request["box"]
    if sys.platform != box["os"] or socket.gethostname().casefold() != box["hostname"].casefold():
        raise ValueError("player is on a different box from its named assignment")
    repo = Path(box["repo"])
    source = video.source_evidence(repo)
    if source["tip"] != request["commit"] or source["dirty"]:
        raise ValueError("player checkout differs from the committed scene launcher")
    if box["os"] in POSIX_SYSTEMS:
        os.environ["CCCP_TEST_BINARY"] = str(repo / box["binary"])
    receipt = read(box["build_receipt"])
    executable = video.file_evidence(video.capture_binary(repo))
    if receipt.get("commit") != request["commit"] or receipt.get("build_exit_code") != 0:
        raise ValueError("player build receipt differs from the committed scene launcher")
    if receipt.get("executable_sha256") != executable.get("sha256"):
        raise ValueError("player executable differs from its build receipt")
    if video.file_evidence(receipt["build_log"]).get("sha256") != receipt.get("build_log_sha256"):
        raise ValueError("player build log differs from its build receipt")
    return source, executable, receipt


def play(request_path):
    request = read(request_path)
    source, executable, receipt = check_build(request)
    document = scene(str(Path(request["box"]["repo"]) / request["scene_relative"]))
    if committed_scene_sha256(request["box"]["repo"], request["commit"], document["path"]) != request["scene_sha256"]:
        raise ValueError("player scene differs from the original scene")
    check_scene(document)
    # The runner's files are this run's; the scene's runs share one folder on the box, so a restart finds its own runtime.
    out, scene_out = Path(request["out"]), Path(request["scene_out"])
    role, index = request["role"], request["run_index"]
    run = copy.deepcopy(scene_runs(document)[index])
    name = run.get("name", "run" + str(index))
    # Every player's probe directory is a token here, though this box plays only its own player.
    roles = scene_roles(document, run)
    run["peers"] = [peer for peer in run.get("peers", document.get("peers", [])) if peer["name"] == role]
    for peer in run["peers"]:
        # A diagnostic lever the collector names reaches this player's engine; the scene itself is unchanged.
        peer["env"] = {**peer.get("env", {}), **request.get("engine_env", {}), **request.get("runner_env", {})}
    local_scene = copy.deepcopy(document)
    local_scene["checklist"] = [item for item in document.get("checklist", []) if not item.get("peer") or item["peer"] == role]
    headed = headed_box(request["box"])
    options = SimpleNamespace(repo=Path(request["box"]["repo"]), size=None, port=request["port"], fps=request["fps"],
                              setting=[], render_cap=60, headed=headed, fullstate_every=0, sheet_every=request["sheet_every"],
                              tokens={"PROBE_DIR_" + other: scene_out / name / (other + "-stage/probe")
                                      for other in roles},
                              resume_tokens=request.get("resume_tokens", {}), service_tokens={},
                              completed_runs=completed_runs(request, scene_out, role),
                              scratch_root=Path(request["box"]["scratch"]), scratch_limit_bytes=video.SCRATCH_LIMIT,
                              remote_capture=PlayerRun(request, out / (role + "-started.json")), shared_run_root=True)
    missing = video.requirement_findings(options.repo, document)
    if missing:
        raise ValueError("scene requirements failed: " + json.dumps(missing))
    video.check_scratch_budget(options.scratch_root, options.scratch_limit_bytes)
    for peer in run["peers"]:
        reference = peer.get("retain_runtime_from") or {}
        if reference.get("run") == name:
            # Its engine is staged on the earlier player's runtime, which that player's run fills only by playing.
            await_earlier_player(options.remote_capture, peer["start_when"], float(document.get("timeout_s", 480)))
    captured = video._run_one(options, local_scene, run, index, scene_out)
    captured.update(repo=str(options.repo), topology="spread", proof=False, peer_boxes={role: request["box"]["name"]})
    for peer in captured["peers"]:
        record_path = Path(peer["root"]) / "launch.json"
        record = read(record_path) if record_path.is_file() else {}
        peer["record"].update({key: record.get(key) for key in ("box", "engine_hostname", "source_commit", "scene_sha256")})
        peer.update(box=request["box"]["name"], box_hostname=request["box"]["hostname"],
                    native_source=source, native_exe=executable, build_receipt=receipt)
        if headed:
            peer.update(render_cap=None, presentation="headed")
    video.finish_run(local_scene, run, captured, source, options, shared_gates=False)
    (Path(captured["root"]) / "review.json").rename(Path(captured["root"]) / (role + "-review.json"))
    scan = Path(captured["root"]) / "secret-scan.json"
    if scan.is_file():
        scan.rename(Path(captured["root"]) / (role + "-secret-scan.json"))
    video.write_json(Path(captured["root"]) / (role + "-capture.json"), captured)
    video.write_json(out / (role + "-finished.json"), {"finished": video.stamp()})
    passed = all(video.peer_completed(peer) for peer in captured["peers"])
    print(f"{role}: {'GREEN' if passed else 'RED'} {captured['root']}", flush=True)
    return 0 if passed else 1


def await_earlier_player(remote, gate, timeout_s, poll_s=0.5):
    """A player restarted in its run starts only once the player it restarts from has ended on this box."""
    deadline = time.monotonic() + timeout_s
    while not remote.peer_fact(gate):
        if time.monotonic() >= deadline:
            raise ValueError(f"the player this one restarts from never ended: {gate}")
        time.sleep(poll_s)


def completed_runs(request, scene_out, role):
    """The scene's earlier runs as this box knows them: every player's end from the collector, this player's own run from
    its half here, where its runtime and save stay."""
    runs = []
    for earlier in request.get("completed_runs", []):
        half = scene_out / earlier["name"] / (role + "-capture.json")
        own = [peer for peer in (read(half)["peers"] if half.is_file() else []) if peer["peer"] == role]
        runs.append({"name": earlier["name"], "peers": own + [peer for peer in earlier["peers"] if peer["peer"] != role]})
    return runs


def checkpoint(request_path):
    """The save a resumed run restarts from, read on the box that wrote it."""
    request = read(request_path)
    half = read(Path(request["scene_out"]) / request["run_name"] / (request["role"] + "-capture.json"))
    peer = next(peer for peer in half["peers"] if peer["peer"] == request["role"])
    tokens, evidence = video.checkpoint_tokens(peer)
    return {"tokens": tokens, "evidence": evidence}


def spread_check(capture):
    reasons, observed = [], []
    if capture.get("topology") != "spread":
        reasons.append("capture topology is not spread")
    runs = capture.get("runs", [])
    if not runs:
        reasons.append("capture has no runs")
    # A player restarted from another player's runtime plays that player's seat on its box; every seat has its own box.
    seats = player_seats(capture["scenario_definition"]) if capture.get("scenario_definition") else {}
    for run in runs:
        placed = {}
        peers = run.get("peers", [])
        roles = set(run.get("peer_boxes", {}))
        if "host" not in roles or len(roles) < 2 or len(peers) != len(roles) or {peer.get("peer") for peer in peers} != roles:
            reasons.append(run["name"] + ": a player's record is absent")
        for peer in peers:
            label = run["name"] + "/" + peer["peer"]
            box = peer.get("box")
            path = Path(peer["root"]) / "launch.json"
            record = read(path) if path.is_file() else {}
            host = record.get("engine_hostname")
            placed.setdefault(seats.get(peer["peer"], peer["peer"]), set()).add((str(box or "").casefold(), str(host or "").casefold()))
            if not box or record.get("box") != box or run.get("peer_boxes", {}).get(peer["peer"]) != box:
                reasons.append(label + ": box name disagrees with the engine record")
            if not host or host.casefold() != str(peer.get("box_hostname", "")).casefold():
                reasons.append(label + ": engine host name disagrees with the assigned box")
            for key in ("pid", "exe_sha256", "engine_hostname", "box", "source_commit", "scene_sha256"):
                if not record.get(key) or record.get(key) != peer.get("record", {}).get(key):
                    reasons.append(label + ": retained engine record differs at " + key)
            if record.get("source_commit") != capture.get("source", {}).get("tip") or record.get("scene_sha256") != capture.get("scene_sha256"):
                reasons.append(label + ": engine source or scene differs from the capture")
            observed.append({"run": run["name"], "peer": peer["peer"], "box": box, "engine_hostname": host,
                             "pid": record.get("pid"), "engine_pid": (record.get("hop") or {}).get("engine_pid", record.get("pid")),
                             "record": str(path)})
        if any(len(spots) != 1 for spots in placed.values()):
            reasons.append(run["name"] + ": a restarted player ran on another box than the player it restarts from")
        if len({box for spots in placed.values() for box, _ in spots}) != len(placed):
            reasons.append(run["name"] + ": peers ran on one named box")
        if len({host for spots in placed.values() for _, host in spots}) != len(placed):
            reasons.append(run["name"] + ": peers ran on one engine host")
    return {"schema": 1, "passed": not reasons, "reasons": reasons, "peers": observed}


def move_paths(value, original, local, key=""):
    if key in ("args", "env", "record", "native_source", "native_exe", "build_receipt", "settings"):
        return value
    if isinstance(value, str):
        text = value.replace("\\", "/")
        old = str(original).replace("\\", "/").rstrip("/")
        if text == old or text.startswith(old + "/"):
            return str(local / text[len(old):].lstrip("/"))
        return value
    if isinstance(value, list):
        return [move_paths(item, original, local, key) for item in value]
    if isinstance(value, dict):
        return {name: move_paths(item, original, local, name) for name, item in value.items()}
    return value


def pack(request_path):
    request = read(request_path)
    runner, scene_out = Path(request["out"]), Path(request["scene_out"])
    archive = runner.with_suffix(".tgz")
    document = scene(str(Path(request["box"]["repo"]) / request["scene_relative"]))
    run = scene_runs(document)[request["run_index"]]
    run_name = run.get("name", "run" + str(request["run_index"]))
    # Every other player brings its own files, a restarted one sharing this folder too; this player packs only what it wrote.
    # A runtime, and the save in it, stays here.
    others = [other for other in scene_roles(document, run) if other != request["role"]]
    theirs = re.compile("(?:" + "|".join(map(re.escape, others)) + r")(?:$|[-._])") if others else None
    # A twenty-minute fight's tick history is mostly repeated hashes; compressed, the player's files cross the relay box's link faster.
    with tarfile.open(archive, "w:gz", compresslevel=6) as output:
        for directory, folders, files in os.walk(scene_out / run_name, followlinks=False):
            top = Path(directory) == scene_out / run_name
            folders[:] = [name for name in folders if name != "runtime" and not video.is_link(Path(directory) / name) and
                          not (top and theirs and theirs.match(name))]
            files = [name for name in files if not (top and theirs and theirs.match(name))]
            for name in files:
                path = Path(directory) / name
                if not video.is_link(path) and stat.S_ISREG(path.stat().st_mode):
                    output.add(path, arcname=path.relative_to(scene_out).as_posix(), recursive=False)
        for name in ("session.log", "session.done", "foreground_probe.py"):
            path = runner / name
            if path.is_file() and not video.is_link(path):
                output.add(path, arcname="commands/" + run_name + "-" + request["role"] + "-" + name, recursive=False)
    print(json.dumps(video.file_evidence(archive)), flush=True)


def unpack(archive, out):
    with tarfile.open(archive) as source:
        for member in source.getmembers():
            target = (out / member.name).resolve()
            if not member.isfile() or not target.is_relative_to(out) or "runtime" in Path(member.name).parts:
                raise ValueError("collected file escapes the capture or contains a runtime")
            if target.exists():
                raise ValueError("collected file would overwrite another player's evidence: " + member.name)
            target.parent.mkdir(parents=True, exist_ok=True)
            with source.extractfile(member) as stream, target.open("wb") as destination:
                import shutil
                shutil.copyfileobj(stream, destination)


# A box whose ssh is "local" is the collector's own: one of its players plays on the box the collector runs on.
LOCAL = "local"


def remote_command(box, args):
    args = list(map(str, args))
    return args if box["ssh"] == LOCAL else ["ssh", "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=20", box["ssh"], shlex.join(args)]


def ssh(box, args, **kwargs):
    return subprocess.run(remote_command(box, args), **kwargs)


def copy_file(box, source, destination, download=False):
    if box["ssh"] == LOCAL:
        import shutil
        shutil.copyfile(*((destination, source) if download else (source, destination)))
        return
    remote = box["ssh"] + ":" + str(destination).replace("\\", "/")
    args = [remote, str(source)] if download else [str(source), remote]
    subprocess.run(["scp", "-q", "-o", "BatchMode=yes", *args], check=True)


def wait_start(request_path):
    request = read(request_path)
    out, role = Path(request["out"]), request["role"]
    deadline = time.monotonic() + 86400
    while time.monotonic() < deadline:
        if (out / (role + "-started.json")).is_file():
            print("STARTED " + role, flush=True)
            return 0
        done = out / "session.done"
        if done.is_file() or (out / (role + "-finished.json")).is_file():
            print(done.read_text() if done.is_file() else "player ended before starting its engine", flush=True)
            return 1
        time.sleep(2)
    raise RuntimeError("player start hang guard expired")


def read_player_files(request_path):
    from spread_peers import complete_signal
    request = read(request_path)
    out, role = Path(request["out"]), request["role"]
    root = Path(request["scene_out"]) / request["run_name"]
    files = {}
    for relative in request.get("signal_files", []):
        path = root / relative
        if relative.startswith(role + "-stage/probe/") and path.is_file():
            data = path.read_bytes()
            if complete_signal(data, relative):
                files[relative] = base64.b64encode(data).decode()
    pace = root / role / video.PACE_REPORT
    try:
        early = json.loads(pace.read_text(encoding="utf-8")) if pace.is_file() else None
    except ValueError:
        early = None
    # The facts other players' gates wait for, as this player's own files show them; each stays met once met.
    facts = [fact_key(part) for part in request.get("gate_facts", []) if video.peer_gate_met(part, root)]
    return {"started": (out / (role + "-started.json")).is_file(), "files": files, "pace": early, "facts": facts}


def write_player_files(request_path, shared):
    from spread_peers import atomic_bytes, publish_signals
    request = read(request_path)
    out = Path(request["out"])
    facts = shared.get("facts", {})
    if not all(re.fullmatch(PLAYER_NAME, other) and other != request["role"] and isinstance(parts, list) and
               all(isinstance(part, str) for part in parts) for other, parts in facts.items()):
        raise ValueError("relayed facts must name another player's facts")
    # A gate may wait before its player's engine starts, so facts arrive from the start; files only once the run's folder exists.
    if facts:
        atomic_bytes(out / FACTS_FILE, json.dumps(facts, sort_keys=True).encode())
    if not shared.get("files"):
        return
    if not (out / (request["role"] + "-started.json")).is_file():
        raise ValueError("player has not started its scene")
    allowed = {relative for relative in request.get("signal_files", [])
               if not relative.startswith(request["role"] + "-stage/")}
    publish_signals(Path(request["scene_out"]) / request["run_name"], shared["files"], allowed)


def share_player_files(players, stop, errors, controls=None):
    sent = {role: {} for role in players}
    told = set()
    try:
        while not stop.is_set():
            states = {}
            for role, (box, request, process, remote, tool, log) in players.items():
                result = ssh(box, [box["python"], tool, "--read-files", remote], check=True, capture_output=True, text=True)
                states[role] = json.loads(result.stdout)
                early = states[role].get("pace")
                if early and role not in told:
                    # A player's fight fell below its pace gate; the collector says so now, not after the fight.
                    told.add(role)
                    print(role + ": " + early["line"], flush=True)
                    if controls:
                        video.write_json(controls / (request["run_name"] + "-" + role + "-" + video.PACE_REPORT), early)
            for role, (box, request, process, remote, tool, log) in players.items():
                files = {path: data for other, state in states.items() if other != role for path, data in state["files"].items()}
                facts = {other: state["facts"] for other, state in states.items() if other != role and state.get("facts")}
                incoming = {"files": files if states[role]["started"] else {}, "facts": facts}
                if (incoming["files"] or facts) and incoming != sent[role]:
                    ssh(box, [box["python"], tool, "--write-files", remote], check=True,
                        input=json.dumps(incoming), capture_output=True, text=True)
                    sent[role] = incoming
            stop.wait(2)
    except Exception as error:
        errors.append(str(error))


def launch(box, request_path, log):
    tool = str(Path(box["repo"]) / "tools/e2e_peers.py").replace("\\", "/")
    request = read(request_path)
    remote = request["out"] + "/request.json"
    ssh(box, [box["python"], tool, "--prepare", request["out"]], check=True)
    copy_file(box, request_path, remote)
    if box["os"] == "win32":
        args = ["pwsh", "-NoProfile", "-File", box["repo"] + "/tools/e2e/peer_box.ps1", "-Request", remote]
    else:
        shell, script = PLAYER_SCRIPTS[box["os"]]
        args = [shell, box["repo"] + "/" + script, remote, box["repo"]]
    process = subprocess.Popen(remote_command(box, args), stdout=log, stderr=subprocess.STDOUT)
    return process, remote, tool


def engine_levers(values):
    """Test levers for both players' engines; only CC_TEST_* names, which cost nothing when unset."""
    levers = {}
    for value in values or []:
        name, separator, setting = value.partition("=")
        if not separator or not re.fullmatch(r"CC_TEST_[A-Z0-9_]+", name):
            raise ValueError("--engine-env takes CC_TEST_NAME=VALUE")
        levers[name] = setting
    return levers


def box_runner_env(box):
    """The runner settings a boxes-file entry gives every engine its box plays: the test rig's placement, never a scene's."""
    settings = box.get("runner_env", {})
    if not isinstance(settings, dict) or not all(re.fullmatch(r"CC_RUNNER_[A-Z0-9_]+", name) and isinstance(value, str)
                                                 for name, value in settings.items()):
        raise ValueError(box["name"] + ": runner_env takes CC_RUNNER_NAME settings with text values")
    return dict(settings)


def selected_boxes(value, path, one_box=False, seats=None):
    """Each player's box. One box plays every player only as a detector: the players still split the way the runners split them.
    A player restarted from another player's runtime plays that player's seat, so it plays on that player's box."""
    assignments = dict(part.split("=", 1) for part in value.split(","))
    roles = ["host"] + [role for role in assignments if role != "host"]
    seats = {role: (seats or {}).get(role, role) for role in assignments}
    placed = {}
    for role, name in assignments.items():
        placed.setdefault(seats[role], set()).add(name.casefold())
    if not one_box and any(len(names) != 1 for names in placed.values()):
        raise ValueError("a restarted player plays on the box of the player it restarts from")
    if "host" not in assignments or len(placed) < 2 or \
            (not one_box and len({next(iter(names)) for names in placed.values()}) != len(placed)):
        raise ValueError("name a host and a different box for each player")
    entries = {box["name"].casefold(): box for box in read(path)["boxes"]}
    result = {role: entries[assignments[role].casefold()] for role in roles}
    for box in result.values():
        box_runner_env(box)
    if one_box:
        if len({box["hostname"].casefold() for box in result.values()}) != 1 or any(box["ssh"] != LOCAL for box in result.values()):
            raise ValueError("a one-box detector plays every player on the collector's own box")
        return result
    if result["host"]["os"] != "win32" or any(box["os"] not in PLAYER_SYSTEMS for box in result.values()):
        raise ValueError("this scene launcher needs a Windows host and Windows, Mac or Linux joiners")
    if len({result[role]["hostname"].casefold() for role in roles}) != len(placed):
        raise ValueError("two assignments name one engine host")
    return result


def wait_host_start(box, tool, remote, player, log, settle_s=10):
    """The host's engine is up before anyone joins. A player that ends before its engine starts (a refused build, a busy box)
    ends the wait with what it printed; no runner has to write a receipt for that."""
    waiter = subprocess.Popen(remote_command(box, [box["python"], tool, "--wait-start", remote]), stdout=subprocess.PIPE, text=True)
    ended = None
    while waiter.poll() is None:
        if player.poll() is not None:
            ended = ended or time.monotonic()
            if time.monotonic() - ended >= settle_s:
                waiter.terminate()
                waiter.wait()
                tail = Path(log).read_text(encoding="utf-8", errors="replace").strip().splitlines()[-1:] if Path(log).is_file() else []
                raise RuntimeError("host launch refused: the player ended before starting its engine" + (": " + tail[0] if tail else ""))
        time.sleep(1)
    if waiter.returncode:
        raise RuntimeError("host launch refused: " + waiter.stdout.read().strip())


def collect_player(out, name, role, player):
    box, request, process, remote, tool, log = player
    process.wait()
    log.close()
    result = ssh(box, [box["python"], tool, "--pack", remote], check=True, capture_output=True, text=True)
    package = json.loads(result.stdout)
    archive = out / "commands" / (name + "-" + role + ".tgz")
    copy_file(box, archive, package["path"], download=True)
    if video.file_evidence(archive).get("sha256") != package["sha256"]:
        raise ValueError("collected player files differ from their box")
    unpack(archive, out)
    archive.unlink()


# Windows refuses a path of 260 characters or more where long paths are off; a player's runtime holds the game's Data.
WINDOWS_PATH_LIMIT = 259


def longest_data_path(repo):
    return max((len(("Data" / path.relative_to(Path(repo) / "Data")).as_posix())
                for path in (Path(repo) / "Data").rglob("*") if path.is_file()), default=0)


def scene_folder(out_name, unique):
    """A player's folder for the whole scene on its box: every run of the scene lands there, a restart beside its earlier run."""
    return f"{out_name}-{unique}"


def check_windows_paths(boxes, out_name, run_name, repo, longest=None):
    """Refuse a run whose name would put a Windows player's game files past the path limit: the engine cannot read them there."""
    for role, box in boxes.items():
        if box["os"] != "win32":
            continue
        longest = longest_data_path(repo) if longest is None else longest
        runtime = f'{box["scratch"].rstrip("/")}/{scene_folder(out_name, "0" * 8)}/{run_name}/{role}/runtime/'
        if len(runtime) + longest > WINDOWS_PATH_LIMIT:
            raise ValueError(f"{box['name']}: the {role}'s game files would reach {len(runtime) + longest} characters, past Windows' "
                             f"{WINDOWS_PATH_LIMIT + 1}-character path limit; name the run shorter")


def scene_port(document, assigned=None):
    return video.port_for(0, assigned if assigned is not None else int(document.get("port_base", video.PORT_LO)))


def capture_scene(args):
    document = scene(args.scenario)
    check_scene(document)
    one_box = getattr(args, "one_box", False)
    boxes = selected_boxes(args.peer_boxes, args.boxes_file, one_box, player_seats(document))
    port = scene_port(document, getattr(args, "port", None))
    engine_env = engine_levers(getattr(args, "engine_env", None))
    runner_env = {role: box_runner_env(box) for role, box in boxes.items()}
    out, repo = args.out.resolve(), args.repo.resolve()
    source = video.source_evidence(repo)
    if source["dirty"]:
        raise ValueError("commit the scene launcher before playing")
    scene_path = Path(document["path"])
    relative = scene_path.relative_to(repo).as_posix()
    scene_hash = committed_scene_sha256(repo, source["tip"], scene_path)
    windows = {role: box for role, box in boxes.items() if box["os"] == "win32"}
    if windows:
        longest = longest_data_path(repo)
        for index, run in enumerate(scene_runs(document)):
            check_windows_paths(windows, out.name, run.get("name", "run" + str(index)), repo, longest)
    out.mkdir(parents=True, exist_ok=False)
    controls = out / "commands"
    controls.mkdir()
    started = time.monotonic()
    capture = {"schema": 1, "scenario": document["name"], "scenario_definition": document, "scene_sha256": scene_hash,
               "source": source, "exe": {"exists": False, "reason": "driver only; engines build on their boxes"},
               "repo": str(repo), "out": str(out), "platform": sys.platform, "fps": video.DEFAULT_FPS,
               "size": document.get("size"), "settings": {}, "render_cap": 60, "started": video.stamp(),
               "command": [sys.executable, *sys.argv], "scratch_root": str(out.parent),
               "scratch_limit_bytes": video.SCRATCH_LIMIT, "topology": "single-box" if one_box else "spread", "proof": False, "runs": [],
               "engine_env": engine_env, "runner_env": runner_env, "port": port}
    video.write_json(out / "capture.json", capture)
    # One folder per seat for the whole scene: a player restarted later, in its run or a later one, finds the runtime and save there.
    seats = player_seats(document)
    unique = {seat: uuid.uuid4().hex[:8] for seat in set(seats.values())}
    scene_outs = {role: box["scratch"].rstrip("/") + "/" + scene_folder(out.name, unique[seats[role]]) for role, box in boxes.items()}
    requests = {}
    for index, run in enumerate(scene_runs(document)):
        name = run.get("name", "run" + str(index))
        players, collected = {}, set()
        stop, file_errors, sharing = threading.Event(), [], None
        roles = scene_roles(document, run)
        try:
            if set(roles) != set(boxes):
                raise ValueError("the assigned boxes name other players than the scene: " + ", ".join(roles))
            if (run.get("start_when") or {}).get("run") and not video.cross_run_ready(capture["runs"], run["start_when"]):
                raise ValueError(f"previous run has not ended: {run['start_when']}")
            resume_tokens = {}
            if run.get("resume_from"):
                # The save is read where it was written; only its tokens and evidence come back.
                saved = requests[(run["resume_from"]["run"], run["resume_from"]["peer"])]
                result = ssh(saved[0], [saved[0]["python"], saved[2], "--checkpoint", saved[1]], check=True, capture_output=True, text=True)
                found = json.loads(result.stdout)
                resume_tokens = found["tokens"]
                video.write_json(controls / (name + "-checkpoint.json"), {"resume_from": run["resume_from"], **found})
            earlier = [{"name": done["name"], "peers": [{"peer": peer["peer"], "record": {"exit_code": peer["record"].get("exit_code")}}
                                                          for peer in done["peers"]]} for done in capture["runs"]]
            facts = gate_facts(document, run)
            require_free_port(run_directory(document, run), port)
            for role in roles:
                box = boxes[role]
                remote_out = scene_outs[role] + "/" + name + "-" + role + ".runner"
                request = dict(box=box, role=role, run_index=index, out=remote_out, scene_out=scene_outs[role], commit=source["tip"],
                               engine_env=engine_env, scene_relative=relative, scene_sha256=scene_hash, fps=video.DEFAULT_FPS,
                               sheet_every=15, port=port, completed_runs=earlier, resume_tokens=resume_tokens,
                               gate_facts=facts.get(role, []), run_name=name, signal_files=player_files(document, run))
                if runner_env[role]:
                    request["runner_env"] = runner_env[role]
                request_path = controls / (name + "-" + role + ".json")
                video.write_json(request_path, request)
                log = (controls / (name + "-" + role + ".log")).open("w", encoding="utf-8")
                process, remote, tool = launch(box, request_path, log)
                players[role] = (box, request, process, remote, tool, log)
                requests[(name, role)] = (box, remote, tool)
                if role == "host":
                    wait_host_start(box, tool, remote, process, controls / (name + "-host.log"))
            if player_files(document, run) or facts or any(video.pace_specs(document, name, role) for role in players):
                sharing = threading.Thread(target=share_player_files, args=(players, stop, file_errors, controls))
                sharing.start()
            for role, player in players.items():
                collected.add(role)
                collect_player(out, name, role, player)
            halves = [move_paths(read(out / name / (role + "-capture.json")), players[role][1]["scene_out"], out)
                      for role in roles]
            captured = {**halves[0], "peers": [peer for half in halves for peer in half["peers"]],
                        "peer_boxes": {role: boxes[role]["name"] for role in boxes}, "topology": "spread",
                        "interrupted": next((half["interrupted"] for half in halves if half.get("interrupted")), None)}
            capture["runs"].append(captured)
            check = spread_check(capture)
            # A one-box detector is never proof, whatever its placement check reads.
            captured.update(proof=check["passed"] and not one_box, topology="single-box" if one_box else "spread")
            for peer in captured["peers"]:
                peer.update(topology=captured["topology"], proof=captured["proof"])
            captured["repo"] = str(repo)
            # Each player brought its own history; the gates that compare players run once over all of them, as on one box.
            video.feel_probes({**document, **run}, captured, source)
            video.review(document, captured, out / name)
            video.write_json(out / "capture.json", capture)
        except Exception as error:
            capture["interrupted"] = str(error)
        finally:
            for role, player in players.items():
                box, request, process, remote, tool, log = player
                process.wait()
                log.close()
                if role not in collected:
                    collected.add(role)
                    try:
                        collect_player(out, name, role, player)
                    except Exception as error:
                        capture["interrupted"] += "; could not collect " + role + ": " + str(error)
            stop.set()
            if sharing:
                sharing.join()
            if file_errors:
                capture["interrupted"] = ((capture.get("interrupted") + "; ") if capture.get("interrupted") else "") + \
                                         "could not share the scene's declared player files: " + "; ".join(file_errors)
            video.write_json(out / "capture.json", capture)
        if capture.get("interrupted"):
            break
    check = spread_check(capture)
    video.write_json(out / "spread-check.json", check)
    video.scenario_manifest(capture, out, time.monotonic() - started)
    review = video.aggregate_review(capture, out)
    if capture.get("interrupted"):
        review["run_findings"].append({"class": "harness", "reason": capture["interrupted"]})
    if one_box:
        review.update(topology="single-box", proof=False)
    elif not check["passed"]:
        review["run_findings"].append({"class": "harness", "reason": "; ".join(check["reasons"])})
    for peer in review["peers"]:
        peer.update(box=boxes[peer["peer"]]["name"], engine_hostname=boxes[peer["peer"]]["hostname"])
    passed = (check["passed"] or one_box) and all(video.peer_completed(peer) for run in capture["runs"] for peer in run["peers"])
    passed &= not review["run_findings"] and not any(item.get("finding") or item.get("blocked_by") for item in review["checklist"])
    review["scripted_result"] = "GREEN" if passed else "RED"
    video.write_json(out / "review.json", review)
    for run in capture["runs"]:
        for peer in run["peers"]:
            peer.pop("index", None)
    video.write_json(out / "capture.json", capture)
    print(f"{document['name']}: {review['scripted_result']} capture={out / 'capture.json'} review={out / 'review.json'}", flush=True)
    return 0 if passed else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--scenario")
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path)
    parser.add_argument("--peer-boxes")
    parser.add_argument("--boxes-file", type=Path, default=os.environ.get("CORTEX_SCENE_BOXES"))
    parser.add_argument("--port", type=int, help=f"this run's lobby port inside {video.PORT_LO}-{video.PORT_HI}; defaults to the scene's port_base")
    parser.add_argument("--engine-env", action="append", default=[], metavar="CC_TEST_NAME=VALUE",
                        help="a test lever set in both players' engines; recorded in the capture")
    parser.add_argument("--one-box", action="store_true",
                        help="play every player on this box, each through its own player run: a detector, never proof")
    parser.add_argument("--check-spread", type=Path)
    parser.add_argument("--checkpoint", type=Path)
    parser.add_argument("--play", type=Path)
    parser.add_argument("--pack", type=Path)
    parser.add_argument("--prepare", type=Path)
    parser.add_argument("--wait-start", type=Path)
    parser.add_argument("--read-files", type=Path)
    parser.add_argument("--write-files", type=Path)
    args = parser.parse_args()
    if args.prepare:
        args.prepare.mkdir(parents=True, exist_ok=False)
        return 0
    if args.wait_start:
        return wait_start(args.wait_start)
    if args.read_files:
        print(json.dumps(read_player_files(args.read_files)), flush=True)
        return 0
    if args.write_files:
        write_player_files(args.write_files, json.load(sys.stdin))
        return 0
    if args.checkpoint:
        print(json.dumps(checkpoint(args.checkpoint)), flush=True)
        return 0
    if args.play:
        return play(args.play)
    if args.pack:
        pack(args.pack)
        return 0
    if args.check_spread:
        check = spread_check(read(args.check_spread / "capture.json"))
        print("GREEN: peers ran on separate boxes" if check["passed"] else "RED: " + "; ".join(check["reasons"]))
        return 0 if check["passed"] else 1
    if not (args.scenario and args.out and args.peer_boxes and args.boxes_file):
        parser.error("supply --scenario, --out, --peer-boxes and --boxes-file")
    return capture_scene(args)


if __name__ == "__main__":
    raise SystemExit(main())
