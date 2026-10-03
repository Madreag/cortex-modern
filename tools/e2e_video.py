"""Play one end-to-end scenario on private hidden desktops and leave a video, a contact sheet and a review file.

The engine writes the frames (`-record-video <dir>`); this driver launches every peer a scenario needs through
run_sim_test.make_run, waits, encodes each peer's frames with ffmpeg, tiles a contact sheet and writes review.json:
the scenario's checklist, each item with the frame range the capture puts it in and the assertion its probe made.
A reviewing agent reads review.json first, then the contact sheet, then the video.

    python tools/e2e_video.py --repo <tree> --out <dir> --scenario mp-host-join [--size 960x540] [--fps 30]
    python tools/e2e_video.py --repo <tree> --review-only <dir>

Ports: this driver owns 49400-49479 and hands each scenario run a slice of it; the readback detector owns
49180-49199, the launch driver 48320-48539 and 48630-48649, and no scenario may reach outside its own slice.
Every engine launch goes through the runners with CCCP_HEADLESS=1; nothing here ever creates the process itself.
"""

import argparse
from contextlib import nullcontext
from datetime import datetime, timedelta, timezone
import hashlib
import bisect
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import threading
import time
import uuid

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from run_sim_test import make_run, seed_settings  # noqa: E402
from compare_sim_traces import compare_fullstate, CORE  # noqa: E402
from net_lobby_wire import session_protocol  # noqa: E402

SCENARIO_DIR = TOOLS / "e2e"
PORT_LO, PORT_HI = 49400, 49479
DEFAULT_PORT_HI = PORT_HI
DEFAULT_SIZE = "960x540"
DEFAULT_FPS = 30
SHEET_COLUMNS = 6
SHEET_THUMB_WIDTH = 320
FFMPEG_CANDIDATES = (
    r"C:\Users\egerm\scoop\shims\ffmpeg.exe",
    "/opt/homebrew/bin/ffmpeg",
    "/usr/local/bin/ffmpeg",
    "/usr/bin/ffmpeg",
)
SCRATCH_LIMIT = 5_000_000_000


def setting_pair(value):
    key, separator, setting = value.partition("=")
    if not separator or not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", key) or any(c in setting for c in "\r\n"):
        raise argparse.ArgumentTypeError("settings require KEY=VALUE on one line")
    return key, setting


def render_cap_hz(value):
    if str(value) not in ("0", "60"):
        raise argparse.ArgumentTypeError("[feel] invalid render settings: expected RenderCapHz = 0 or 60")
    return int(value)


def positive_bytes(value):
    try:
        result = int(value)
    except (TypeError, ValueError):
        raise argparse.ArgumentTypeError("scratch limit must be a positive integer byte count") from None
    if result <= 0:
        raise argparse.ArgumentTypeError("scratch limit must be a positive integer byte count")
    return result


def scratch_limit(options, capture=None):
    selected = getattr(options, "scratch_limit_bytes", None)
    return positive_bytes(selected if selected is not None else (capture or {}).get("scratch_limit_bytes", SCRATCH_LIMIT))


def check_scratch_budget(root, limit):
    footprint = scratch_bytes(root)
    if footprint >= limit:
        raise RuntimeError(f"scratch footprint {footprint} bytes reaches {limit}; no cleanup performed")
    return footprint


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def file_evidence(path):
    path = Path(path)
    if not path.is_file():
        return {"path": str(path), "exists": False}
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return {"path": str(path), "bytes": path.stat().st_size, "sha256": digest.hexdigest()}


def stamp():
    clock = subprocess.check_output(["date", "-u", "+%Y-%m-%dT%H:%M:%S"], text=True).strip()
    utc = datetime.strptime(clock, "%Y-%m-%dT%H:%M:%S").replace(tzinfo=timezone.utc)
    return utc.astimezone(timezone(timedelta(hours=-7))).strftime("%Y-%m-%d %I:%M:%S %p MST")


def parse_stamp(value):
    for form in ('%Y-%m-%d %I:%M:%S %p MST', '%Y-%m-%d %H:%M:%S MST'):
        try:
            return datetime.strptime(value, form)
        except ValueError:
            pass
    raise ValueError(f'invalid capture timestamp: {value}')


def scratch_bytes(root):
    total, pending = 0, [Path(root)]
    while pending:
        directory = pending.pop()
        if not directory.exists():
            continue
        with os.scandir(directory) as entries:
            for entry in entries:
                info = entry.stat(follow_symlinks=False)
                if getattr(info, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400) or entry.is_symlink():
                    continue
                if stat.S_ISDIR(info.st_mode):
                    pending.append(Path(entry.path))
                elif stat.S_ISREG(info.st_mode):
                    total += info.st_size
    return total


def note_footprint(root, peak):
    """A live run's footprint is measured, never enforced: its transients are retired once its verdict is written."""
    return max(peak, scratch_bytes(root))


def is_link(path):
    info = os.lstat(path)
    return stat.S_ISLNK(info.st_mode) or bool(getattr(info, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400))


def retire_transients(capture_run):
    """After the run's verdict: PNG frames give way to the encoded MP4 and sheet, and closed feel records are packed losslessly."""
    from feel.records import compress_closed_record
    receipts = []
    for peer in capture_run["peers"]:
        root, frames = Path(peer["root"]), Path(peer["video_dir"]) / "frames"
        encoded = peer.get("video") and Path(peer["video"]).is_file() and peer.get("contact_sheet") and Path(peer["contact_sheet"]).is_file()
        if encoded and frames.is_dir() and not is_link(frames):
            count, size = 0, 0
            for path in frames.glob("frame-*.png"):
                if is_link(path):
                    continue
                size += path.stat().st_size
                path.unlink()
                count += 1
            receipts.append({"peer": peer["peer"], "kind": "frames", "files": count, "bytes": size,
                             "kept": [peer["video"], peer["contact_sheet"]]})
        raw = root / "feel" / "raw.jsonl"
        if raw.is_file() and not is_link(raw) and not is_link(raw.parent):
            try:
                receipts.append({"peer": peer["peer"], "kind": "feel-record", **compress_closed_record(raw, root)})
            except (OSError, ValueError, RuntimeError) as error:
                receipts.append({"peer": peer["peer"], "kind": "feel-record", "original": str(raw), "error": f"{type(error).__name__}: {error}"})
    capture_run["retired"] = receipts
    return receipts


def retained_footprint(capture_run, root, limit):
    """The cap judges what a run leaves behind: the retained set after its transients were retired."""
    capture_run["retained"] = {"root": str(root), "bytes": scratch_bytes(root), "limit": limit}
    return capture_run["retained"]


def finish_run(scenario, run, captured, source, options):
    """A run's verdict first, whatever its footprint; then its transients are retired and the retained set is measured."""
    feel_probes({**scenario, **run}, captured, source)
    render(captured, options.fps, options.sheet_every)
    retire_transients(captured)
    retained_footprint(captured, options.scratch_root, options.scratch_limit_bytes)
    return review(scenario, captured, Path(captured["root"]))


def source_evidence(repo):
    package = Path(repo) / "MANIFEST.json"
    if not (Path(repo) / ".git").exists() and package.is_file():
        # An unpacked release package is no git tree: its manifest names the commit and tree it was built from.
        manifest = json.loads(package.read_text(encoding="utf-8"))
        return {"tip": manifest["source"]["commit"], "tree": manifest["source"]["tree"], "package": manifest["tag"],
                "dirty": ["package built from a modified tree"] if manifest["source"]["dirty"] else [], "diff_sha256": None}

    def git(*args):
        return subprocess.check_output(["git", "-C", str(repo), *args], text=True).strip()
    return {"tip": git("rev-parse", "HEAD"), "dirty": git("status", "--porcelain").splitlines(),
            "diff_sha256": hashlib.sha256(git("diff", "HEAD").encode()).hexdigest()}


def capture_binary(repo):
    if sys.platform == "win32":
        return Path(repo).resolve() / "Cortex Command.exe"
    from posix_test_runner import resolve_binary
    return resolve_binary(repo)


def find_ffmpeg():
    """PATH first, then the two machines' known locations; None when the encode has to be skipped."""
    found = shutil.which("ffmpeg")
    if found:
        return found
    for candidate in FFMPEG_CANDIDATES:
        if Path(candidate).is_file():
            return candidate
    return None


def find_ffprobe(ffmpeg):
    """The ffprobe installed beside the ffmpeg in use, which a shell without that directory on PATH still finds; else PATH."""
    if ffmpeg and Path(ffmpeg).parent != Path("."):
        sibling = Path(ffmpeg).with_name("ffprobe" + Path(ffmpeg).suffix)
        if sibling.is_file():
            return str(sibling)
    return shutil.which("ffprobe")


def load_scenario(name):
    path = SCENARIO_DIR / f"{name}.json"
    if not path.is_file():
        raise SystemExit(f"no such scenario: {path}")
    scenario = json.loads(path.read_text(encoding="utf-8"))
    if scenario.get("schema") != 1:
        raise SystemExit(f"{path}: schema 1 expected, found {scenario.get('schema')!r}")
    scenario["path"] = str(path)
    return scenario


def scenario_text(scenario, key):
    """A script the scenario either spells out inline or keeps beside itself in tools/e2e/."""
    inline = scenario.get("scripts", {}).get(key)
    if inline is not None:
        return inline if isinstance(inline, str) else "".join(inline)
    path = SCENARIO_DIR / key
    if not path.is_file():
        raise SystemExit(f"{scenario['path']}: no script {key} inline or at {path}")
    return path.read_text(encoding="utf-8")


def source_tokens(repo):
    """The tree's own wire values a scenario may name; a tree without the headers names none."""
    try:
        return {"NET_PROTOCOL_VERSION": session_protocol(repo).value}
    except (OSError, RuntimeError):
        return {}


def substitute(value, tokens):
    if isinstance(value, str):
        for name, replacement in tokens.items():
            value = value.replace("{" + name + "}", replacement.as_posix() if isinstance(replacement, Path) else str(replacement))
        return value
    if isinstance(value, list):
        return [substitute(item, tokens) for item in value]
    if isinstance(value, dict):
        return {key: substitute(item, tokens) for key, item in value.items()}
    return value


def supplied_tokens(arguments=(), environment=None):
    environment = os.environ if environment is None else environment
    result = {name: environment[name] for name in ("TURN_SERVER", "TURN_USER", "TURN_PASS", "HOST_ADDRESS") if environment.get(name)}
    for argument in arguments:
        name, separator, value = argument.partition("=")
        if not separator or not re.fullmatch(r"[A-Z][A-Z0-9_]*", name) or not value:
            raise ValueError("tokens require a nonempty NAME=VALUE")
        result[name] = value
    return result


def prior_run(captures, name):
    return next((run for run in captures if run["name"] == name), None)


def prior_peer(captures, reference):
    run = prior_run(captures, reference["run"])
    return next((peer for peer in run["peers"] if peer["peer"] == reference["peer"]), None) if run else None


def cross_run_ready(captures, condition):
    names = condition.get("peers", [condition.get("peer")])
    run = prior_run(captures, condition["run"])
    if not run or not condition.get("ended"):
        return False
    return all(any(peer["peer"] == name and peer["record"].get("exit_code") is not None for peer in run["peers"]) for name in names)


def checkpoint_tokens(peer):
    runtime = Path(peer.get("runtime", Path(peer["root"]) / "runtime"))
    store = runtime / "Autosaves"
    candidates = []
    for manifest in store.glob("*.ccmanifest"):
        fields = dict(re.findall(r"(?m)^([A-Za-z]+)\s*=\s*([^\r\n]+)", manifest.read_text(encoding="utf-8")))
        match, tick_text = manifest.stem.rsplit("-", 1)
        tick = int(tick_text)
        if fields.get("SavedTick") != str(tick) or not manifest.with_suffix(".ccsave").is_file() or not (store / f"{match}.admission").is_file():
            continue
        if fields.get("MatchId", match) != match:
            continue
        candidates.append((tick, match, manifest))
    if not candidates:
        raise ValueError(f"No retained checkpoint and restart manifest in {store}")
    tick, match, manifest = max(candidates)
    evidence = [file_evidence(path) for path in (manifest, manifest.with_suffix(".ccsave"), store / f"{match}.admission", runtime / "Userdata/NetworkIdentity.key")]
    return {"RESUME_MATCH": match, "RESUME_TICK": tick}, evidence


def run_preflight(scenario, run, captures, tokens):
    peers = run.get("peers") or scenario.get("peers", [])
    for node in [run, *peers]:
        kill_gate = node.get("kill_when")
        # A list of gates drops the peer at the first one met.
        for gate in (kill_gate if isinstance(kill_gate, list) else [kill_gate] if kill_gate else []):
            if gate.get("peer") not in {peer["name"] for peer in peers} or \
                    [bool(gate.get("event")), gate.get("probe_complete") is True, bool(gate.get("log")), "sim_tick" in gate].count(True) != 1:
                return {"class": "harness", "reason": "A process-drop gate must name a peer and one of an event, a completed probe, a log line or a sim tick"}
        condition = node.get("start_when", {})
        if condition.get("run") and not cross_run_ready(captures, condition):
            return {"class": "harness", "reason": f"Previous run has not ended: {condition}"}
        reference = node.get("retain_runtime_from")
        same_run = reference and reference["run"] == run.get("name", "run0")
        if same_run:
            names = [peer["name"] for peer in peers]
            if reference["peer"] not in names or node.get("name") not in names or names.index(reference["peer"]) >= names.index(node["name"]) or node.get("start_when") != {"peer": reference["peer"], "ended": True}:
                return {"class": "harness", "reason": "A same-run runtime may be reused only after its earlier owner ends"}
        elif reference and (not prior_peer(captures, reference) or not cross_run_ready(captures, {**reference, "ended": True})):
            return {"class": "harness", "reason": f"Retained runtime is unavailable: {reference}"}
    builtins = {"REPO", "PORT", "OUT", "SIZE", "WIDTH", "HEIGHT", "FPS", "PEER", "STAGE", "PROBE_DIR", "MENU_SCRIPT", "INPUT_SCRIPT", "VIDEO", "DIRECTORY_URL", "DIRECTORY_PIN", "DIRECTORY_ROOT", "DIRECTORY_SESSION",
                "NET_PROTOCOL_VERSION"}
    builtins.update(f"{prefix}_{peer['name']}" for peer in peers for prefix in ("STAGE", "PROBE_DIR", "VIDEO"))
    body = json.dumps(peers)
    for peer in peers:
        for key in ("menu_script", "probe", "input_script"):
            if peer.get(key):
                body += scenario_text(scenario, peer[key])
    needed = set(re.findall(r"\{([A-Z][A-Za-z0-9_]*)\}", body)) - builtins
    missing = sorted(name for name in needed if name not in tokens)
    if missing:
        return {"class": "harness", "reason": "Unresolved tokens: " + ", ".join(missing), "tokens": missing}
    return {"class": run.get("blocker_class", "engine"), "reason": run["blocked_by"]} if run.get("blocked_by") else None


def directory_session(root, port):
    """The session id this run's own directory lists for the run's port, once its host has registered."""
    path = Path(root) / "listed.json" if root else None
    try:
        rows = json.loads(path.read_text(encoding="utf-8")).get("sessions", []) if path and path.is_file() else []
    except ValueError:
        return None
    return next((row["session_id"] for row in rows if row.get("listen_port") == port and row.get("session_id")), None)


def bind_directory_session(staged_menu, session):
    """A peer that joins by session id gets it written into its staged script just before it starts."""
    path = Path(staged_menu)
    if path.is_file() and "{DIRECTORY_SESSION}" in path.read_text(encoding="utf-8"):
        path.write_text(path.read_text(encoding="utf-8").replace("{DIRECTORY_SESSION}", session), encoding="utf-8")
        return True
    return False


def process_alive(pid):
    """Whether a process with this id still runs; a pid that cannot be opened for that reason is gone."""
    if sys.platform == "win32":
        import ctypes
        kernel = ctypes.windll.kernel32
        handle = kernel.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not handle:
            return kernel.GetLastError() != 87  # ERROR_INVALID_PARAMETER: no such process
        code = ctypes.c_ulong()
        kernel.GetExitCodeProcess(handle, ctypes.byref(code))
        kernel.CloseHandle(handle)
        return code.value == 259  # STILL_ACTIVE
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


PORT_CLAIMS = Path(os.environ.get("TEMP") or os.environ.get("TMPDIR") or "/tmp") / "e2e-video-ports"


class PortClaim:
    """A run's ports, claimed on this machine for its lifetime. Two captures of one scenario at once share its ports, and a
    host of the second then answers to the first run's directory; a claim a live process holds refuses the second run."""

    def __init__(self, ports):
        self.ports = sorted({port for port in ports if port})
        self.taken = []
        self.guards = []
        self.receipt = f'{os.getpid()} {uuid.uuid4().hex}\n'

    def __enter__(self):
        if self.taken:
            return self
        PORT_CLAIMS.mkdir(parents=True, exist_ok=True)
        for port in self.ports:
            path = PORT_CLAIMS / f"{port}.claim"
            guard = os.open(PORT_CLAIMS / f'{port}.guard', os.O_CREAT | os.O_RDWR)
            try:
                if os.name == 'nt':
                    import msvcrt
                    msvcrt.locking(guard, msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(guard, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                os.close(guard)
                self.__exit__(None, None, None)
                raise RuntimeError(f'port {port} is owned by another claim') from None
            self.guards.append(guard)
            for _ in range(2):
                try:
                    handle = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
                except FileExistsError:
                    try:
                        holder = int(path.read_text(encoding="utf-8").split()[0])
                    except (OSError, ValueError, IndexError):
                        self.__exit__(None, None, None)
                        raise RuntimeError(f'port {port} has an incomplete ownership receipt') from None
                    if holder <= 0 or process_alive(holder):
                        self.__exit__(None, None, None)
                        raise RuntimeError(f"port {port} is claimed by the live capture process {holder}")
                    path.unlink(missing_ok=True)
                    continue
                try:
                    data = self.receipt.encode()
                    while data:
                        written = os.write(handle, data)
                        if written <= 0: raise OSError('claim receipt write made no progress')
                        data = data[written:]
                except OSError:
                    self.__exit__(None, None, None)
                    raise
                finally:
                    os.close(handle)
                self.taken.append(path)
                break
            else:
                self.__exit__(None, None, None)
                raise RuntimeError(f"port {port} could not be claimed")
        return self

    def __exit__(self, *exc):
        for path in self.taken:
            try:
                if path.read_text(encoding='utf-8') == self.receipt: path.unlink()
            except OSError:
                pass
        self.taken = []
        for guard in self.guards:
            os.close(guard)
        self.guards = []
        return False


def directory_port_for(scenario, run, base):
    """The run's session directory port: the scenario's place below the top of the driver's block, so a lane's own block
    carries it with the runs. None when the scenario serves no directory."""
    named = run.get("directory_port", scenario.get("directory_port"))
    if not named:
        return None
    port = PORT_HI - (DEFAULT_PORT_HI - int(named))
    runs = {base + index for index in range(len(scenario.get("runs", [])))}
    if not PORT_LO <= port <= PORT_HI or port in runs:
        raise SystemExit(f"{scenario['name']}: directory port {port} is outside {PORT_LO}-{PORT_HI} or on a run's port")
    return port


def port_for(run_index, base):
    """One port per run; every peer of a run shares the host's. Kept inside this driver's own block."""
    port = base + run_index
    if not PORT_LO <= port <= PORT_HI:
        raise SystemExit(f"port {port} is outside this driver's block {PORT_LO}-{PORT_HI}")
    return port


def parse_freezedetect(text):
    """The still spans ffmpeg's freezedetect reports, in seconds of the video: (start, end); a span still open at the end runs to None."""
    spans, start = [], None
    for line in text.splitlines():
        found = re.search(r"freeze_(start|end): ([0-9.]+)", line)
        if not found:
            continue
        if found.group(1) == "start":
            start = float(found.group(2))
        elif start is not None:
            spans.append((start, float(found.group(2))))
            start = None
    if start is not None:
        spans.append((start, None))
    return spans


# The engine marks the moment the harness ends a round by its tick cap; the screen then holds its last picture while
# the run drains its relay, lingers and writes its records, and no player ever sees a match end that way.
CAPPED_STOP_EVENT = "capped stop"
# The mark is stamped just after the last picture is presented; freezedetect starts the still at that picture.
CAPPED_STOP_SLACK_S = 0.2


def running_stills(spans, rows, minimum_s=1.0, capped_stop_ms=None, allowed=None, named=None):
    """The stills that fall while this screen's match runs: every saved frame over the span shows the game with the service Running.
    A still over a menu, a load or a stopped service is not the match freezing; the one still that holds the harness's capped stop
    is the run ending, recorded in `allowed` with its reason, and is not a freeze either."""
    saved = [row for row in rows if row.get("saved", True) and "wall_ms" in row]
    if not saved:
        return []
    origin, last = saved[0]["wall_ms"], saved[-1]["wall_ms"]
    capped_s = (capped_stop_ms - origin) / 1000.0 if capped_stop_ms is not None else None
    stills = []
    for start, end in spans:
        stop = end if end is not None else (last - origin) / 1000.0
        if stop - start < minimum_s:
            continue
        inside = [row for row in saved if origin + start * 1000 <= row["wall_ms"] <= origin + stop * 1000]
        if inside and all(row.get("screen") == "game" and row.get("service_state") == "Running" for row in inside):
            still = {"start_s": round(start, 3), "end_s": round(stop, 3), "frames": [inside[0].get("frame"), inside[-1].get("frame")]}
            if capped_s is not None and capped_s - CAPPED_STOP_SLACK_S <= start and capped_s <= stop:
                if allowed is not None:
                    allowed.append({**still, "reason": "the harness's capped stop at %.2f s" % capped_s})
                continue
            window = next((window for window in named or [] if window[0] <= origin + start * 1000 and origin + stop * 1000 <= window[1]), None)
            if window:
                if allowed is not None:
                    allowed.append({**still, "reason": window[2]})
                continue
            stills.append(still)
    return stills


def named_still_windows(scenario, run_name, peer, video_dir):
    """The scenario's still states for this peer as recorder-clock windows, each from its first mark to its closing mark."""
    path = Path(video_dir) / "events.jsonl" if video_dir else None
    if not path or not path.is_file():
        return []
    marks = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        message = event.get("message", "")
        if message.startswith("video_mark ") and not message.endswith(" PASS"):
            marks.setdefault(message.split(" ", 2)[1], event.get("wall_ms"))
    windows = []
    for spec in scenario.get("allowed_stills", []):
        if spec.get("peer") != peer or spec.get("run", run_name) != run_name:
            continue
        begin, end = marks.get(spec["from_mark"]), marks.get(spec["to_mark"])
        if begin is not None and end is not None and begin < end:
            windows.append((begin, end, spec["reason"]))
    return windows


def capped_stop_ms(video_dir):
    """The recorder's clock when the harness ended this round by its tick cap, or None."""
    path = Path(video_dir) / "events.jsonl" if video_dir else None
    if not path or not path.is_file():
        return None
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if event.get("message") == CAPPED_STOP_EVENT:
            return event.get("wall_ms")
    return None


# A still is confirmed on the picture: a frozen one changes no pixel by more than this many levels between its ends, while
# encoder noise stays far below it; this many such pixels is a picture that moved (a soldier walking on a 4K whole-map view).
MOTION_LEVELS = 40
MOTION_PIXELS = 1000


def changed_pixels(ffmpeg, video, start_s, end_s):
    """The pixels that changed by more than MOTION_LEVELS between the frames an encoded capture shows at two times."""
    graph = (f"[0:v]select='gte(t\\,{start_s:.3f})',format=gray,setpts=N[a];[1:v]select='gte(t\\,{end_s:.3f})',format=gray,setpts=N[b];"
             f"[a][b]blend=all_mode=difference,lutyuv=y='if(gt(val\\,{MOTION_LEVELS})\\,255\\,0)',signalstats,"
             "metadata=print:key=lavfi.signalstats.YAVG")
    result = subprocess.run([ffmpeg, "-hide_banner", "-nostdin", "-i", str(video), "-i", str(video), "-filter_complex", graph,
                             "-frames:v", "1", "-f", "null", "-"], capture_output=True, text=True, errors="replace", timeout=600)
    if result.returncode != 0: return None
    found = re.search(r"lavfi\.signalstats\.YAVG=([0-9.]+)", result.stderr)
    size = re.search(r"Stream #0:0.*?, (\d+)x(\d+)", result.stderr)
    if not found or not size:
        return None
    return round(float(found[1]) / 255 * int(size[1]) * int(size[2]))


def freeze_scan(ffmpeg, video, rows, video_dir=None, allowed=None, named=None):
    """ffmpeg's freezedetect over one peer's encoded capture: a still picture over one second while the match runs. Each
    span it calls still is confirmed on the picture; one whose ends differ in MOTION_PIXELS pixels moved and is kept as moved."""
    if not ffmpeg or not video or not Path(video).is_file():
        return None
    result = subprocess.run([ffmpeg, "-hide_banner", "-nostdin", "-i", str(video), "-vf", "freezedetect=n=-60dB:d=1", "-map", "0:v:0", "-f", "null", "-"],
                            capture_output=True, text=True, errors="replace", timeout=600)
    if result.returncode != 0:
        raise RuntimeError(f'ffmpeg still scan exited {result.returncode}: {result.stderr[-500:]}')
    if len(rows) < 2: raise ValueError(f'still scan has {len(rows)} indexed frames')
    stills = running_stills(parse_freezedetect(result.stderr), rows, capped_stop_ms=capped_stop_ms(video_dir), allowed=allowed, named=named)
    for still in stills:
        # The span's last picture shows a little before its end time; a span open to the end of the video stays a still.
        if still.get("end_s") is not None and still["end_s"] - still["start_s"] > 0.1:
            still["changed_pixels"] = changed_pixels(ffmpeg, video, still["start_s"], still["end_s"] - 0.05)
    return stills


def recording_health(video_dir, minimum_share=0.9, minimum_duration_s=None):
    """The recorder's own account of a capture, judged against what it was given: of the frames the engine presented at the
    capture rate (saved, failed to write, or turned away by a full queue), the share it saved, and the longest wait between two
    saved frames that holds a frame it turned away. The engine's own presented rate is reported beside it, never judged here:
    a box whose engines present fewer frames than the capture rate is a load fact of that box, and the scene's own bars judge it."""
    manifest_path = Path(video_dir) / "manifest.json"
    if not manifest_path.is_file():
        return None
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    rows = [row for row in read_index(video_dir) if "wall_ms" in row]
    saved = [row["wall_ms"] for row in rows if row.get("saved", True)]
    failed = [row["wall_ms"] for row in rows if not row.get("saved", True)]
    # The engine indexes each slot its full queue turned away; a gap holding such slots is the recorder's, not the screen's.
    dropped_path = Path(video_dir) / "dropped.jsonl"
    drops = sorted(row["wall_ms"] for row in read_rows(dropped_path) if "wall_ms" in row) if dropped_path.is_file() else None
    # The recording is the match's: what follows the harness's capped stop is the run ending, not a frame the recorder owed.
    capped = capped_stop_ms(video_dir)
    if capped is not None:
        saved = [wall for wall in saved if wall <= capped]
        failed = [wall for wall in failed if wall <= capped]
        drops = [wall for wall in drops if wall <= capped] if drops is not None else None
    fps = manifest.get("fps") or 0
    span_s = (saved[-1] - saved[0]) / 1000.0 if len(saved) > 1 else 0.0
    saved_fps = (len(saved) - 1) / span_s if span_s > 0 else 0.0
    longest_gap_ms = max((later - earlier for earlier, later in zip(saved, saved[1:])), default=0)
    turned_away = len(drops) if drops is not None else int(manifest.get("frames_dropped") or 0)
    presented = len(saved) + len(failed) + turned_away
    presented_fps = presented / span_s if span_s > 0 else 0.0
    saved_share = len(saved) / presented if presented else 0.0
    recorder_gap_ms = None
    if drops is not None:
        recorder_gap_ms = 0
        for earlier, later in zip(saved, saved[1:]):
            if later - earlier > recorder_gap_ms and bisect.bisect_right(drops, earlier) < bisect.bisect_left(drops, later):
                recorder_gap_ms = later - earlier
    # More than two slots in a row lost to the queue shows as a still of the recorder's own making.
    gap_bar_ms = 3 * 1000 // fps if fps > 0 else 0
    minimum_duration_s = max(manifest.get('minimum_duration_s', 0), minimum_duration_s or 0)
    complete = len(saved) >= 2 and span_s > 0 and span_s >= minimum_duration_s
    starved = not complete or fps <= 0 or presented == 0 or saved_share < minimum_share or (recorder_gap_ms is not None and recorder_gap_ms > gap_bar_ms)
    return {"fps": fps, "frames_saved": manifest.get("frames_saved"), "frames_dropped": manifest.get("frames_dropped"),
            "frames_rate_limited": manifest.get("frames_rate_limited"), "span_s": round(span_s, 3), "saved_fps": round(saved_fps, 2),
            "presented_at_capture_rate": presented, "saved_share": round(saved_share, 4), "engine_presented_fps": round(presented_fps, 2),
            "longest_gap_ms": longest_gap_ms, "recorder_gap_ms": recorder_gap_ms, "recorder_gap_bar_ms": gap_bar_ms, "starved": starved,
            "complete": complete, "minimum_duration_s": minimum_duration_s}


def read_index(video_dir):
    """The engine's per-frame index; a capture that never presented a frame leaves it empty."""
    return read_rows(Path(video_dir) / "frames.jsonl")


def read_rows(path):
    """One JSON object per line; a torn or missing file reads as what it holds."""
    rows = []
    if not path.is_file():
        return rows
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return rows


LOCKSTEP_ROUND_START = "[net-lockstep] start round="
ROUND_START_FRAME = re.compile(r"(?m)^\[net-lockstep\] start round=\d+ frame=(\d+) ")
RECLAIM_SAMPLE = re.compile(r"(?m)^\[fullstate-reclaim\] (tick=\d+ hash=[0-9a-f]{16} sections=\S+ round=\d+)\s*$")
ACTIVITY_OVER = re.compile(r"(?m)^\[net-match-service-e2e\] activity over at frame (\d+): no full-state sample follows$")


def with_reclaim_samples(log):
    """A peer's log plus its labelled reclaim-frame samples, written beside it as oracle lines the comparison reads."""
    log = Path(log)
    text = log.read_text(encoding="utf-8", errors="replace") if log.is_file() else ""
    samples = ["[fullstate] " + body for body in RECLAIM_SAMPLE.findall(text)]
    if not samples:
        return [log]
    derived = log.with_name(log.stem + ".reclaim-samples.log")
    derived.write_text("\n".join(samples) + "\n", encoding="utf-8")
    return [log, derived]


def fullstate_verdict(host_log, client_log):
    """The full-state oracle for a pair, or not applicable when neither peer started a lockstep round (a scenario that
    ends in the lobby samples nothing to compare, and that is not a divergence)."""
    started = [Path(log).is_file() and LOCKSTEP_ROUND_START in Path(log).read_text(encoding="utf-8", errors="replace")
               for log in (host_log, client_log)]
    if not any(started):
        return {"passed": None, "not_applicable": "neither peer started a lockstep round", "reasons": [], "compared_samples": 0}
    verdict = compare_fullstate(with_reclaim_samples(host_log), with_reclaim_samples(client_log))
    if verdict["reasons"] == ["the peers share no full-state sample"] and not verdict["client_samples"]:
        # A finished activity takes no capture: a peer whose rounds all began at or after the end has nothing to compare.
        host_text, client_text = (Path(log).read_text(encoding="utf-8", errors="replace") for log in (host_log, client_log))
        ended = [int(frame) for frame in ACTIVITY_OVER.findall(host_text)]
        starts = [int(frame) for frame in ROUND_START_FRAME.findall(client_text)]
        if ended and all(frame >= min(ended) for frame in starts):
            verdict.update(passed=None, reasons=[], not_applicable=f"the peer's round began at {starts or 'no frame'}, at or after the activity ended at frame "
                                                                   f"{min(ended)}: a finished activity takes no full-state capture")
    return verdict


def exempt_injected(verdict, pair, injected, items):
    """A divergence the scenario injects on purpose is the script at its declared tick, and only once every declared repair
    item passed; any other divergent tick stays a finding."""
    if verdict.get("passed") is not False or not verdict.get("divergences") or injected.get("peer") not in pair.split("/"):
        return verdict
    repairs = [item for item in items if item.get("id") in injected["repair_items"]]
    if {item["id"] for item in repairs} != set(injected["repair_items"]) or any(item.get("finding") for item in repairs):
        return verdict
    kept = [row for row in verdict["divergences"] if row["tick"] != injected["tick"]]
    if len(kept) == len(verdict["divergences"]):
        return verdict
    result = {**verdict, "divergences": kept, "divergent_ticks": len(kept), "first_divergence": None, "reasons": [],
              "exempted": [{**row, "declared": f"injected_desync peer={injected['peer']} tick={injected['tick']}",
                            "repair_items": injected["repair_items"]} for row in verdict["divergences"] if row not in kept]}
    if kept:
        first = kept[0]
        result["first_divergence"] = {"round": first["round"], "tick": first["tick"], "section": first["sections"][0], "sections": first["sections"]}
        result["reasons"].append(f"full state differs at round {first['round']} tick {first['tick']}: first section {first['sections'][0]} "
                                 f"of {len(first['sections'])} {first['sections']}")
    result["passed"] = not result["reasons"]
    return result


def staged_copy(source, replacements):
    """A fixture's bytes with each [pattern, replacement] applied to exactly one line, as the feel driver rewrites its tick target."""
    data = Path(source).read_bytes()
    for pattern, replacement in replacements:
        text, count = re.subn(pattern, replacement, data.decode("utf-8"), flags=re.M)
        if count != 1:
            raise ValueError(f"{source}: {pattern!r} matched {count} lines, expected exactly one")
        data = text.encode("utf-8")
    return data


def log_line_seen(path, pattern):
    """Whether a peer's own stdout has printed a line matching pattern yet (a gate on the product's own log)."""
    path = Path(path)
    return path.is_file() and re.search(pattern, path.read_text(encoding="utf-8", errors="replace"), re.M) is not None


RECORDER_FLUSH_S = 5.0


def last_event_ms(video_dir):
    """The recorder's clock at its latest event line; events are written at once, frames behind a queue."""
    path = Path(video_dir) / "events.jsonl"
    if not path.is_file():
        return None
    for line in reversed(path.read_text(encoding="utf-8", errors="replace").splitlines()):
        try:
            return json.loads(line)["wall_ms"]
        except (json.JSONDecodeError, KeyError, TypeError):
            continue
    return None


def await_recorder(video_dir, stop, timeout_s=RECORDER_FLUSH_S):
    """Waits until the recorder has written a frame taken at or after its latest event, bounded by timeout_s."""
    target = last_event_ms(video_dir)
    if target is None or not (Path(video_dir) / "frames.jsonl").is_file():
        return {"target_wall_ms": target, "flushed": False, "reason": "no recorder output"}
    deadline = time.monotonic() + timeout_s
    while True:
        rows = read_index(video_dir)
        latest = rows[-1]["wall_ms"] if rows else None
        if latest is not None and latest >= target:
            return {"target_wall_ms": target, "flushed": True, "last_frame_wall_ms": latest}
        if time.monotonic() >= deadline:
            return {"target_wall_ms": target, "flushed": False, "last_frame_wall_ms": latest, "reason": "timed out"}
        if stop.wait(.05):
            return {"target_wall_ms": target, "flushed": False, "last_frame_wall_ms": latest, "reason": "capture stopping"}


def read_manifest(video_dir):
    path = Path(video_dir) / "manifest.json"
    if not path.is_file():
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {}


_ENCODER_CODEC = {}


def encoder_codec(ffmpeg):
    """The codec the engines stream into: the GPU's h264 encoder when this box offers one, libx264 otherwise; probed once."""
    if ffmpeg not in _ENCODER_CODEC:
        probe = subprocess.run([ffmpeg, "-hide_banner", "-nostdin", "-loglevel", "error", "-f", "lavfi", "-i", "color=black:s=320x240:d=0.2",
                                "-c:v", "h264_nvenc", "-f", "null", "-"], capture_output=True, text=True) if ffmpeg else None
        _ENCODER_CODEC[ffmpeg] = "h264_nvenc" if probe is not None and probe.returncode == 0 else "libx264"
    return _ENCODER_CODEC[ffmpeg]


def probe_video(ffmpeg, destination):
    ffprobe = find_ffprobe(ffmpeg)
    if not ffprobe:
        return {}
    check = subprocess.run([ffprobe, "-v", "error", "-count_frames", "-select_streams", "v:0", "-show_entries",
                            "stream=width,height,nb_read_frames,r_frame_rate:format=duration", "-of", "json", str(destination)],
                           capture_output=True, text=True)
    return json.loads(check.stdout) if check.returncode == 0 else {}


def encode(ffmpeg, video_dir, fps, destination):
    """One peer's frames to h264. Even dimensions are forced because yuv420p needs them. A capture the engine streamed into
    its own encoder is taken as it is: one video frame per capture slot, the last picture held over a slot nothing filled."""
    streamed = Path(video_dir) / "capture.mp4"
    if streamed.is_file():
        rows = [row for row in read_index(video_dir) if row.get("saved", True) and "video_frame" in row]
        os.replace(streamed, destination)
        manifest = read_manifest(video_dir) or {}
        return {"encoded": Path(destination).is_file() and bool(rows), "timing": "engine-slots", "origin_wall_ms": rows[0]["wall_ms"] if rows else None,
                "fps": fps, "ffprobe": probe_video(ffmpeg, destination), "encoder": manifest.get("encoder"),
                "capture_duration_s": (rows[-1]["wall_ms"] - rows[0]["wall_ms"]) / 1000 + 1 / fps if rows else 0, "path": str(destination)}
    frames = Path(video_dir) / "frames"
    if not frames.is_dir() or not any(frames.glob("frame-*.png")):
        return {"encoded": False, "reason": f"no frames in {frames}"}
    if not ffmpeg:
        return {"encoded": False, "reason": "ffmpeg not found on PATH or at the known locations"}
    rows = [row for row in read_index(video_dir) if row.get("saved", True) and
            (frames / f"frame-{row['frame']:06d}.png").is_file()]
    if not rows:
        return {"encoded": False, "reason": "no indexed saved frames"}
    timeline = Path(video_dir) / "timeline.ffconcat"
    lines = ["ffconcat version 1.0"]
    for index, row in enumerate(rows):
        path = (frames / f"frame-{row['frame']:06d}.png").resolve().as_posix().replace("'", "'\\''")
        duration = (rows[index + 1]["wall_ms"] - row["wall_ms"]) / 1000 if index + 1 < len(rows) else 1 / fps
        lines += [f"file '{path}'", "option framerate 1000", f"duration {max(.001, duration):.6f}"]
    lines += [f"file '{path}'", "option framerate 1000"]
    timeline.write_text("\n".join(lines) + "\n", encoding="utf-8")
    command = [ffmpeg, "-y", "-nostdin", "-loglevel", "error", "-f", "concat", "-safe", "0",
               "-i", str(timeline), "-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2", "-r", str(fps), "-fps_mode", "cfr",
               "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "20", str(destination)]
    result = subprocess.run(command, capture_output=True, text=True)
    metadata = probe_video(ffmpeg, destination) if result.returncode == 0 else {}
    return {"encoded": result.returncode == 0 and Path(destination).is_file(), "timing": "wall-clock-cfr",
            "origin_wall_ms": rows[0]["wall_ms"], "fps": fps, "ffprobe": metadata,
            "capture_duration_s": (rows[-1]["wall_ms"] - rows[0]["wall_ms"]) / 1000 + 1 / fps,
            "command": command, "returncode": result.returncode,
            "stderr": result.stderr[-2000:], "path": str(destination)}


def extract_frames(ffmpeg, video, rows, frames):
    """The picked rows' pictures out of a streamed capture's MP4, named as the PNG path would have named them."""
    wanted = [row for row in rows if "video_frame" in row and not (frames / f"frame-{row['frame']:06d}.png").is_file()]
    if not wanted or not ffmpeg or not Path(video).is_file():
        return
    frames.mkdir(parents=True, exist_ok=True)
    select = "+".join(f"eq(n,{row['video_frame']})" for row in wanted)
    scratch = frames / "extract"
    scratch.mkdir(exist_ok=True)
    subprocess.run([ffmpeg, "-hide_banner", "-nostdin", "-loglevel", "error", "-y", "-i", str(video), "-vf", f"select='{select}'",
                    "-fps_mode", "passthrough", str(scratch / "pick-%06d.png")], capture_output=True, text=True)
    for index, row in enumerate(sorted(wanted, key=lambda row: row["video_frame"]), 1):
        picture = scratch / f"pick-{index:06d}.png"
        if picture.is_file():
            os.replace(picture, frames / f"frame-{row['frame']:06d}.png")
    for leftover in scratch.glob("*.png"):
        leftover.unlink()
    scratch.rmdir()


def contact_sheet(video_dir, rows, destination, every, ffmpeg, video=None):
    """Every `every`-th frame tiled SHEET_COLUMNS wide, each thumbnail labelled with what it is."""
    frames = Path(video_dir) / "frames"
    picked = rows[::every] if rows else []
    if not picked:
        return {"written": False, "reason": "no frames to tile"}
    if video:
        extract_frames(ffmpeg, video, picked, frames)
    try:
        from PIL import Image, ImageDraw  # noqa: PLC0415
    except ImportError:
        return contact_sheet_ffmpeg(frames, destination, every, ffmpeg)
    thumbs, labels = [], []
    for row in picked:
        path = frames / f"frame-{row['frame']:06d}.png"
        if not path.is_file():
            continue
        image = Image.open(path).convert("RGB")
        scale = SHEET_THUMB_WIDTH / image.width
        thumbs.append(image.resize((SHEET_THUMB_WIDTH, max(1, int(image.height * scale)))))
        labels.append(f"#{row['frame']} {(row['wall_ms'] - rows[0]['wall_ms']) / 1000:.2f}s t{row.get('sim_tick', '?')} {row.get('screen', '?')}")
    if not thumbs:
        return {"written": False, "reason": f"no readable PNGs under {frames}"}
    band = 14
    width, height = thumbs[0].width, thumbs[0].height + band
    columns = min(SHEET_COLUMNS, len(thumbs))
    lines = (len(thumbs) + columns - 1) // columns
    sheet = Image.new("RGB", (columns * width, lines * height), (16, 16, 16))
    draw = ImageDraw.Draw(sheet)
    for index, (thumb, label) in enumerate(zip(thumbs, labels)):
        x, y = (index % columns) * width, (index // columns) * height
        sheet.paste(thumb, (x, y))
        draw.text((x + 3, y + thumb.height + 2), label, fill=(235, 235, 235))
    sheet.save(destination)
    return {"written": True, "path": str(destination), "tiles": len(thumbs), "every": every, "by": "PIL"}


def contact_sheet_ffmpeg(frames, destination, every, ffmpeg):
    """The fallback sheet: no burned-in labels, so review.json carries the frame numbers instead."""
    if not ffmpeg:
        return {"written": False, "reason": "neither PIL nor ffmpeg is available for the contact sheet"}
    command = [ffmpeg, "-y", "-nostdin", "-loglevel", "error", "-start_number", "0",
               "-i", str(frames / "frame-%06d.png"),
               "-vf", f"select=not(mod(n\\,{every})),scale={SHEET_THUMB_WIDTH}:-1,tile={SHEET_COLUMNS}x8",
               "-frames:v", "1", str(destination)]
    result = subprocess.run(command, capture_output=True, text=True)
    return {"written": result.returncode == 0 and Path(destination).is_file(), "path": str(destination),
            "every": every, "by": "ffmpeg tile", "returncode": result.returncode, "stderr": result.stderr[-2000:]}


def frame_range(rows, item):
    """Where the capture puts a checklist item: the frames whose screen (or any of its screens) and sim tick match."""
    screen = item.get("screen")
    screens = [screen] if isinstance(screen, str) else screen
    low, high = (item.get("sim_ticks") or [None, None])[:2]
    hits = []
    for row in rows:
        if not row.get("saved", True):
            continue
        if screens and row.get("screen") not in screens:
            continue
        if item.get("service_state") and row.get("service_state") != item["service_state"]:
            continue
        tick = row.get("sim_tick")
        if low is not None and (tick is None or tick < low):
            continue
        if high is not None and (tick is None or tick > high):
            continue
        hits.append(row["frame"])
    if not hits:
        return None
    return [hits[0], hits[-1]]


def probe_verdict(probe_dir, item):
    """Whether the peer's menu probe completed successfully."""
    result = Path(probe_dir) / "net-ui-result.json"
    if not probe_dir or not result.is_file():
        return {"probe": "fail", "reason": "Required probe result is absent"}
    try:
        observed = json.loads(result.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {"probe": "fail", "reason": "Required probe result is unreadable"}
    return {"probe": "pass" if observed.get("pass") and observed.get("complete") else "fail",
            "complete": bool(observed.get("complete")), "path": str(result)}


# The screen checks every scenario carries (A13): the engine judges each drawn frame from its arming to the process's end and logs
# every distinct offence once, so one capture lists them all.
SCREEN_WATCH_RULES = {
    "layout": ("Every shown label's text fits its own rect, its rect sits inside its panel and the screen.", "layout always"),
    "duplicates": ("No two shown overlay controls carry the same line at once (the status strip and a toast never stack one event).", "duplicates always"),
    "held-reads-held": ("A seat kept for its player never reads 'Left' on another screen while it is held.", "forbid remote_held Left - AI in control"),
    "own-hold-line": ("The held player's own screen says it is held from the hold's first frame to the frame its control returns.", "require local_held Held - AI in control"),
    "rtt": ("NET STATUS's round-trip summary agrees with the per-player pings listed beside it.", "rtt always"),
    "seat-rows": ("Every seat the open seats panel lists has its row drawn inside the panel.", "seat_rows panel_open"),
}
SCREEN_WATCHES = "".join(f"h15-{name} {spec}\n" for name, (_, spec) in SCREEN_WATCH_RULES.items())
TEXT_WATCH_LINE = re.compile(r"^\[text-watch\] (armed|violation|summary) (.*)$", re.M)


def screen_watch_results(peer_root):
    """Each native armed watch and every summary; missing terminal coverage remains visible to review."""
    log = Path(peer_root) / "stdout.log"
    text = log.read_text(encoding="utf-8", errors="replace") if log.is_file() else ""
    results = {}
    for kind, body in TEXT_WATCH_LINE.findall(text):
        if kind == "armed":
            try:
                armed = json.loads(body)
            except json.JSONDecodeError:
                armed = {'armed': 'invalid-armed-json'}
            full = str(armed.get('armed', 'invalid-unnamed-watch'))
            name = full[4:] if full.startswith('h15-') else full
            row = results.setdefault(name, {'offences': [], 'summary': None, 'armed': armed, 'arms': 0, 'summaries': []})
            row['arms'] += 1
        elif kind == "violation":
            name, _, rest = body.partition(" ")
            name = name[4:] if name.startswith('h15-') else name
            if name in results:
                record = rest.partition(" ")[2]
                try:
                    results[name]["offences"].append(json.loads(record))
                except json.JSONDecodeError:
                    results[name]["offences"].append({"raw": record})
        else:
            try:
                summary = json.loads(body)
            except json.JSONDecodeError:
                continue
            name = str(summary.get("watch", ""))
            name = name[4:] if name.startswith('h15-') else name
            row = results.setdefault(name, {'offences': [], 'summary': None, 'armed': None, 'arms': 0, 'summaries': []})
            summary['_arm'] = row['arms']
            row['summary'] = summary
            row['summaries'].append(summary)
    return results or None


def scene_termination_tick(peer):
    record = peer.get('record') or {}
    if not peer.get('expected_termination') or not str(record.get('injected_termination', '')).startswith('scenario drop'):
        return None
    path = Path(peer.get('video_dir') or Path(peer['root']) / 'video') / 'injected-drop.json'
    try:
        receipt = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None
    tick = (receipt.get('last_recorded_frame') or {}).get('sim_tick')
    return tick if type(tick) is int and tick >= 0 else None


def expected_screen_watches(peer):
    names = set(SCREEN_WATCH_RULES)
    root = Path(peer.get('stage') or Path(peer['root']).with_name(Path(peer['root']).name + '-stage'))
    for leaf in ('menu.txt', 'screen-watches.txt', 'probe/probe.json'):
        path = root / leaf
        text = path.read_text(encoding='utf-8', errors='replace') if path.is_file() else ''
        names.update(re.findall(r'\bh15-([\w-]+)\b', text))
    return names


def capture_evidence_items(scenario, capture, peer):
    name = peer['peer']
    declared = next((run for run in scenario.get('runs', []) if run.get('name') == capture['name']), scenario)
    declared_peer = next((row for row in declared.get('peers', scenario.get('peers', [])) if row.get('name') == name), {})
    minimum = peer.get('minimum_capture_s', declared_peer.get('minimum_capture_s', declared.get('minimum_capture_s', scenario.get('minimum_capture_s'))))
    base = dict(run=capture['name'], peer=name, screen='any', frames=None, capture_frames=None, video_seconds=None,
                video=peer.get('video'), contact_sheet=peer.get('contact_sheet'), state='checked')
    def item(id, rule, error='', incomplete=False, **details):
        return dict(base, id=id, what=rule, **{'assert': rule}, probe='incomplete' if incomplete else 'fail' if error else 'pass',
                    **details, **(dict(finding=dict(class_='harness', reason=error, launch=None, errors=[])) if error else {}))
    output = []
    from feel.harness_cost import reduce_costs
    cost = reduce_costs([Path(peer['root']) / 'stdout.log'])
    output.append(item(f'harness-cost-{name}', cost['rule'], cost['reason'],
                       incomplete=cost['status'] == 'INCOMPLETE', instrumentation=cost))
    health, error = None, ''
    try:
        health = recording_health(peer['video_dir'], minimum_duration_s=minimum) if peer.get('video_dir') else None
    except (OSError, ValueError, TypeError) as failed:
        error = f'recording metadata: {type(failed).__name__}: {failed}'
    if health is None: error = error or 'recorder manifest is absent'
    elif not health['complete']: error = f'capture span={health["span_s"]} s, minimum={health["minimum_duration_s"]} s, saved frames={health["frames_saved"]}'
    elif health['starved']: error = f'saved share={health["saved_share"]}, recorder gap={health["recorder_gap_ms"]} ms, gap bound={health["recorder_gap_bar_ms"]} ms'
    output.append(item(f'recording-rate-{name}', 'Recorder evidence is complete, reaches its declared minimum and meets the existing saved-share/gap bars',
                       error, incomplete=health is None or not health.get('complete', False), recording=health))
    allowed, stills, error = [], None, ''
    try:
        stills = freeze_scan(find_ffmpeg(), peer.get('video'), read_index(peer['video_dir']) if peer.get('video_dir') else [],
                             peer.get('video_dir'), allowed, named_still_windows(scenario, capture['name'], name, peer.get('video_dir')))
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as failed:
        error = f'still scan: {type(failed).__name__}: {failed}'
    if stills is None: error = error or 'still scan is absent: ffmpeg or encoded video unavailable'
    moved = [still for still in stills or [] if (still.get('changed_pixels') or 0) >= MOTION_PIXELS]
    failures = [still for still in stills or [] if still not in moved]
    if failures: error = f'{len(failures)} running still(s), first at {failures[0]["start_s"]} s'
    output.append(item(f'recording-stills-{name}', f'Checked decoder and no running still over 1 s; pixel bars={MOTION_LEVELS}/{MOTION_PIXELS}',
                       error, incomplete=stills is None, stills=failures, moved_stills=moved, allowed_stills=allowed))
    watches = screen_watch_results(peer['root']) or {}
    killed = bool(peer.get('expected_termination'))
    kill_tick = scene_termination_tick(peer)
    for watch in sorted(expected_screen_watches(peer) | set(watches)):
        result = watches.get(watch) or dict(offences=[], summary=None, armed=None, arms=0, summaries=[])
        summaries = result.get('summaries', [])
        offences = [row for row in result['offences'] if not (killed and kill_tick is not None
                    and type(row.get('lockstep_frame')) is int and row['lockstep_frame'] > kill_tick)]
        selected = []
        for arm in range(1, result.get('arms', 0) + 1):
            own = [row for row in summaries if row.get('_arm') == arm]
            if killed:
                own = [row for row in own if type(row.get('through_tick')) is int and kill_tick is not None
                       and 0 <= row['through_tick'] <= kill_tick]
                if own: selected.append(max(own, key=lambda row: row['through_tick']))
            else:
                final = [row for row in own if row.get('flush') != 'periodic']
                if len(final) == 1: selected.append(final[0])
        complete = bool(result.get('arms')) and len(selected) == result['arms']
        if not complete: offences.append(dict(detail=f'watch {watch}: arms={result.get("arms", 0)}, summaries={len(summaries)}'))
        for summary in selected:
            if type(summary.get('frames')) is not int or summary['frames'] <= 0 or type(summary.get('violations')) is not int:
                complete = False; offences.append(dict(detail=f'watch {watch}: invalid summary {summary}'))
            elif summary['violations'] > 0: offences.append(dict(detail=f'watch {watch}: summary violations={summary["violations"]}'))
            if watch.startswith('scene-') and summary.get('active_frames', 0) == 0:
                offences.append(dict(detail='its state never held while it was armed'))
        rule = SCREEN_WATCH_RULES.get(watch, (f'Every armed {watch} screen check has a terminal summary', ''))[0]
        if killed:
            rule += f' terminated by the scene at tick {kill_tick}' if kill_tick is not None else ' scene termination tick is unproved'
        output.append(item(f'screen-{watch}-{name}', rule, json.dumps(offences[0]) if offences else '', incomplete=not complete,
                           watch=dict(summary=selected[-1] if selected else None, summaries=summaries,
                                      selected_summaries=selected, termination_tick=kill_tick, offences=offences[:40])))
    for row in output:
        if 'finding' in row: row['finding']['class'] = row['finding'].pop('class_')
        if capture.get('interrupted'):
            row.update(probe='incomplete', finding={'class': 'harness', 'reason': capture['interrupted'], 'launch': None, 'errors': []})
    return output


def menu_script_failures(peer_root):
    text = ""
    for leaf in ("stdout.log", "stderr.log"):
        path = Path(peer_root) / leaf
        if path.is_file():
            text += path.read_text(encoding="utf-8", errors="replace")
    return [line for line in text.splitlines() if any(marker in line for marker in
            ("[menu-script] FAILED", "[record-video] refused", "[net-ui-probe] FAIL"))]


def log_assertions(peer_root, required=(), forbidden=()):
    lines = []
    console = "console.log" if (Path(peer_root) / "console.log").is_file() else "runtime/LogConsole.txt"
    for leaf in ("stdout.log", "stderr.log", console):
        path = Path(peer_root) / leaf
        if path.is_file():
            lines += [{"path": str(path), "line": index + 1, "text": line}
                      for index, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines())]
    return [{"assertion": pattern, "forbidden": denied,
             "matches": [line for line in lines if re.search(pattern, line["text"])]}
            for denied, patterns in ((False, required), (True, forbidden)) for pattern in patterns]


LISTED_ROW = re.compile(r' row=("(?:[^"\\]|\\.)*")')
ROW_ENDPOINT = re.compile(r"\s(\S+):(\d+)( \[[^\]]+\])?$")


def listed_rows(peer_root, control):
    """The rows a menu list showed, in list order, from the script's last text-fit readback of that list."""
    path = Path(peer_root) / "stdout.log"
    text = path.read_text(encoding="utf-8", errors="replace") if path.is_file() else ""
    lines = [line for line in text.splitlines() if line.startswith(f"[menu-script] assert_text_fits {control} ")]
    return [json.loads(row) for row in LISTED_ROW.findall(lines[-1])] if lines else None


def own_session_evidence(peer_root, spec, port):
    """A LAN listing shows every engine beaconing on the network, so the count is of this run's own session only."""
    rows = listed_rows(peer_root, spec["control"])
    endpoints = [(row, ROW_ENDPOINT.search(row)) for row in rows or []]
    own = [row for row, found in endpoints if found and int(found[2]) == port]
    others = [row for row, found in endpoints if not (found and int(found[2]) == port)]
    address = spec.get("address")
    placed = all(ROW_ENDPOINT.search(row)[1] == address for row in own) if address else True
    return {"control": spec["control"], "port": port, "rows": rows, "own_rows": own, "other_sessions": others,
            "expected": spec.get("expected", 1), "address": address,
            "pass": rows is not None and len(own) == spec.get("expected", 1) and placed}


def join_port_evidence(peer_root, spec):
    """The Port field follows the first joinable listed row, whichever session that is."""
    rows = listed_rows(peer_root, spec["control"])
    joinable = [found for found in (ROW_ENDPOINT.search(row) for row in rows or []) if found and not found[3]]
    path = Path(peer_root) / "stdout.log"
    text = path.read_text(encoding="utf-8", errors="replace") if path.is_file() else ""
    shown = re.findall(rf'^\[menu-script\] assert_label {re.escape(spec["field"])} ".*?" text="([^"]*)" PASS$', text, re.M)
    expected = joinable[0][2] if joinable else None
    return {"control": spec["control"], "field": spec["field"], "rows": rows, "first_joinable_port": expected,
            "shown": shown[-1] if shown else None, "pass": bool(expected) and bool(shown) and shown[-1] == expected}


def frame_gap_evidence(record, spec):
    """The recorder's own wall clock between presented frames. A menu that holds one is a render stall."""
    rows = [row for row in record.get("index", []) if isinstance(row.get("wall_ms"), (int, float))]
    limit = int(spec.get("max_ms", 1000))
    wanted = spec.get("screens")
    ignored = set(spec.get("ignore_screens", ["Loading", "LoadingScreen", "game"]))
    worst, over = None, []
    for earlier, later in zip(rows, rows[1:]):
        screens = (earlier.get("screen"), later.get("screen"))
        if any(screen in ignored for screen in screens):
            continue
        if wanted and any(screen not in wanted for screen in screens):
            continue
        gap = {"frames": [earlier.get("frame"), later.get("frame")], "screen": later.get("screen"),
               "gap_ms": later["wall_ms"] - earlier["wall_ms"], "wall_ms": earlier["wall_ms"]}
        if worst is None or gap["gap_ms"] > worst["gap_ms"]:
            worst = gap
        if gap["gap_ms"] > limit:
            over.append(gap)
    return {"max_ms": limit, "screens": wanted, "measured_frames": len(rows), "worst": worst,
            "over": over[:10], "over_count": len(over), "pass": worst is not None and not over}


def drop_evidence(record, tick):
    if not record:
        return {"pass": False, "reason": "The dropped peer was not launched"}
    path = Path(record["video_dir"]) / "injected-drop.json"
    receipt = json.loads(path.read_text(encoding="utf-8")) if path.is_file() else {}
    passed = receipt.get("requested_tick") == tick and receipt.get("last_recorded_frame", {}).get("sim_tick", -1) >= tick
    passed = passed and str(record.get("record", {}).get("injected_termination", "")).startswith("scenario drop")
    return {"path": str(path), "receipt": receipt, "pass": passed}


def completed_probe(path):
    try:
        result = json.loads(Path(path).read_text(encoding="utf-8"))
        return isinstance(result, dict) and result.get("complete") is True and result.get("pass") is True
    except (OSError, ValueError):
        return False


def window_progress(observed, required):
    """The ticks the engine committed across a marked window, by its own counters at the window's two ends: the
    round's committed frame when a lockstep round runs through both, else every sim update. The window's own length
    on the engine's clock is reported beside it; the window itself is bounded by the probe's step, not by the wall."""
    if len(observed) < 2:
        return {"observed": 0, "required": required, "counted": None, "window_ms": None, "pass": False}
    first, last = observed[0], observed[-1]
    lockstep = all(row.get("lockstep_frame", 0) > 0 for row in (first, last))
    counter = "lockstep_frame" if lockstep else "sim_frame"
    progress = last[counter] - first[counter]
    window_ms = last["at_ms"] - first["at_ms"] if all("at_ms" in row for row in (first, last)) else None
    return {"observed": progress, "required": required, "counted": counter, "from": first[counter], "to": last[counter],
            "window_ms": window_ms, "ticks_per_s": round(progress * 1000 / window_ms, 2) if window_ms else None,
            "pass": progress >= required}


ROUND_START_FRAME = re.compile(r"^\[net-lockstep\] start round=\d+ frame=(\d+)")


def resumed_play_evidence(record, spec):
    """Play after an automatic repair, read from the engine's own records only: its relaunch line, the frame the
    relaunched round starts on, every tick it committed from there through the required tick (the -tick-hashes
    trace), actors the sim moved across them and the actors alive at the end (the match report)."""
    root = Path(record["root"])
    path = root / "stdout.log"
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines() if path.is_file() else []
    relaunch = next((index for index, line in enumerate(lines) if re.search(spec["relaunch"], line)), None)
    starts = [int(found[1]) for found in (ROUND_START_FRAME.match(line) for line in lines[:relaunch or 0]) if found]
    resume = starts[-1] if starts else None
    evidence = {"log": str(path), "relaunch_line": relaunch + 1 if relaunch is not None else None, "resume_frame": resume,
                "through_tick": spec["through_tick"]}
    trace = root.parent / spec["trace"].format(peer=record["peer"])
    report = root.parent / spec["report"].format(peer=record["peer"])
    try:
        rows = json.loads(trace.read_text(encoding="utf-8"))["runs"][0]["tick_hashes"]
    except (OSError, ValueError, KeyError, IndexError):
        rows = []
    try:
        match = json.loads(report.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        match = {}
    # The repaired round re-commits its resume frame, so its records start at the last one naming that frame.
    begin = max((index for index, row in enumerate(rows) if row.get("tick") == resume), default=None)
    after = rows[begin:] if begin is not None else []
    ticks = [row.get("tick") for row in after]
    committed = set(ticks)
    wanted = range(resume, spec["through_tick"] + 1) if resume is not None else range(0)
    gaps = [tick for tick in wanted if tick not in committed]
    actor_states = {row["subsystems"].get("actors") for row in after if row.get("tick", 0) <= spec["through_tick"] and row.get("subsystems")}
    evidence.update(trace=str(trace), report=str(report), first_committed_after_repair=ticks[0] if ticks else None,
                    last_committed=ticks[-1] if ticks else None, missing_ticks=gaps[:10], missing_count=len(gaps),
                    actor_states=len(actor_states), live_actors=match.get("actors"), resyncs=match.get("resyncs"))
    reasons = [text for failed, text in (
        (relaunch is None, "no relaunch line"),
        (resume is None, "no relaunched round start before the relaunch line"),
        (not ticks or ticks[0] != resume, "the trace does not resume at the relaunched round's frame"),
        (bool(gaps), f"{len(gaps)} tick(s) from the resume frame through {spec['through_tick']} were never committed"),
        (len(actor_states) < 2, "no actor changed across the resumed ticks"),
        (not match.get("actors"), "no live actor at the end of the match"),
        (not match.get("resyncs"), "the match report records no resync")) if failed]
    evidence.update({"pass": not reasons, "reason": "; ".join(reasons) or None})
    return evidence


def report_toast_evidence(record, spec):
    """The toasts a peer showed, read from its match report: each required toast by kind, text and tick."""
    report = Path(record["root"]).parent / spec.get("report", "{peer}-match.json").format(peer=record["peer"])
    try:
        toasts = json.loads(report.read_text(encoding="utf-8")).get("ui", {}).get("toasts", [])
    except (OSError, ValueError, AttributeError):
        toasts = []
    checks = []
    for required in spec["require"]:
        low, high = (required.get("ticks") or [None, None])[:2]
        found = [toast for toast in toasts if toast.get("kind") == required["kind"] and re.search(required["text"], toast.get("text", ""))
                 and (low is None or toast.get("tick", -1) >= low) and (high is None or toast.get("tick", -1) <= high)]
        checks.append({**required, "observed": found[:3], "pass": bool(found)})
    missing = [check["kind"] + " " + check["text"] for check in checks if not check["pass"]]
    return {"report": str(report), "toasts": len(toasts), "checks": checks, "pass": not missing,
            "reason": "no toast " + "; ".join(missing) + " in " + str(report) if missing else None}


# What decides an item without eyes: a log line, a numeric gate, a probe step or an engine record. Such an item is a LOG item,
# judged by its probe alone; every other item names what the screen must show and is a PICTURE item for the reviewer.
LOG_EVIDENCE = ("gate", "log_regex", "forbidden_log_regex", "events", "readback", "probe_steps", "sim_progress", "peer_drop", "drop_tick", "ownership_reclaim")


def dropped_index_evidence(record, drop_tick):
    """A killed peer's own recording: its index reaches the tick it was killed after, and no line but the last is torn."""
    path = Path(record["video_dir"]) / "frames.jsonl" if record.get("video_dir") else None
    lines = [line for line in path.read_text(encoding="utf-8", errors="replace").splitlines() if line.strip()] if path and path.is_file() else []
    torn = []
    for number, line in enumerate(lines):
        try:
            json.loads(line)
        except json.JSONDecodeError:
            torn.append(number)
    ticks = [row.get("sim_tick", 0) for row in record.get("index", []) if row.get("saved", True)]
    reached = max(ticks, default=0)
    passed = bool(ticks) and reached >= drop_tick and all(number == len(lines) - 1 for number in torn)
    return {"pass": passed, "rows": len(lines), "torn_lines": torn, "last_sim_tick": reached, "drop_tick": drop_tick,
            "reason": None if passed else f"the killed peer's index ends at tick {reached} (killed after {drop_tick}) with torn lines {torn}"}


def item_kind(item):
    """An item's own 'kind' when it names one, else what its evidence makes it."""
    if item.get("kind") in ("log", "picture"):
        return item["kind"]
    # Evidence that names nothing (an empty step list) proves nothing.
    return "log" if any(item.get(key) for key in LOG_EVIDENCE) else "picture"


def item_evidence(record, item, port=None):
    rows = record["index"]
    events_path = Path(record["video_dir"]) / "events.jsonl"
    events = [json.loads(line) for line in events_path.read_text(encoding="utf-8").splitlines()] if events_path.is_file() else []
    evidence = {}
    if item.get("mark"):
        marker = "video_mark " + item["mark"]
        starts = [index for index, event in enumerate(events) if event["message"] == marker]
        if not starts:
            return None, {"probe": "not-reached", "reason": f"No recorded marker {item['mark']}", "events": str(events_path)}
        index = starts[0]
        start = events[index]["wall_ms"]
        following = next((position for position in range(index + 1, len(events))
                          if events[position]["message"].startswith("video_mark ") and not events[position]["message"].endswith(" PASS")),
                         len(events))
        end = events[following]["wall_ms"] if following < len(events) else float("inf")
        rows = [row for row in rows if start <= row["wall_ms"] < end]
        # The marked events go by log order: an assert logged in the same millisecond as the next mark is still this one's.
        events = events[index:following]
        evidence.update(events=str(events_path), marker=item["mark"])
    required = item.get("events", [])
    probe_path = Path(record.get("probe_dir", "")) / "net-ui-result.json"
    probe_steps = item.get("probe_steps")
    if item.get("mark") and probe_steps is None and probe_path.is_file():
        script = json.loads(probe_path.read_text(encoding="utf-8")).get("script", {}).get("steps", [])
        start = next((index for index, step in enumerate(script) if step.get("command") == "video_mark " + item["mark"]), None)
        if start is not None:
            end = next((index for index in range(start + 1, len(script)) if script[index].get("command", "").startswith("video_mark ")), len(script))
            probe_steps = list(range(start, end))
    if required:
        matched = [{"assertion": pattern, "observed": [event for event in events if re.search(pattern, event["message"])]}
                   for pattern in required]
        evidence.update(probe="pass" if all(row["observed"] for row in matched) else "fail", assertions=matched)
    elif item.get("gate"):
        gate = record.get("gates", {}).get(item["gate"])
        evidence.update(probe="pass" if gate and gate.get("status") == "PASS" else "fail", assertions=[gate],
                        reason=None if gate else f"Missing feel gate {item['gate']}")
    elif probe_steps is not None:
        path = Path(record["probe_dir"]) / "net-ui-result.json"
        observed = json.loads(path.read_text(encoding="utf-8")) if path.is_file() else {}
        steps = {step["index"]: step for step in observed.get("steps", [])}
        required = probe_steps
        evidence.update(probe=("pass" if all(index in steps for index in required) else "not-reached") if required else "awaiting-review",
                        assertions=[{"step": index, "command": observed.get("script", {}).get("steps", [])[index]
                                     if index < len(observed.get("script", {}).get("steps", [])) else None,
                                     "observed": steps.get(index)} for index in required], path=str(path))
        if item.get("sim_progress") is not None:
            evidence["simulation_progress"] = window_progress([steps[index]["observed"] for index in required if index in steps],
                                                              item["sim_progress"])
            if not evidence["simulation_progress"]["pass"]:
                evidence["probe"] = "fail"
    else:
        declared_probe = Path(record.get('probe_dir', '')) / 'probe.json'
        evidence.update(probe_verdict(record.get("probe_dir", ""), item) if declared_probe.is_file() or probe_path.is_file()
                        else {'probe': 'awaiting-review'})
        if evidence['probe'] == 'pass':
            evidence['probe'] = 'awaiting-review'
    if item.get("resumed_play"):
        # The engine's own records decide this item; a probe step that a script may never reach does not.
        evidence["resumed_play"] = resumed_play_evidence(record, item["resumed_play"])
        evidence["probe"] = "pass" if evidence["resumed_play"]["pass"] else "fail"
        if not evidence["resumed_play"]["pass"]:
            evidence["reason"] = evidence["resumed_play"]["reason"]
    if item.get("drop_tick") is not None:
        evidence["dropped_index"] = dropped_index_evidence(record, int(item["drop_tick"]))
        evidence["probe"] = "pass" if evidence["dropped_index"]["pass"] else "fail"
        if not evidence["dropped_index"]["pass"]:
            evidence["reason"] = evidence["dropped_index"]["reason"]
    if item.get("log_regex") or item.get("forbidden_log_regex"):
        assertions = log_assertions(record["root"], item.get("log_regex", []), item.get("forbidden_log_regex", []))
        passed = all(bool(value["matches"]) != value["forbidden"] for value in assertions)
        evidence["log_assertions"] = assertions
        if not passed or evidence.get("probe") in ("none", "awaiting-review"):
            evidence["probe"] = "pass" if passed else "fail"
    if item.get("report_toasts"):
        evidence["report_toasts"] = report_toast_evidence(record, item["report_toasts"])
        passed = evidence["report_toasts"]["pass"]
        if not passed or evidence.get("probe") in ("none", "awaiting-review"):
            evidence["probe"] = "pass" if passed else "fail"
        if not passed:
            evidence["reason"] = evidence["report_toasts"]["reason"]
    for key, measure in (("own_session_rows", lambda spec: own_session_evidence(record["root"], spec, port)),
                         ("join_port_follows_list", lambda spec: join_port_evidence(record["root"], spec))):
        if item.get(key):
            evidence[key] = measure(item[key])
            passed = evidence[key]["pass"]
            if not passed or evidence.get("probe") in ("none", "awaiting-review"):
                evidence["probe"] = "pass" if passed else "fail"
    if item.get("frame_gap"):
        evidence["frame_gap"] = frame_gap_evidence(record, item["frame_gap"])
        passed = evidence["frame_gap"]["pass"]
        if not passed or evidence.get("probe") in ("none", "awaiting-review"):
            evidence["probe"] = "pass" if passed else "fail"
        if not passed:
            evidence["reason"] = ("No presented frames to measure" if evidence["frame_gap"]["worst"] is None else
                                  f"{evidence['frame_gap']['over_count']} render stall(s) over "
                                  f"{evidence['frame_gap']['max_ms']} ms; worst {evidence['frame_gap']['worst']['gap_ms']} ms")
    if item.get("drop_tick") is not None:
        evidence["process_drop"] = drop_evidence(record, item["drop_tick"])
        passed = evidence["process_drop"]["pass"]
        if not passed or evidence.get("probe") in ("none", "awaiting-review"):
            evidence["probe"] = "pass" if passed else "fail"
    if 'ownership_reclaim' in item:
        from e2e.ownership import reclaim_evidence
        owned = reclaim_evidence(record['root'], item['ownership_reclaim'])
        evidence['ownership_reclaim'] = owned
        if not owned['passed'] or evidence.get('probe') in ('none', 'awaiting-review'):
            evidence['probe'] = 'pass' if owned['passed'] else 'fail'
        if not owned['passed']: evidence['reason'] = '; '.join(owned['errors'])
    if item.get("readback"):
        observed = json.loads(probe_path.read_text(encoding="utf-8")) if probe_path.is_file() else {}
        steps = {step["index"]: step.get("observed", {}) for step in observed.get("steps", [])}
        checks = []
        for check in item["readback"]:
            value = steps.get(check["step"])
            for key in check["path"]:
                value = value.get(key) if isinstance(value, dict) else None
            if "not_contains" in check:
                passed = isinstance(value, str) and check["not_contains"] not in value
            else:
                passed = check["contains"] in value if "contains" in check and isinstance(value, str) else value == check.get("equals") and "equals" in check
            checks.append({**check, "actual": value, "pass": passed})
        evidence["readback_assertions"] = checks
        if not all(check["pass"] for check in checks):
            evidence["probe"] = "fail"
    if item.get("match_identity"):
        identity = record.get("match_identity") or {}
        path = Path(record.get("probe_dir", "")) / "match-identity.json"
        if path.is_file():
            identity = json.loads(path.read_text(encoding="utf-8"))
        passed = bool(identity.get("session_id") and identity.get("round") and identity.get("config_hash"))
        evidence["match_identity"] = identity
        if not passed or evidence.get("probe") in ("none", "awaiting-review"):
            evidence["probe"] = "pass" if passed else "fail"
    return frame_range(rows, item), evidence


def review(scenario, capture, out):
    """The reviewing agent's first read: every checklist item, where to look, and what the probe said."""
    items = []
    for item in scenario.get("checklist", []):
        scope = item.get("run")
        if scope and capture["name"] not in ([scope] if isinstance(scope, str) else scope):
            continue
        peer = item.get("peer")
        declared = next((run for run in scenario.get("runs", []) if run.get("name") == capture["name"]), scenario)
        peers = [peer] if peer else [row["peer"] for row in capture["peers"]] or [row["name"] for row in declared.get("peers", scenario.get("peers", []))]
        for name in peers:
            record = next((row for row in capture["peers"] if row["peer"] == name), None)
            if record is None:
                items.append({**item, "peer": name, "run": capture["name"], "frames": None, "probe": "not-run", "state": "skipped",
                              "finding": capture.get("skip_finding") or {"class": "harness", "reason": "No such peer in this capture"}})
                continue
            # A check that names the session protocol reads it from the tree the capture launched.
            if capture.get("repo") and "{NET_PROTOCOL_VERSION}" in json.dumps(item):
                item = substitute(item, source_tokens(capture["repo"]))
            found, assertions = item_evidence(record, item, capture.get("port"))
            if item.get("peer_drop"):
                required = item["peer_drop"]
                witness = next((row for row in capture["peers"] if row["peer"] == required["peer"]), None)
                assertions["peer_drop"] = {"peer": required["peer"], **drop_evidence(witness, required["tick"])}
                if not assertions["peer_drop"]["pass"]:
                    assertions.update(probe="fail", reason="The other peer has no matching recorded process drop")
            video_frames = found if record.get("video") else None
            encode_result = record.get("encode", {})
            seconds = None
            if video_frames and encode_result.get("timing") == "wall-clock-cfr":
                indexed = {row["frame"]: row for row in record["index"]}
                seconds = [(indexed[frame]["wall_ms"] - encode_result["origin_wall_ms"]) / 1000 for frame in found]
                video_frames = [round(second * encode_result["fps"]) for second in seconds]
            elif video_frames and encode_result.get("timing") == "engine-slots":
                # A streamed capture names each frame's place in its own video: one frame per capture slot.
                indexed = {row["frame"]: row for row in record["index"]}
                video_frames = [indexed[frame]["video_frame"] for frame in found if "video_frame" in indexed.get(frame, {})]
                seconds = [frame / encode_result["fps"] for frame in video_frames]
            resolved = {**item, "kind": item_kind(item), "peer": name, "run": capture["name"], "frames": video_frames,
                          "capture_frames": found, "video_seconds": seconds,
                          "video": record.get("video"), "contact_sheet": record.get("contact_sheet"),
                          "state": "captured" if video_frames else "no MP4 evidence",
                          **assertions}
            if assertions.get('probe') == 'awaiting-review' and video_frames and resolved["kind"] == "picture":
                resolved['state'] = 'AWAITING REVIEW'
            if not video_frames or item.get("blocked_by") or assertions.get("probe") in (None, "none", "fail", "not-reached", "not-run"):
                resolved["finding"] = {"class": (capture.get("stop_finding") or {}).get("class", "harness" if capture.get("interrupted") else "unclassified"), "reason": (capture.get("stop_finding") or {}).get("reason") or capture.get("interrupted") or item.get("blocked_by") or assertions.get("reason") or
                                       "Required frames or assertions absent; inspect the retained launch, probe and logs",
                                       "launch": record.get("launch"), "errors": record.get("menu_script_failures", [])}
            items.append(resolved)
    # Every capture answers the same question the player would have been asked: did a dialog fire? A headless
    # run continues past an assert the way a player's Ignore does, so the line it logged is the evidence.
    dialogs = []
    for peer in capture["peers"]:
        log = Path(peer["root"]) / "stdout.log"
        text = log.read_text(encoding="utf-8", errors="replace") if log.is_file() else ""
        dialogs += [{"peer": peer["peer"], "line": line.strip(), "log": str(log)}
                    for line in text.splitlines() if "RTE Assert (headless" in line]
    items.append({"id": "no-assert-dialogs", "run": capture["name"], "peer": "all", "screen": "any",
                  "what": "No peer had to answer an assert dialog: a player would have seen one for each line below.",
                  "assert": "No 'RTE Assert (headless' line in any peer's stdout.",
                  "frames": None, "capture_frames": None, "video_seconds": None, "video": None,
                  "contact_sheet": None, "state": "checked", "probe": "fail" if dialogs else "pass",
                  "assert_dialogs": dialogs,
                  **({"finding": {"class": "engine", "reason": "assert dialog: " + dialogs[0]["line"][:200],
                                  "launch": None, "errors": [row["line"] for row in dialogs[:3]]}} if dialogs else
                     {"finding": {"class": "harness", "reason": capture["interrupted"], "launch": None, "errors": []}}
                     if capture.get("interrupted") else {})})
    # Enumerate the mandatory evidence before reading its results. Missing output remains a required row.
    expected_evidence = []
    for peer in capture['peers']:
        expected_evidence += [f'recording-rate-{peer["peer"]}', f'recording-stills-{peer["peer"]}',
                              *[f'screen-{name}-{peer["peer"]}' for name in sorted(expected_screen_watches(peer))]]
        items.extend(capture_evidence_items(scenario, capture, peer))
    run_findings = []
    for peer in capture["peers"]:
        record = peer.get("record", {})
        planned = peer.get("expected_termination") and str(record.get("injected_termination", "")).startswith("scenario drop")
        if peer.get("error") or record.get("timed_out") or record.get("exit_code") not in (0, None) and not planned:
            run_findings.append({"class": (capture.get("stop_finding") or {}).get("class", "unclassified"), "run": capture["name"], "peer": peer["peer"],
                                 "reason": (capture.get("stop_finding") or {}).get("reason") or peer.get("error") or f"Unexpected runner result: exit={record.get('exit_code')} timed_out={record.get('timed_out')}",
                                 "launch": peer.get("launch")})
    declared_run = next((run for run in scenario.get("runs", []) if run.get("name") == capture["name"]), {})
    if declared_run.get("injected_desync") and capture.get("fullstate"):
        capture["fullstate"] = {pair: exempt_injected(verdict, pair, declared_run["injected_desync"], items)
                                for pair, verdict in capture["fullstate"].items()}
    retained = capture.get("retained")
    if retained and retained["bytes"] >= retained["limit"]:
        run_findings.append({"class": "harness", "run": capture["name"], "peer": "all", "launch": None,
                             "reason": f"retained set {retained['bytes']} bytes reaches {retained['limit']} after the run's transients were retired"})
    for pair, verdict in (capture.get("fullstate") or {}).items():
        if not verdict.get("not_applicable") and not verdict["passed"]:
            run_findings.append({"class": "engine", "run": capture["name"], "peer": pair,
                                  "reason": "full-state oracle: " + "; ".join(verdict["reasons"]), "launch": None})
    injected = declared_run.get('injected_desync') or {}
    repairs = injected.get('repair_items', [])
    repaired = bool(repairs) and all(any(item.get('id') == name and item.get('probe') == 'pass' and not item.get('finding')
                                       for item in items) for name in repairs)
    injector = next((peer for peer in capture['peers'] if peer['peer'] == injected.get('peer')), None)
    injection_log = Path(injector['root']) / 'stdout.log' if injector else None
    injected_here = bool(injection_log and injection_log.is_file() and re.search(
        rf'(?m)^\[net-test\] live perturb frame={injected.get("tick")}\b',
        injection_log.read_text(encoding='utf-8', errors='replace')))
    for peer in capture['peers']:
        log = Path(peer['root']) / 'stdout.log'
        text = log.read_text(encoding='utf-8', errors='replace') if log.is_file() else ''
        for match in re.finditer(r'(?m)^\[lockstep\] desync at frame (\d+)[^\n]*', text):
            if repaired and injected_here and int(match[1]) == injected.get('tick'):
                continue
            run_findings.append(dict(**{'class': 'engine'}, run=capture['name'], peer=peer['peer'],
                                     reason='Unplanned desync: ' + match[0], launch=peer.get('launch')))
    document = {"schema": 1, "scenario": scenario["name"], "title": scenario.get("title", ""),
                "requires": scenario.get("requires", []),
                "reviewer_reads": ["review.json", "<peer>-sheet.png", "<peer>.mp4"], "expected_evidence": expected_evidence,
                "peers": [{k: v for k, v in row.items() if k != "index"} for row in capture["peers"]],
                "checklist": items,
                "run_findings": run_findings,
                "failures": {row["peer"]: row["menu_script_failures"] for row in capture["peers"]},
                "verdict": "agent-review-required"}
    if capture.get("feel_window"):
        document["feel_window"] = capture["feel_window"]
    for key in ("footprint_peak_bytes", "retained", "retired"):
        if capture.get(key) is not None:
            document[key] = capture[key]
    (Path(out) / "review.json").write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    return document


def stage_peer(scenario, peer, root, tokens):
    """Writes this peer's probe, input script and menu script beside its run, and returns its env.

    Every peer's staging paths are already in `tokens` before a byte is written, so one peer's script
    can name another's done file the way the readback driver's paired cases do.
    """
    environment = {"CCCP_HEADLESS": "1"}
    if peer.get("probe"):
        directory = Path(root) / "probe"
        directory.mkdir(parents=True, exist_ok=True)
        path = directory / "probe.json"
        probe = substitute(json.loads(scenario_text(scenario, peer["probe"])), tokens)
        path.write_text(json.dumps(probe, indent=2) + "\n", encoding="utf-8")
        environment["CC_TEST_NET_UI_SCRIPT"] = str(path)
    if peer.get("input_script"):
        path = Path(root) / "input.txt"
        path.write_text(substitute(scenario_text(scenario, peer["input_script"]), tokens), encoding="utf-8")
    if peer.get("menu_script"):
        path = Path(root) / "menu.txt"
        path.write_text(substitute(scenario_text(scenario, peer["menu_script"]), tokens), encoding="utf-8")
    # Every peer's engine arms the shared screen watches from this file on its first drawn frame.
    watches = Path(root) / "screen-watches.txt"
    watches.write_text(SCREEN_WATCHES, encoding="utf-8")
    environment["CCCP_TEST_SCREEN_WATCHES"] = str(watches)
    if any('ownership_reclaim' in item and item.get('peer') == peer['name'] for item in scenario.get('checklist', [])):
        environment.update(CC_TEST_CROSS_RECORDS=str(Path(tokens['VIDEO']).parent / 'events.jsonl'),
                           CC_TEST_CROSS_INSTANCE=peer['name'], CC_TEST_CROSS_INCARNATION=str(peer.get('incarnation', 0)))
    # Frames stream into one encoder process as they land (H11): no capture spools its pictures to disk first.
    ffmpeg = find_ffmpeg()
    if ffmpeg:
        environment["CCCP_TEST_RECORD_ENCODER"] = str(ffmpeg)
        environment["CCCP_TEST_RECORD_CODEC"] = encoder_codec(ffmpeg)
    environment.update(substitute(peer.get("env", {}), tokens))
    if environment["CCCP_HEADLESS"] != "1":
        raise ValueError("a scenario cannot override CCCP_HEADLESS=1")
    return environment


def drop_peer(handle, reason="scenario drop"):
    """Kills one peer through its runner. A peer that already finished is not a failure here."""
    try:
        handle.terminate(reason=reason)
    except RuntimeError:
        pass


def peer_arguments(peer, tokens, fps):
    args = ["-record-video", str(tokens["VIDEO"]), "-record-video-fps", str(fps)]
    if peer.get("menu_script"):
        args += ["-menu-script", str(tokens["MENU_SCRIPT"])]
    return args + [str(value) for value in substitute(peer.get("args", []), tokens)]


def gameplay_signals(video, stage, epochs=1):
    last_tick, starts = None, []
    for row in read_index(video):
        if row.get("screen") != "game":
            continue
        tick = row.get("sim_tick", 0)
        if last_tick is None or tick < last_tick:
            starts.append(row)
        last_tick = tick
    for index, row in enumerate(starts[:epochs], 1):
        path = Path(stage) / ("gameplay-started.json" if index == 1 else f"gameplay-epoch-{index}.json")
        if not path.exists():
            write_json(path, row)


def run_one(options, scenario, run, run_index, out):
    """One scenario run: its peers launched together, each recording its own video."""
    root = Path(out) / run.get("name", f"run{run_index}")
    dry = getattr(options, "dry_run", False)
    if not dry:
        root.mkdir(parents=True, exist_ok=False)
    size = options.size or run.get("size") or scenario.get("size") or DEFAULT_SIZE
    width, height = (int(part) for part in size.split("x"))
    port = port_for(run_index, options.port)
    timeout = run.get("timeout_s") or scenario.get("timeout_s") or 300
    runs, records, staged = {}, {}, {}
    failed = threading.Event()
    stop_watchers = threading.Event()
    peers = run.get("peers") or scenario.get("peers") or []
    if not peers:
        raise SystemExit(f"{scenario['path']}: run {run_index} names no peers")

    # Every peer's staging paths are known before any script is written, so a paired script can name
    # the other peer's probe directory and done file.
    shared = {**getattr(options, "tokens", {}), **getattr(options, "resume_tokens", {}),
              "REPO": Path(options.repo).resolve(), "PORT": port, "OUT": root, "SIZE": size,
              **source_tokens(options.repo),
              "WIDTH": width, "HEIGHT": height, "FPS": options.fps, **getattr(options, "service_tokens", {})}
    for peer in peers:
        stage = root / f"{peer['name']}-stage"
        shared[f"STAGE_{peer['name']}"] = stage
        shared[f"PROBE_DIR_{peer['name']}"] = stage / "probe"
        shared[f"VIDEO_{peer['name']}"] = root / peer["name"] / "video"

    for peer in peers:
        name = peer["name"]
        peer_root = root / name
        # make_run owns the run directory, so this peer's scripts live beside it, never inside it.
        stage = root / f"{name}-stage"
        tokens = {**shared, "PEER": name, "STAGE": stage, "PROBE_DIR": stage / "probe",
                  "MENU_SCRIPT": stage / "menu.txt", "INPUT_SCRIPT": stage / "input.txt",
                  "VIDEO": peer_root / "video"}
        args = peer_arguments(peer, tokens, options.fps)
        # Every peer of a multi-peer run hashes its whole capture every N committed ticks; the pairs are compared after.
        if getattr(options, "fullstate_every", 0) and len(peers) > 1:
            args += ["-net-fullstate-hash-every", str(options.fullstate_every)]
        reference = peer.get("retain_runtime_from")
        seed = {"ResolutionX": width, "ResolutionY": height}
        seed.update(substitute(peer.get("settings", {}), tokens))
        seed.update(dict(getattr(options, "setting", [])))
        cap = render_cap_hz(getattr(options, "render_cap", 60))
        arm = {"size": f"{seed['ResolutionX']}x{seed['ResolutionY']}", "settings": seed, "render_cap": cap}
        if dry:
            runtime = peer_root / "runtime"
            if reference:
                previous = staged.get(reference["peer"]) if reference["run"] == root.name else prior_peer(
                    getattr(options, "completed_runs", []), reference)
                runtime = Path(previous["runtime"]) if previous else Path(out) / reference["run"] / reference["peer"] / "runtime"
            args += ["-feel-render-settings", str(runtime / "Userdata/FeelRender.ini")]
            staged[name] = {"peer": name, "args": args, "runtime": str(runtime), **arm}
            continue
        stage.mkdir(parents=True, exist_ok=False)
        environment = stage_peer(scenario, peer, stage, tokens)
        retained = None
        if reference:
            previous = prior_peer(getattr(options, "completed_runs", []), reference)
            retained = Path(runs[reference["peer"]].cwd).resolve() if reference["run"] == root.name else Path(previous.get("runtime", Path(previous["root"]) / "runtime")).resolve()
            if not retained.is_relative_to(Path(out).resolve()):
                raise ValueError(f"Retained runtime leaves this capture: {retained}")
            console = retained / "LogConsole.txt"
            if previous and console.is_file():
                (Path(previous["root"]) / "console-before-restore.log").write_bytes(console.read_bytes())
        run_handle = make_run(options.repo, args, peer_root, timeout, env=environment, **({"runtime": retained} if retained else {}))
        (Path(run_handle.out) / "video").mkdir(parents=True, exist_ok=False)
        for directory in peer.get("output_dirs", []):
            (Path(run_handle.out) / directory).mkdir(parents=True, exist_ok=False)
        seed_settings(run_handle, seed)
        render_path = Path(run_handle.cwd) / "Userdata/FeelRender.ini"
        render_path.write_text(f"RenderCapHz = {cap}\n", encoding="utf-8")
        render_args = ["-feel-render-settings", str(render_path)]
        run_handle.argv.extend(render_args)
        args += render_args
        runtime_manifest = Path(run_handle.out) / "runtime.json"
        if runtime_manifest.is_file():
            metadata = json.loads(runtime_manifest.read_text(encoding="utf-8"))
            arm["settings"] = metadata["settings_overrides"] = {**metadata.get("settings_overrides", {}), **seed}
            metadata["settings_sha256"] = file_evidence(Path(run_handle.cwd) / "Userdata/Settings.ini")["sha256"]
            metadata["feel_render_settings"] = file_evidence(render_path)
            metadata["render_cap"] = cap
            write_json(runtime_manifest, metadata)
        # Fixture modules a scenario needs land in the private runtime, never in the repository.
        for entry in substitute(peer.get("runtime_files", []), tokens):
            destination = Path(run_handle.cwd) / entry["to"]
            destination.parent.mkdir(parents=True, exist_ok=True)
            if "copy" in entry:
                source = Path(options.repo) / entry["copy"]
                # An unpacked package carries the game alone: a harness fixture comes from the driver's own tree.
                if not source.is_file() and (Path(options.repo) / "MANIFEST.json").is_file():
                    source = TOOLS.parent / entry["copy"]
                destination.write_bytes(staged_copy(source, entry.get("replace", [])))
            else:
                destination.write_text(entry["write"], encoding="utf-8")
        runs[name] = run_handle
        staged[name] = {**arm, "args": args, "env": {k: str(v) for k, v in environment.items()},
                        "runtime": str(run_handle.cwd), "retain_runtime_from": reference,
                        "stage": str(stage), "probe_dir": str(stage / "probe"),
                        "gameplay_signal": str(stage / "gameplay-started.json"),
                        "start_delay_s": peer.get("start_delay_s", 0)}

    if dry:
        return {"name": root.name, "root": str(root), "size": size, "port": port, "peers": list(staged.values())}

    def drive(name):
        try:
            record = runs[name].start().finish()
            console = Path(runs[name].cwd) / "LogConsole.txt"
            if console.is_file():
                (Path(runs[name].out) / "console.log").write_bytes(console.read_bytes())
            records[name] = record
        except Exception as error:  # the peer's record carries the failure; the others still finish
            records[name] = {"error": repr(error)}
        observe = next(peer.get("observe_after_failure", False) for peer in peers if peer["name"] == name)
        if not observe and (records[name].get("error") or menu_script_failures(runs[name].out) or
                            records[name].get("exit_code") not in (0, None) and not records[name].get("injected_termination")):
            failed.set()

    threads, killers = [], []
    def gate_met(gate):
        if gate.get("run"):
            return cross_run_ready(getattr(options, "completed_runs", []), gate)
        if gate.get("directory_listed") and not directory_session(shared.get("DIRECTORY_ROOT"), port):
            return False
        name = gate["peer"]
        if gate.get("ended"):
            return name in records and records[name].get("exit_code") is not None
        if gate.get("probe_complete"):
            return completed_probe(Path(shared[f"PROBE_DIR_{name}"]) / "net-ui-result.json")
        if gate.get("log"):
            return log_line_seen(root / name / "stdout.log", gate["log"])
        video = Path(shared[f"VIDEO_{name}"])
        if "sim_tick" in gate:
            return any(row.get("screen") == "game" and row.get("sim_tick", 0) >= gate["sim_tick"] for row in read_index(video))
        path = video / "events.jsonl"
        return path.is_file() and gate["event"] in path.read_text(encoding="utf-8", errors="replace")

    def kill_at_tick(name, tick):
        video = Path(shared[f"VIDEO_{name}"])
        while not stop_watchers.wait(.05):
            if name in records:
                return
            rows = read_index(video)
            hit = next((row for row in reversed(rows) if row.get("screen") == "game" and row.get("sim_tick", 0) >= tick), None)
            if hit:
                write_json(video / "injected-drop.json", {"requested_tick": tick, "last_recorded_frame": hit})
                drop_peer(runs[name], f"scenario drop after recorded tick {tick}")
                return

    def kill_when(name, gates):
        gates = gates if isinstance(gates, list) else [gates]
        while not stop_watchers.wait(.05):
            if name in records:
                return
            gate = next((candidate for candidate in gates if gate_met(candidate)), None)
            if gate:
                # A peer dropped because a probe finished ends a scenario, not a fault: its recorder writes the
                # frames it already took first, so the probe's last marked window keeps its video.
                flushed = await_recorder(shared[f"VIDEO_{gate['peer']}"], stop_watchers) if gate.get("probe_complete") else None
                rows = read_index(shared[f"VIDEO_{name}"])
                write_json(Path(shared[f"VIDEO_{name}"]) / "injected-drop.json", {"requested_event": gate, "last_recorded_frame": rows[-1] if rows else None,
                                                                                  "recorder_flush": flushed})
                description = ("completed peer probe" if gate.get("probe_complete") else "peer log line " + gate["log"] if gate.get("log")
                               else f"{gate['peer']} sim tick {gate['sim_tick']}" if "sim_tick" in gate else "peer event " + gate["event"])
                drop_peer(runs[name], "scenario drop after " + description)
                return

    def kill_after_seconds(name, seconds):
        video = Path(shared[f'VIDEO_{name}'])
        rows = read_index(video)
        write_json(video / 'injected-drop.json', dict(requested_seconds=seconds, last_recorded_frame=rows[-1] if rows else None))
        drop_peer(runs[name], f'scenario drop after {seconds} seconds')

    interrupted, stop_finding, footprint = None, None, {"peak": 0}
    try:
        for peer in peers:
            name = peer["name"]
            delay = float(peer.get("start_delay_s", 0) or 0)
            if delay:
                # A menu-driven join has to find the host's lobby already listening, and a late joiner
                # has to find a world already running.
                if failed.wait(delay):
                    records[name] = {"error": "not launched because an earlier peer failed"}
                    break
            gate = peer.get("start_when")
            if gate:
                deadline = time.monotonic() + gate.get("timeout_s", 90)
                while not gate_met(gate):
                    if failed.wait(.1) or time.monotonic() >= deadline:
                        records[name] = {"error": f"start gate not reached: {gate}"}
                        failed.set()
                        break
                if failed.is_set():
                    break
                if failed.wait(float(peer.get("after_gate_delay_s", 0))):
                    break
            session = directory_session(shared.get("DIRECTORY_ROOT"), port)
            if session and bind_directory_session(Path(staged[name]["stage"]) / "menu.txt", session):
                staged[name]["directory_session"] = session
            thread = threading.Thread(target=drive, args=(name,))
            threads.append(thread)
            thread.start()
            # A scenario that drops a peer kills it through the runner, never by name or by PID.
            kill_after = float(peer.get("kill_after_s", 0) or 0)
            if kill_after:
                timer = threading.Timer(kill_after, kill_after_seconds, args=(name, kill_after))
                timer.daemon = True
                timer.start()
                killers.append(timer)
            if peer.get("kill_at_tick"):
                watcher = threading.Thread(target=kill_at_tick, args=(name, int(peer["kill_at_tick"])), daemon=True)
                watcher.start()
                killers.append(watcher)
            if peer.get("kill_when"):
                watcher = threading.Thread(target=kill_when, args=(name, peer["kill_when"]), daemon=True)
                watcher.start()
                killers.append(watcher)
        def measure_footprint():
            # A full scratch takes seconds to walk; the loop below writes the probes' gameplay signal and never waits on it.
            while True:
                footprint["peak"] = note_footprint(options.scratch_root, footprint["peak"])
                if stop_watchers.wait(10):
                    return

        measurer = threading.Thread(target=measure_footprint, daemon=True)
        measurer.start()
        killers.append(measurer)
        while any(thread.is_alive() for thread in threads):
            request = Path(out) / "stop-request.json"
            if request.is_file():
                reason = request.read_text(encoding="utf-8").strip()
                try:
                    finding = json.loads(reason)
                    if finding.get("class") in ("engine", "harness", "data") and finding.get("reason"):
                        stop_finding = finding
                except (ValueError, AttributeError):
                    pass
                raise RuntimeError("capture stop requested: " + reason)
            for name in runs:
                signal = Path(staged[name]["gameplay_signal"])
                epochs = next(peer.get("gameplay_epochs", 1) for peer in peers if peer["name"] == name)
                if not signal.exists() or epochs > 1:
                    gameplay_signals(shared[f"VIDEO_{name}"], staged[name]["stage"], epochs)
            if failed.is_set():
                for handle in runs.values():
                    drop_peer(handle, "another scenario peer failed")
            for thread in threads:
                thread.join(.1)
    except (KeyboardInterrupt, Exception) as error:
        interrupted = f"{type(error).__name__}: {error}"
        for handle in runs.values():
            drop_peer(handle, "capture interrupted: " + interrupted)
    finally:
        stop_watchers.set()
        for thread in threads:
            thread.join()
    for timer in killers:
        if isinstance(timer, threading.Timer):
            timer.cancel()
        else:
            timer.join()
    for name, handle in runs.items():
        handle.close()
    footprint_peak = note_footprint(options.scratch_root, footprint["peak"])

    collected = []
    for peer in peers:
        name = peer["name"]
        peer_root = root / name
        video_dir = peer_root / "video"
        console = Path(runs[name].cwd) / "LogConsole.txt"
        if console.is_file() and not (peer_root / "console.log").is_file():
            (peer_root / "console.log").write_bytes(console.read_bytes())
        collected.append({"peer": name, "root": str(peer_root), "video_dir": str(video_dir),
                          "expected_termination": bool(peer.get("kill_after_s") or peer.get("kill_at_tick") or peer.get("kill_when")),
        "record": {k: records.get(name, {}).get(k) for k in
                                     ("exit_code", "timed_out", "pid", "elapsed_seconds", "exe_sha256", 'exe_path', 'runner', 'package_unpacked',
                                      "private_desktop", "input_desktop_before", "input_desktop_after", "injected_termination")},
                          "launch": str(peer_root / "launch.json"),
                          "error": records.get(name, {}).get("error"),
                          "manifest": read_manifest(video_dir), "index": read_index(video_dir),
                          "menu_script_failures": menu_script_failures(peer_root), **staged[name]})
    fullstate = None
    if getattr(options, "fullstate_every", 0) and len(peers) > 1:
        first = peers[0]["name"]
        fullstate = {f"{first}/{peer['name']}": fullstate_verdict(root / first / "stdout.log", root / peer["name"] / "stdout.log")
                     for peer in peers[1:]}
    return {"name": root.name, "root": str(root), "size": size, "port": port, "peers": collected, "interrupted": interrupted, "stop_finding": stop_finding,
            "fullstate": fullstate, "footprint_peak_bytes": footprint_peak}


def render(capture_run, fps, every):
    """Encodes missing media while preserving a completed peer's retained output."""
    ffmpeg = find_ffmpeg()
    for peer in capture_run["peers"]:
        if peer.get("video") and Path(peer["video"]).is_file() and peer.get("contact_sheet") and Path(peer["contact_sheet"]).is_file():
            continue
        root = Path(peer["root"])
        video = encode(ffmpeg, peer["video_dir"], fps, root.parent / f"{peer['peer']}.mp4")
        peer["video"] = video.get("path") if video.get("encoded") else None
        peer["encode"] = video
        sheet = contact_sheet(peer["video_dir"], peer["index"], root.parent / f"{peer['peer']}-sheet.png", every, ffmpeg, peer["video"])
        peer["contact_sheet"] = sheet.get("path") if sheet.get("written") else None
        peer["sheet"] = sheet
    capture_run["ffmpeg"] = ffmpeg
    return capture_run


def review_only(options):
    root = Path(options.review_only)
    paths = [root / "review.json"] if (root / "review.json").is_file() else sorted(root.glob("*/review.json"))
    if not paths:
        raise SystemExit(f"no review.json under {root}")
    complete = True
    for path in paths:
        document = json.loads(path.read_text(encoding="utf-8"))
        print(f"{document['scenario']}: {document.get('verdict', 'unreviewed')} ({path})")
        for finding in document.get("run_findings", []):
            print(f"  run finding: {finding}")
            complete = False
        for item in document["checklist"]:
            print(f"  {item.get('run', path.parent.name)}/{item.get('peer', '?')} {item['id']}: "
                  f"frames={item.get('frames')} probe={item.get('probe', 'none')} {item.get('what', '')}")
            if item.get("finding") or item.get("blocked_by"):
                print(f"    finding: {item.get('finding') or item['blocked_by']}")
            complete &= item.get("frames") is not None and not item.get("blocked_by")
            complete &= not item.get("finding") and item.get("probe") not in ("fail", "not-reached", "unreadable")
    return 0 if complete else 1


def finalizer_roots(capture, out, allow_recorded_root=False):
    """Validate every review destination before writes, rebasing a copied run only by explicit request."""
    def contained(path):
        path = Path(path)
        if not path.resolve().is_relative_to(out):
            raise ValueError(f'finalize recorded root/path leaves given root: {path}')
        return path

    def child(root, name):
        if not isinstance(name, str) or name in ('', '.', '..') or Path(name).name != name:
            raise ValueError(f'finalize recorded root has invalid child name: {name!r}')
        return contained(root / name)

    for leaf in ('capture.json', 'manifest.json', 'review.json'):
        contained(out / leaf)
    if capture.get('root') and Path(capture['root']).resolve() != out:
        if not allow_recorded_root: raise ValueError(f'finalize recorded root differs from given root: {capture["root"]} != {out}')
        capture['recorded_root'] = capture['root']; capture['root'] = str(out)
    definitions = capture['scenario_definition'].get('runs') or [{'name': 'run0', 'peers': capture['scenario_definition'].get('peers', [])}]
    for index, definition in enumerate(definitions):
        root = child(out, definition.get('name', f'run{index}'))
        contained(root / 'review.json')
        for peer in definition.get('peers') or capture['scenario_definition'].get('peers', []):
            child(root, peer['name'])
    for run in capture['runs']:
        root = child(out, run['name'])
        old = Path(run.get('root') or root).absolute()
        if old.resolve() != root.resolve():
            if not allow_recorded_root:
                raise ValueError(f'finalize recorded root differs from given root: {old} != {root}; use --allow-recorded-root to rebase the copy')
            run['recorded_root'] = str(old)
        run['root'] = str(root)
        contained(root / 'review.json')
        for peer in run.get('peers', []):
            expected = child(root, peer['peer'])
            for field in ('root', 'video_dir', 'probe_dir', 'launch', 'video', 'contact_sheet', 'stage'):
                if not peer.get(field): continue
                recorded = Path(peer[field]).absolute()
                if recorded.is_relative_to(old):
                    rebased = root / recorded.relative_to(old)
                else:
                    raise ValueError(f'finalize recorded root does not own {field}: {recorded}')
                if recorded.resolve() != rebased.resolve() and not allow_recorded_root:
                    raise ValueError(f'finalize recorded root differs for {field}: {recorded}')
                peer[field] = str(contained(rebased))
            if Path(peer.get('root', expected)).resolve() != expected.resolve():
                raise ValueError(f'finalize recorded root differs for peer {peer["peer"]}')
            for leaf in ('stdout.reclaim-samples.log', 'video/timeline.ffconcat', 'video/capture.mp4', 'video/frames', 'feel/raw.jsonl.gz'):
                contained(expected / leaf)
            contained(root / f'{peer["peer"]}.mp4')
            contained(root / f'{peer["peer"]}-sheet.png')


def finalize_only(options):
    out = options.finalize_only.resolve()
    capture = json.loads((out / "capture.json").read_text(encoding="utf-8"))
    finalizer_roots(capture, out, getattr(options, 'allow_recorded_root', False))
    scenario = capture["scenario_definition"]
    existing = {run["name"]: run for run in capture["runs"]}
    prior_manifest = json.loads((out / "manifest.json").read_text(encoding="utf-8")) if (out / "manifest.json").is_file() else {}
    recovered = []
    for index, definition in enumerate(scenario.get("runs") or [{"name": "run0", "peers": scenario.get("peers", [])}]):
        name = definition.get("name", f"run{index}")
        root = out / name
        definitions = definition.get("peers") or scenario.get("peers", [])
        if not any((root / peer["name"] / "launch.json").is_file() for peer in definitions):
            if existing.get(name, {}).get("skip_finding"):
                recovered.append(existing[name])
            continue
        run = existing.get(name, {"name": name, "root": str(root), "size": capture.get("size") or definition.get("size") or scenario.get("size") or DEFAULT_SIZE, "peers": []})
        prior = {peer["peer"]: peer for peer in run["peers"]}
        peers = []
        for definition_peer in definitions:
            peer_name = definition_peer["name"]
            peer_root = root / peer_name
            launch_path = peer_root / "launch.json"
            launch = json.loads(launch_path.read_text(encoding="utf-8")) if launch_path.is_file() else {}
            video = peer_root / "video"
            peer = prior.get(peer_name, {"peer": peer_name, "root": str(peer_root), "video_dir": str(video),
                                        "probe_dir": str(root / f"{peer_name}-stage" / "probe"), "launch": str(launch_path),
                                        "args": launch.get("argv"), "env": launch.get("env_set"),
                                        "expected_termination": bool(definition_peer.get("kill_after_s") or definition_peer.get("kill_at_tick") or definition_peer.get("kill_when"))})
            peer["record"] = {key: launch.get(key) for key in ("exit_code", "timed_out", "pid", "elapsed_seconds", "exe_sha256", 'exe_path', 'runner', 'package_unpacked',
                              "private_desktop", "input_desktop_before", "input_desktop_after", "injected_termination")}
            peer["manifest"] = read_manifest(video)
            peer["index"] = read_index(video)
            peer["menu_script_failures"] = menu_script_failures(peer_root)
            if launch.get("exit_code") is None:
                peer["error"] = "The recorder owner ended before the runner saved an exit record"
                run["interrupted"] = peer["error"]
            peers.append(peer)
        run["peers"] = peers
        recovered.append(run)
    capture["runs"] = recovered
    finalizer_roots(capture, out)
    incomplete = len(recovered) != len(scenario.get("runs") or [None]) or any(run.get("interrupted") for run in recovered)
    if incomplete and not capture.get("interrupted"):
        capture["interrupted"] = "Finalized after the capture owner ended; unstarted runs remain findings"
    capture["finalized"] = stamp()
    budget = options.scratch_root or capture.get("scratch_root") or next((parent for parent in out.parents if parent.parent == Path("D:/mx")), out)
    limit = scratch_limit(options, capture)
    capture.update(scratch_root=str(budget), scratch_limit_bytes=limit)
    for run in recovered:
        if not options.metadata_only:
            render(run, capture["fps"], options.sheet_every)
            retire_transients(run)
            retained_footprint(run, budget, limit)
        review(scenario, run, Path(run["root"]))
    start = parse_stamp(capture['started'])
    end = parse_stamp(capture['finalized'])
    scenario_manifest(capture, out, prior_manifest.get("wall_seconds", (end - start).total_seconds()))
    aggregate_review(capture, out)
    for run in recovered:
        for peer in run["peers"]:
            peer.pop("index", None)
    write_json(out / "capture.json", capture)
    print(f"Finalized retained evidence: {out}; metadata_only={options.metadata_only}")
    return 1


def scenario_manifest(capture, out, elapsed):
    peers = []
    for run in capture["runs"]:
        for peer in run["peers"]:
            peers.append({"run": run["name"], "peer": peer["peer"], "size": peer.get("size", run["size"]),
                          "settings": peer.get("settings", {}), "render_cap": peer.get("render_cap"),
                          "fps": capture["fps"], "frames": len(peer["index"]),
                          "wall_seconds": peer["record"].get("elapsed_seconds"),
                          "exe_sha256": peer["record"].get("exe_sha256"),
                          "video": file_evidence(peer["video"]) if peer.get("video") else None,
                          "contact_sheet": file_evidence(peer["contact_sheet"]) if peer.get("contact_sheet") else None,
                          "launch": peer.get("launch"), "recorder": peer["manifest"]})
            identity = Path(peer.get("probe_dir", "")) / "match-identity.json"
            if identity.is_file():
                peer["match_identity"] = json.loads(identity.read_text(encoding="utf-8"))
                peers[-1]["match_identity"] = peer["match_identity"]
    manifest = {"schema": 1, "scenario": capture["scenario"], "source": capture["source"],
                "exe": capture["exe"], "started": capture["started"], "finished": stamp(),
                "wall_seconds": round(elapsed, 3), "fps": capture["fps"], "interrupted": capture.get("interrupted"),
                "frame_count": sum(peer["frames"] for peer in peers), "peers": peers,
                "skipped_runs": [{"run": run["name"], **run["skip_finding"]} for run in capture["runs"] if run.get("skip_finding")],
                "requires_findings": capture.get("requires_findings", []),
                "scratch_root": capture.get("scratch_root"), "scratch_limit_bytes": capture.get("scratch_limit_bytes", SCRATCH_LIMIT),
                "peer_selection": capture.get("scenario_definition", {}).get("peer_selection"), "platform": capture.get("platform", sys.platform)}
    if capture.get("scenario_definition", {}).get("cross_machine"):
        from e2e.cross import record_auxiliary_evidence
        manifest["cross_auxiliary"] = record_auxiliary_evidence(capture, peers)
    write_json(Path(out) / "manifest.json", manifest)
    return manifest


def aggregate_review(capture, out):
    documents = [json.loads((Path(run["root"]) / "review.json").read_text(encoding="utf-8")) for run in capture["runs"]]
    items = [item for review_doc in documents for item in review_doc["checklist"]]
    recorded = {run["name"] for run in capture["runs"]}
    definition = capture["scenario_definition"]
    for index, run in enumerate(definition.get("runs") or [{"name": "run0", "peers": definition.get("peers", [])}]):
        name = run.get("name", f"run{index}")
        if name in recorded:
            continue
        for item in definition.get("checklist", []):
            scope = item.get("run")
            if scope and name not in ([scope] if isinstance(scope, str) else scope):
                continue
            for peer in ([item["peer"]] if item.get("peer") else [peer["name"] for peer in run.get("peers", definition.get("peers", []))]):
                items.append({**item, "run": name, "peer": peer, "frames": None, "probe": "not-run", "state": "not started",
                              "finding": {"class": "harness", "reason": capture.get("interrupted") or "Capture ended before this run"}})
    document = {"schema": 1, "scenario": capture["scenario"], "title": capture["scenario_definition"].get("title"),
                "source": capture["source"], "manifest": str(Path(out) / "manifest.json"),
                "peer_selection": capture["scenario_definition"].get("peer_selection"),
                "peers": [{"run": run["name"], "peer": peer["peer"], "size": peer.get("size", run.get("size")),
                           "settings": peer.get("settings", {}), "render_cap": peer.get("render_cap")}
                          for run in capture["runs"] for peer in run["peers"]],
                "command": capture["command"], "verdict": "agent-review-required",
                "checklist": items, "interrupted": capture.get("interrupted"),
                "run_findings": [finding for document in documents for finding in document.get("run_findings", [])],
                "reviews": [str(Path(run["root"]) / "review.json") for run in capture["runs"]]}
    write_json(Path(out) / "review.json", document)
    return document


def compare_round_histories(root, config):
    from feel.retained_resume import read_live_hashes, PER_PEER_SUBSYSTEMS
    errors, histories, ordered = [], {}, {}
    (root / 'round-traces').mkdir(exist_ok=True)
    try:
        for peer in config['peers']:
            rows = read_live_hashes(root / f'{peer}-live.jsonl')
            ordered[peer] = list(dict.fromkeys(row.get('round') for row in rows))
            histories[peer] = {}
            for row in rows:
                key = (row.get('round'), row['tick'])
                if key in histories[peer]:
                    errors.append(f'{peer}: duplicate round/tick {key}')
                histories[peer][key] = row
            if len(ordered[peer]) != config['rounds'] or None in ordered[peer]:
                errors.append(f'{peer}: expected {config["rounds"]} rounds, found {ordered[peer]}')
            for index, round_id in enumerate(ordered[peer]):
                ticks = {tick for rid, tick in histories[peer] if rid == round_id}
                # A round the host ends early for the rematch runs to its end frame; the last plays the full count.
                last = config['ticks'] if index + 1 == len(ordered[peer]) else max(ticks, default=0)
                if not ticks or ticks != set(range(1, last + 1)):
                    errors.append(f'{peer} round {round_id}: incomplete tick coverage')
                write_json(root / 'round-traces' / f'{round_id}-{peer}.json',
                           dict(round=round_id, peer=peer, tick_hashes=[r for r in rows if r.get('round') == round_id]))
        reference = config['peers'][0]
        for peer in config['peers'][1:]:
            if ordered[peer] != ordered[reference] or histories[peer].keys() != histories[reference].keys():
                errors.append(f'{peer}: round histories do not match {reference}')
            for key in histories[peer].keys() & histories[reference].keys():
                a, b = histories[reference][key], histories[peer][key]
                shared = lambda row: {k: v for k, v in row.get('subsystems', {}).items() if k not in PER_PEER_SUBSYSTEMS}
                if not (CORE | {'controller'}) <= shared(a).keys() or not a.get('sim_gated') or shared(a) != shared(b) or a.get('sim_gated') != b.get('sim_gated') or a.get('paused', False) != b.get('paused', False):
                    errors.append(f'{peer}: unequal round/tick {key}')
                    break
    except (OSError, ValueError, KeyError, TypeError) as error:
        errors.append(str(error))
    return dict(status='FAIL' if errors else 'PASS', errors=errors, rounds=ordered,
                expected_rounds=config['rounds'], expected_ticks=config['ticks'])


# What every world tick hashes, actors or none: a comparison without these would be vacuous.
WORLD_FLOOR = frozenset({'scene', 'terrain', 'sim_rng', 'lua_state'})

def hold_frame_waits(text):
    """The waits a peer took at another seat's hold frame, one of at most 50 ms per hold: the slow-player bound's own wait
    for the spike, not a steady stall. Only a hold at least 300 frames into the round counts, where the engine's steady
    stall count certainly holds its wait, so excusing it can never cover a stall the count left out."""
    start = re.search(r'\[net-lockstep\] start round=\S+ frame=(\d+)', text)
    if not start:
        return []
    holds = {int(frame) for frame in re.findall(r'\[net-match\] hold peer=\d+ frame=(\d+) AI in control', text)}
    waits = [(int(frame), int(ms)) for frame, ms in re.findall(r'\[net-frame-wait\] frame=(\d+) wait_ms=(\d+)', text)]
    return sorted({frame for frame, ms in waits if frame in holds and ms <= 50 and frame >= int(start[1]) + 300})


def native_behavior(root, spec):
    from feel.retained_resume import read_live_hashes, PER_PEER_SUBSYSTEMS
    errors, details = [], {}
    def require(condition, reason):
        if not condition: errors.append(reason)
    def load(path):
        return json.loads(path.read_text(encoding='utf-8-sig')) if path.is_file() else {}
    def fields(node, name):
        found = []
        if isinstance(node, dict):
            if name in node: found.append(node[name])
            for value in node.values(): found += fields(value, name)
        elif isinstance(node, list):
            for value in node: found += fields(value, name)
        return found
    def text(peer):
        path = root / peer / 'stdout.log'
        return path.read_text(encoding='utf-8', errors='replace') if path.is_file() else ''
    try:
        for peer, filename in spec['reports'].items():
            report = load(root / filename)
            # The service run's report carries its exit code and tick count; a lobby match's report keeps the ticks in
            # last_match and the process record keeps the exit code.
            exit_code = report['exit_code'] if 'exit_code' in report else load(root / peer / 'launch.json').get('exit_code')
            ticks = report['running_ticks'] if 'running_ticks' in report else report.get('last_match', {}).get('running_ticks', 0)
            details.setdefault('continued_play', {})[peer] = dict(exit_code=exit_code, running_ticks=ticks)
            require(exit_code == 0 and ticks >= spec['ticks'][peer], f'{peer}: incomplete continued play')
            if peer in spec.get('steady_peers', []):
                counts = fields(report, 'steady_missing_frame_stalls')
                excused = hold_frame_waits(text(peer))
                details.setdefault('hold_frame_waits', {})[peer] = excused
                require(bool(counts) and all(type(v) is int for v in counts) and sum(counts) == len(excused),
                        f'{peer}: missing or nonzero steady stall count')
        if spec['kind'] in ('held-seat', 'rehold'):
            target = spec['target']
            target_log = text(target)
            identity = re.search(r'\[net-lockstep\] start [^\n]*local_peer=(\d+)', target_log)
            require(identity is not None, 'held peer identity is absent')
            seat = int(identity[1]) if identity else -1
            histories = {}
            if spec['kind'] == 'rehold':
                # The stalls are the target's own lever arguments; each fires at the first tick at or after its request
                # that its seat is back in play, so the holds are judged against the ticks they fired on.
                argv = load(root / target / 'launch.json').get('argv', [])
                if isinstance(argv, str):
                    import ast
                    argv = ast.literal_eval(argv)
                declared = [int(str(argv[i + 1]).split(':')[0]) for i, value in enumerate(argv[:-1]) if value == '-net-test-live-stall']
                fired_stalls = list(map(int, re.findall(r'\[net-test\] live stall frame=(\d+)', target_log)))
                details['stalls'] = dict(declared=declared, fired=fired_stalls)
                require(bool(declared) and len(fired_stalls) == len(declared) and all(tick >= want for tick, want in zip(fired_stalls, declared)),
                        'a declared stall never fired')
            for peer in spec['observers']:
                log = text(peer)
                all_holds = re.findall(r'\[net-match\] hold peer=(\d+) frame=(\d+) AI in control', log)
                require(all(int(held_seat) == seat for held_seat, _ in all_holds), f'{peer}: an undeclared seat was held')
                holds = sorted(set(map(int, re.findall(rf'\[net-match\] hold peer={seat} frame=(\d+) AI in control', log))))
                reclaims = sorted(set(map(int, re.findall(rf'\[net-match\] seat-reclaimed peer={seat} frame=(\d+)', log))))
                histories[peer] = dict(holds=holds, reclaims=reclaims)
                require(len(holds) == spec['holds'], f'{peer}: missing or unscheduled hold')
                if spec['kind'] == 'rehold':
                    starts = fired_stalls
                    require(len(reclaims) == len(holds) == len(starts), f'{peer}: incomplete reclaim sequence')
                    for index, hold in enumerate(holds[:len(starts)]):
                        end = starts[index + 1] if index + 1 < len(starts) else spec['ticks'][peer]
                        require(starts[index] <= hold < end, f'{peer}: hold outside its injected stall interval')
                        require(index < len(reclaims) and hold < reclaims[index] and
                                (index + 1 == len(holds) or reclaims[index] < holds[index + 1]), f'{peer}: return re-held before its next stall')
                probe = load(root / f'{peer}-stage/probe/net-ui-result.json')
                require(probe.get('pass') and probe.get('complete'), f'{peer}: seat-state probe incomplete')
                dumps = []
                for step in probe.get('steps', []):
                    # A failed step records no observation; it is read as none, not as a harness error.
                    observation = (step.get('observed') or {}).get('menu_observation', '')
                    if isinstance(observation, str) and observation.startswith('{'):
                        observed = json.loads(observation)
                        if 'members' in observed: dumps.append(observed)
                require(bool(dumps) and all(d.get('resyncing') is False and d.get('private_catch_up') is False for d in dumps), f'{peer}: survivor state missing or privately resyncing')
                if spec['kind'] == 'held-seat':
                    require(any(any(m.get('peer') == seat and 'AI in control' in m.get('state', '') for m in d['members']) for d in dumps), f'{peer}: held seat is not shown under AI')
                else:
                    require(any(any(m.get('peer') == seat and m.get('state') and not re.search(r'held|rejoining|AI in control', m['state'], re.I)
                                    for m in d['members']) for d in dumps), f'{peer}: reclaimed seat is not shown live')
            require(all(value == next(iter(histories.values())) for value in histories.values()), 'survivors disagree about holds/reclaims')
            if spec['kind'] == 'rehold':
                completed = set(map(int, re.findall(r'\[net-match\] private catch-up complete frame=(\d+)', target_log)))
                require(all(frame in completed for value in histories.values() for frame in value['reclaims']), 'private reclaim did not complete at its activation frame')
            details['holds'] = histories
        elif spec['kind'] == 'world-continuity':
            launch = load(root / 'world' / 'launch.json')
            argv = launch.get('argv', [])
            if isinstance(argv, str):
                import ast
                argv = ast.literal_eval(argv)
            require('-net-persistent-world' in argv and '-net-world-fresh' in argv,
                    'world did not launch with its persistent/fresh flags')
            host = read_live_hashes(root / 'world-live.jsonl')
            canonical = {(row.get('round'), row['tick']): row for row in host}
            require(len(canonical) == len(host), 'world duplicated a committed tick')
            require(len({row.get('round') for row in host}) == 1 and all(row.get('round') is not None for row in host), 'world round was restarted or not identified')
            require(set(range(1, spec['ticks']['world'] + 1)) <= {r['tick'] for r in host}, 'world stopped ticking across visits')
            world_round = next(iter({row.get('round') for row in host}), None)
            for peer in ('client-first', 'client-late'):
                rows = read_live_hashes(root / f'{peer}-live.jsonl')
                require(bool(rows), f'{peer}: no world hash history')
                # A joiner labels the ticks it replays before it adopts the round with its own round index: one label, on a
                # prefix wholly before its first tick under the world's round, is that round's catch-up.
                first_live = min((row['tick'] for row in rows if row.get('round') == world_round), default=None)
                others = {row.get('round') for row in rows if row.get('round') != world_round}
                prefix = len(others) == 1 and first_live is not None and all(row['tick'] < first_live for row in rows if row.get('round') != world_round)
                require(not others or prefix, f'{peer}: rows under a round the world never ran')
                mapped = {label: world_round for label in others} if prefix else {}
                details.setdefault('round_mapping', {})[peer] = {str(label): str(target) for label, target in mapped.items()}
                ticks = {row['tick'] for row in rows}
                require(len(ticks) >= spec['ticks'][peer] and ticks == set(range(min(ticks, default=0), max(ticks, default=-1) + 1)),
                        f'{peer}: incomplete continued-play history')
                if peer == 'client-late':
                    require(any(r['tick'] >= spec['late_tick'] for r in rows), 'late join preceded the declared world boundary')
                for row in rows:
                    other = canonical.get((mapped.get(row.get('round'), row.get('round')), row['tick']))
                    shared = lambda value: {k: v for k, v in value.get('subsystems', {}).items() if k not in PER_PEER_SUBSYSTEMS}
                    # A world with no actors in play hashes none: the joiner must carry exactly the world's subsystems, never fewer.
                    world_state = (CORE | {'controller'}) & shared(other).keys() if other else CORE
                    if other is None or not WORLD_FLOOR <= shared(row).keys() or not world_state <= shared(row).keys() or not row.get('sim_gated') or shared(row) != shared(other) or row.get('sim_gated') != other.get('sim_gated'):
                        errors.append(f'{peer}: world state differs at tick {row["tick"]}')
                        break
        else:
            errors.append('unknown behavior gate')
    except (OSError, ValueError, KeyError, TypeError, StopIteration) as error:
        errors.append(str(error))
    return dict(status='FAIL' if errors else 'PASS', errors=errors, evidence=details)


def feel_probes(run, capture, source):
    if run.get('backdrop_refit_gate'):
        from test_backdrop_refit import verdict
        config = run['backdrop_refit_gate']
        logs = [Path(capture['root']) / config[role] / 'stdout.log' for role in ('host', 'client')]
        checked = verdict(*(path.read_text(encoding='utf-8', errors='replace') if path.is_file() else '' for path in logs))
        result = dict(checked, status='PASS' if checked['pass'] else 'FAIL')
        write_json(Path(capture['root']) / 'backdrop-refit.json', result)
        for peer in capture['peers']:
            peer.setdefault('gates', {})['backdrop-refit'] = result
    for key, name, check in (('round_hash_gate', 'all-round-hashes', compare_round_histories),
                             ('behavior_gate', 'scenario-behavior', native_behavior)):
        if run.get(key):
            result = check(Path(capture['root']), run[key])
            write_json(Path(capture['root']) / (name + '.json'), result)
            for peer in capture['peers']:
                peer.setdefault('gates', {})[name] = result
    if run.get("migration_gate"):
        migration_probes(run["migration_gate"], capture)
    if run.get("hash_gate"):
        config = run["hash_gate"]
        root = Path(capture["root"])
        try:
            result = compare_hash_range(*(root / f"{name}_trace.json" for name in config["peers"]), config["first_tick"], config["cap"], root / config["name"])
        except (OSError, ValueError, KeyError, IndexError) as error:
            result = {"status": "FAIL", "reason": str(error), "exclusions": []}
        result["evidence"] = str(root / (config["name"] + ".json"))
        write_json(result["evidence"], result)
        for peer in capture["peers"]:
            if peer["peer"] in config["peers"]:
                peer.setdefault("gates", {})[config["name"]] = result
    if not run.get("feel_gate"):
        return
    from feel.report import item9a_gates
    root = Path(capture["root"])
    write_json(root / "manifest.json", {**run["feel_gate"], "source": source})
    result = {}
    for peer in capture["peers"]:
        if peer["peer"] not in ("host", "survivor"):
            continue
        try:
            result[peer["peer"]] = item9a_gates(root, peer["peer"])
            peer.setdefault('gates', {}).update(result[peer['peer']]['pins'])
        except Exception as error:
            result[peer["peer"]] = {"pass_check": False, "error": repr(error), "pins": {}}
            peer.setdefault('gates', {})
    # The gates average over a fixed tick window (feel_gate "ticks" ends it), so a round made longer to fit a
    # relaunch never dilutes them; the window each peer was measured over goes into review.json.
    capture["feel_window"] = {"end_tick": run["feel_gate"].get("ticks"),
                              "measured": {name: {key: (value.get("metrics") or {}).get(key) for key in ("first_tick", "last_tick")}
                                           for name, value in result.items()}}
    write_json(root / "feel-gates.json", result)


def compare_hash_range(first, second, start, cap, out):
    from compare_sim_traces import load_trace, strict_compare
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    rows, paths, validation_errors = [], [], []
    for index, source in enumerate((Path(first), Path(second))):
        try:
            load_trace(source)
        except ValueError as error:
            validation_errors.append({"path": str(source), "error": str(error)})
        data = json.loads(source.read_text(encoding="utf-8-sig"))
        selected = [row for row in data["runs"][0]["tick_hashes"] if start <= row["tick"] <= cap]
        data["runs"][0]["tick_hashes"] = selected
        path = out / f"range-{index}.json"
        write_json(path, data)
        rows.append(selected)
        paths.append(path)
    passed, strict = strict_compare(*paths, expected_ticks=cap - start + 1, first_tick=start)
    coverage = all([row["tick"] for row in trace] == list(range(start, cap + 1)) for trace in rows)
    present = [{row["tick"] for row in trace} for trace in rows]
    first_missing = next((tick for tick in range(start, cap + 1) if any(tick not in ticks for ticks in present)), None)
    first_difference = next((a["tick"] for a, b in zip(*rows) if a != b), None)
    exact = coverage and rows[0] == rows[1]
    return {"status": "PASS" if passed and exact and not validation_errors else "FAIL", "first_tick": start, "last_tick": cap,
            "first_difference": first_difference, "first_missing_tick": first_missing, "full_rows_equal": exact, "strict": strict,
            "traces": [file_evidence(first), file_evidence(second)], "exclusions": [], "validation_errors": validation_errors}


def migration_probes(config, capture):
    from e2e.timing import migration_timing
    root = Path(capture["root"])
    names = config["peers"]
    peers = [next(peer for peer in capture["peers"] if peer["peer"] == name) for name in names]
    result = {"status": "FAIL", "exclusions": []}
    try:
        declarations = [re.findall(r"(?m)^\[net-match\] Host left - (.+) is now hosting; boundary=(\d+) round=(\d+)",
                        (Path(peer["root"]) / "stdout.log").read_text(encoding="utf-8", errors="replace")) for peer in peers]
        result["declarations"] = declarations
        agreed = all(len(lines) == 1 for lines in declarations) and declarations[0] == declarations[1]
        distinct = {entry for lines in declarations for entry in lines}
        if len(distinct) != 1:
            raise ValueError("Survivors did not declare one common host, boundary and round")
        boundary = int(next(iter(distinct))[1])
        result.update(compare_hash_range(*(root / f"{name}_trace.json" for name in names), boundary + 1, config["cap"], root / "migration-hashes"))
        result["boundary"] = boundary
        if not agreed:
            result.update(status="FAIL", reason="A survivor did not declare the common host, boundary and round")
    except (OSError, ValueError, KeyError, IndexError) as error:
        result["reason"] = str(error)
    result["evidence"] = str(root / "migration-hashes.json")
    timing = migration_timing(capture, names)
    write_json(root / "migration-timing.json", timing)
    result["timing"] = str(root / "migration-timing.json")
    write_json(root / "migration-hashes.json", result)
    for peer in peers:
        peer.setdefault("gates", {})["migration-hashes"] = result


def requirement_findings(repo, scenario):
    findings = []
    for name in scenario.get("requires", []):
        path = Path(repo) / "Data" / name
        if not path.is_dir():
            findings.append({"class": "data", "reason": "Required module absent", "path": str(path)})
    for requirement in scenario.get("requires_version", []):
        path = Path(repo) / "Data" / requirement["module"] / "Index.ini"
        if not path.is_file():
            continue
        declared = re.search(r"(?m)^\s*SupportedGameVersion\s*=\s*([^\r\n]+)", path.read_text(encoding="utf-8-sig"))
        if declared and declared[1].strip() != requirement["version"]:
            findings.append({"class": "data", "reason": requirement["reason"], "index": file_evidence(path),
                             "declared": declared[1].strip(), "required": requirement["version"], "log": requirement.get("log")})
    return findings


def peer_completed(peer):
    probe_dir = Path(peer["probe_dir"])
    probe_script = probe_dir / "probe.json"
    probe_path = probe_dir / "net-ui-result.json"
    probe_result = json.loads(probe_path.read_text(encoding="utf-8")) if probe_path.is_file() else {}
    probe_ok = not probe_script.is_file() or probe_result.get("pass") and probe_result.get("complete")
    record = peer["record"]
    planned_drop = peer.get("expected_termination") and str(record.get("injected_termination", "")).startswith("scenario drop")
    exit_ok = record.get("exit_code") == 0 or planned_drop
    return bool(peer["index"] and peer.get("video") and not peer.get("error") and exit_ok and
                not record.get("timed_out") and not peer["menu_script_failures"] and probe_ok)


def main():
    global PORT_LO, PORT_HI
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path)
    parser.add_argument("--scenario")
    parser.add_argument("--peer", choices=("host", "client"))
    parser.add_argument("--merge-peer-captures", nargs=2, type=Path, metavar=("HOST_CAPTURE", "CLIENT_CAPTURE"))
    parser.add_argument("--run", action="append", default=[], help="capture only this named run, repeatable")
    parser.add_argument("--token", action="append", default=[], metavar="NAME=VALUE")
    parser.add_argument("--size")
    parser.add_argument("--setting", action="append", type=setting_pair, default=[], metavar="KEY=VALUE",
                        help="seed every peer after size and scenario settings; repeatable, last value wins")
    parser.add_argument("--render-cap", type=render_cap_hz, default=60, metavar="HZ", help="headless render cap: 0 or 60 (default)")
    parser.add_argument("--dry-run", action="store_true", help="print planned settings and engine arguments without creating files or starting services")
    parser.add_argument("--fps", type=int, default=DEFAULT_FPS)
    parser.add_argument("--port", type=int, help=f"the run block base; defaults to the scenario's port_base inside {PORT_LO}-{PORT_HI}")
    parser.add_argument("--port-block", help=f"the calling lane's own port block LO-HI, used instead of {PORT_LO}-{PORT_HI}; --port is then required")
    parser.add_argument("--sheet-every", type=int, default=15)
    parser.add_argument("--review-only", type=Path)
    parser.add_argument("--finalize-only", type=Path)
    parser.add_argument('--allow-recorded-root', action='store_true', help='rebase copied capture paths to --finalize-only; all writes stay in that given root')
    parser.add_argument("--metadata-only", action="store_true")
    parser.add_argument("--scratch-root", type=Path)
    parser.add_argument("--scratch-limit-bytes", type=positive_bytes,
                        help=f"positive byte allowance; default {SCRATCH_LIMIT}, or the retained allowance when finalizing")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--fullstate-every", type=int, default=0,
                        help="multi-peer runs: every N committed ticks each peer hashes its whole capture (-net-fullstate-hash-every); "
                             "a pair that differs is an engine finding; 0 is off")
    options = parser.parse_args()

    if options.merge_peer_captures:
        if not options.out:
            parser.error("--merge-peer-captures requires --out")
        from e2e.cross import merge_halves
        return 0 if merge_halves(options.merge_peer_captures, options.out) else 1

    if options.list:
        for path in sorted(SCENARIO_DIR.glob("*.json")):
            scenario = json.loads(path.read_text(encoding="utf-8"))
            if not scenario.get("name"):
                continue
            print(f"{scenario.get('name', path.stem):<22} {scenario.get('title', '')}")
        return 0
    if options.finalize_only:
        return finalize_only(options)
    if options.review_only:
        options.port = options.port or PORT_LO
        return review_only(options)
    if not options.scenario or not options.out:
        parser.error("--scenario and --out are required unless --review-only or --list is given")
    if not 1 <= options.fps <= 60:
        parser.error("the engine records at 1-60 fps")
    if options.sheet_every < 1:
        parser.error("--sheet-every must be positive")
    if options.fullstate_every < 0:
        parser.error("--fullstate-every must be 0 or positive")
    if options.port_block:
        low, _, high = options.port_block.partition("-")
        if not (low.isdigit() and high.isdigit() and int(low) <= int(high)) or options.port is None:
            parser.error("--port-block takes LO-HI and needs --port inside it")
        PORT_LO, PORT_HI = int(low), int(high)
    if options.port is not None and not PORT_LO <= options.port <= PORT_HI:
        parser.error(f"this driver owns ports {PORT_LO}-{PORT_HI}")
    if os.environ.get("CCCP_HEADLESS", "1") != "1":
        parser.error("CCCP_HEADLESS must stay 1: nothing this driver launches may reach a desktop")

    scenario = load_scenario(options.scenario)
    if scenario.get("cross_machine") and not options.peer:
        parser.error("a cross-machine capture requires --peer host or --peer client")
    if options.peer:
        from e2e.cross import select_peer
        scenario = select_peer(scenario, options.peer)
    options.tokens = supplied_tokens(options.token)
    # Each scenario keeps its own slice of the block, so two of them can record side by side.
    if options.port is None:
        options.port = int(scenario.get("port_base", PORT_LO))
        if not PORT_LO <= options.port <= PORT_HI:
            parser.error(f"{scenario['name']}: port_base {options.port} is outside {PORT_LO}-{PORT_HI}")
    out = Path(options.out).resolve()
    runs = scenario.get("runs") or [{"name": "run0", "peers": scenario.get("peers", [])}]
    if options.run and set(options.run) - {run.get("name", f"run{i}") for i, run in enumerate(runs)}:
        parser.error("--run names an unknown run")
    if options.dry_run:
        options.completed_runs = []
        for index, run in enumerate(runs):
            if not options.run or run.get("name", f"run{index}") in options.run:
                options.completed_runs.append(run_one(options, scenario, run, index, out))
        print(json.dumps({"scenario": scenario["name"], "dry_run": True, "runs": options.completed_runs}, indent=2))
        return 0
    out.mkdir(parents=True, exist_ok=False)
    options.scratch_root = options.scratch_root or next(
        (parent for parent in out.parents if parent.parent == Path("D:/mx")), out)
    options.scratch_limit_bytes = scratch_limit(options)
    try:
        check_scratch_budget(options.scratch_root, options.scratch_limit_bytes)
    except RuntimeError as error:
        raise SystemExit(str(error)) from None
    started = time.monotonic()
    source = source_evidence(options.repo)
    exe = file_evidence(capture_binary(options.repo))
    capture = {"schema": 1, "scenario": scenario["name"], "repo": str(Path(options.repo).resolve()), "out": str(out), "platform": sys.platform,
               "fps": options.fps, "size": options.size, "runs": [], "source": source, "exe": exe,
               "settings": dict(options.setting), "render_cap": options.render_cap,
               "scratch_root": str(options.scratch_root), "scratch_limit_bytes": options.scratch_limit_bytes,
               "started": stamp(), "command": [sys.executable, *sys.argv], "scenario_definition": scenario}
    missing = requirement_findings(options.repo, scenario)
    if missing:
        capture["requires_findings"] = missing
        write_json(out / "capture.json", capture)
        write_json(out / "review.json", {"schema": 1, "scenario": scenario["name"], "verdict": "requires-blocked",
                   "checklist": [{**item, "frames": None, "probe": "not-run", "state": "requires-blocked",
                                  "finding": {"class": "data", "reason": "; ".join(row["reason"] for row in missing), "evidence": missing}}
                                 for item in scenario["checklist"]]})
        scenario_manifest(capture, out, time.monotonic() - started)
        print(f"{scenario['name']}: requires-blocked: {missing}")
        return 2
    complete = True
    write_json(out / "capture.json", capture)
    try:
        for index, run in enumerate(runs):
            name = run.get("name", f"run{index}")
            options.completed_runs = capture["runs"]
            options.resume_tokens = {}
            skip = None
            if options.run and name not in options.run:
                skip = {"class": "harness", "reason": "Outside the explicitly selected named runs"}
            if not skip and run.get("resume_from"):
                try:
                    previous = prior_peer(capture["runs"], run["resume_from"])
                    if not previous:
                        raise ValueError("Checkpoint source peer is absent")
                    options.resume_tokens, run["checkpoint_evidence"] = checkpoint_tokens(previous)
                except (OSError, ValueError) as error:
                    skip = {"class": "harness", "reason": str(error)}
            skip = skip or run_preflight(scenario, run, capture["runs"], {**options.tokens, **options.resume_tokens})
            if skip:
                root = out / name
                root.mkdir(parents=True, exist_ok=False)
                captured = {"name": name, "root": str(root), "size": options.size or run.get("size") or scenario.get("size") or DEFAULT_SIZE,
                            "peers": [], "skip_finding": skip}
                capture["runs"].append(captured)
                review(scenario, captured, root)
                write_json(out / "capture.json", capture)
                complete = False
                continue
            service = nullcontext({})
            directory_port = directory_port_for(scenario, run, options.port)
            try:
                claim = PortClaim([port_for(index, options.port), directory_port]).__enter__()
            except RuntimeError as error:
                root = out / name
                root.mkdir(parents=True, exist_ok=False)
                captured = {"name": name, "root": str(root), "size": options.size or run.get("size") or scenario.get("size") or DEFAULT_SIZE,
                            "peers": [], "skip_finding": {"class": "harness", "reason": str(error)}}
                capture["runs"].append(captured)
                review(scenario, captured, root)
                write_json(out / "capture.json", capture)
                complete = False
                continue
            if directory_port:
                from e2e.directory import serve
                service = serve(out / f"{name}-directory", directory_port, (PORT_LO, PORT_HI), turn_config=run.get("directory_turn_config"))
            with claim, service as tokens:
                options.service_tokens = tokens
                captured = run_one(options, scenario, run, index, out)
                captured["services"] = {key: str(value) for key, value in tokens.items()}
            capture["runs"].append(captured)
            write_json(out / "capture.json", capture)
            if captured.get("interrupted"):
                capture["interrupted"] = captured["interrupted"]
            finish_run(scenario, run, captured, source, options)
            for peer in captured["peers"]:
                complete &= peer_completed(peer)
            write_json(out / "capture.json", capture)
            if captured.get("interrupted"):
                complete = False
                break
    except (KeyboardInterrupt, Exception) as error:
        capture["interrupted"] = f"{type(error).__name__}: {error}"
        complete = False
        for captured in capture["runs"]:
            captured["interrupted"] = capture["interrupted"]
            review(scenario, captured, Path(captured["root"]))
    scenario_manifest(capture, out, time.monotonic() - started)
    document = aggregate_review(capture, out)
    complete &= not document.get('run_findings') and not any(item.get("finding") or item.get("blocked_by") for item in document["checklist"])
    for run in capture["runs"]:
        for peer in run["peers"]:
            peer.pop("index", None)
    (out / "capture.json").write_text(json.dumps(capture, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"capture": str(out / "capture.json"),
                      "review": [str(Path(run["root"]) / "review.json") for run in capture["runs"]]}, indent=2))
    return 0 if complete else 1


if __name__ == "__main__":
    raise SystemExit(main())
