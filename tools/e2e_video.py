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

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from run_sim_test import make_run, seed_settings  # noqa: E402

SCENARIO_DIR = TOOLS / "e2e"
PORT_LO, PORT_HI = 49400, 49479
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
    return utc.astimezone(timezone(timedelta(hours=-7))).strftime("%Y-%m-%d %H:%M:%S MST")


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


def source_evidence(repo):
    def git(*args):
        return subprocess.check_output(["git", "-C", str(repo), *args], text=True).strip()
    return {"tip": git("rev-parse", "HEAD"), "dirty": git("status", "--porcelain").splitlines(),
            "diff_sha256": hashlib.sha256(git("diff", "HEAD").encode()).hexdigest()}


def find_ffmpeg():
    """PATH first, then the two machines' known locations; None when the encode has to be skipped."""
    found = shutil.which("ffmpeg")
    if found:
        return found
    for candidate in FFMPEG_CANDIDATES:
        if Path(candidate).is_file():
            return candidate
    return None


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


def substitute(value, tokens):
    if isinstance(value, str):
        for name, replacement in tokens.items():
            value = value.replace("{" + name + "}", str(replacement))
        return value
    if isinstance(value, list):
        return [substitute(item, tokens) for item in value]
    if isinstance(value, dict):
        return {key: substitute(item, tokens) for key, item in value.items()}
    return value


def port_for(run_index, base):
    """One port per run; every peer of a run shares the host's. Kept inside this driver's own block."""
    port = base + run_index
    if not PORT_LO <= port <= PORT_HI:
        raise SystemExit(f"port {port} is outside this driver's block {PORT_LO}-{PORT_HI}")
    return port


def read_index(video_dir):
    """The engine's per-frame index; a capture that never presented a frame leaves it empty."""
    path = Path(video_dir) / "frames.jsonl"
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


def read_manifest(video_dir):
    path = Path(video_dir) / "manifest.json"
    if not path.is_file():
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {}


def encode(ffmpeg, video_dir, fps, destination):
    """One peer's frames to h264. Even dimensions are forced because yuv420p needs them."""
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
    metadata = {}
    ffprobe = shutil.which("ffprobe")
    if result.returncode == 0 and ffprobe:
        check = subprocess.run([ffprobe, "-v", "error", "-count_frames", "-select_streams", "v:0", "-show_entries",
                                "stream=width,height,nb_read_frames,r_frame_rate:format=duration", "-of", "json", str(destination)],
                               capture_output=True, text=True)
        if check.returncode == 0:
            metadata = json.loads(check.stdout)
    return {"encoded": result.returncode == 0 and Path(destination).is_file(), "timing": "wall-clock-cfr",
            "origin_wall_ms": rows[0]["wall_ms"], "fps": fps, "ffprobe": metadata,
            "capture_duration_s": (rows[-1]["wall_ms"] - rows[0]["wall_ms"]) / 1000 + 1 / fps,
            "command": command, "returncode": result.returncode,
            "stderr": result.stderr[-2000:], "path": str(destination)}


def contact_sheet(video_dir, rows, destination, every, ffmpeg):
    """Every `every`-th frame tiled SHEET_COLUMNS wide, each thumbnail labelled with what it is."""
    frames = Path(video_dir) / "frames"
    picked = rows[::every] if rows else []
    if not picked:
        return {"written": False, "reason": "no frames to tile"}
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
    """Where the capture puts a checklist item: the frames whose screen and sim tick match what it names."""
    screen = item.get("screen")
    low, high = (item.get("sim_ticks") or [None, None])[:2]
    hits = []
    for row in rows:
        if not row.get("saved", True):
            continue
        if screen and row.get("screen") != screen:
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
    """What the peer's menu probe said about this item, as the probe itself recorded it."""
    result = Path(probe_dir) / "net-ui-result.json"
    if not probe_dir or not result.is_file():
        return {"probe": "none"}
    try:
        observed = json.loads(result.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {"probe": "unreadable"}
    return {"probe": "pass" if observed.get("pass") and observed.get("complete") else "fail",
            "complete": bool(observed.get("complete")), "path": str(result)}


def menu_script_failures(peer_root):
    text = ""
    for leaf in ("stdout.log", "stderr.log"):
        path = Path(peer_root) / leaf
        if path.is_file():
            text += path.read_text(encoding="utf-8", errors="replace")
    return [line for line in text.splitlines() if any(marker in line for marker in
            ("[menu-script] FAILED", "[record-video] refused", "[net-ui-probe] FAIL"))]


def item_evidence(record, item):
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
        end = next((event["wall_ms"] for event in events[index + 1:]
                    if event["message"].startswith("video_mark ") and not event["message"].endswith(" PASS")), float("inf"))
        rows = [row for row in rows if start <= row["wall_ms"] < end]
        events = [event for event in events if start <= event["wall_ms"] < end]
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
    elif item.get("log_regex"):
        path = Path(record["root"]) / "stdout.log"
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines() if path.is_file() else []
        assertions = [{"assertion": pattern, "matches": [{"line": index + 1, "text": line}
                       for index, line in enumerate(lines) if re.search(pattern, line)]} for pattern in item["log_regex"]]
        evidence.update(probe="pass" if all(value["matches"] for value in assertions) else "fail",
                        path=str(path), assertions=assertions)
    elif probe_steps is not None:
        path = Path(record["probe_dir"]) / "net-ui-result.json"
        observed = json.loads(path.read_text(encoding="utf-8")) if path.is_file() else {}
        steps = {step["index"]: step for step in observed.get("steps", [])}
        required = probe_steps
        evidence.update(probe="pass" if all(index in steps for index in required) else "not-reached",
                        assertions=[{"step": index, "command": observed.get("script", {}).get("steps", [])[index]
                                     if index < len(observed.get("script", {}).get("steps", [])) else None,
                                     "observed": steps.get(index)} for index in required], path=str(path))
    else:
        evidence.update(probe_verdict(record.get("probe_dir", ""), item))
    return frame_range(rows, item), evidence


def review(scenario, capture, out):
    """The reviewing agent's first read: every checklist item, where to look, and what the probe said."""
    items = []
    for item in scenario.get("checklist", []):
        scope = item.get("run")
        if scope and capture["name"] not in ([scope] if isinstance(scope, str) else scope):
            continue
        peer = item.get("peer")
        peers = [peer] if peer else [row["peer"] for row in capture["peers"]]
        for name in peers:
            record = next((row for row in capture["peers"] if row["peer"] == name), None)
            if record is None:
                items.append({**item, "peer": name, "frames": None, "state": "no such peer in this capture"})
                continue
            found, assertions = item_evidence(record, item)
            video_frames = found
            encode_result = record.get("encode", {})
            seconds = None
            if found and encode_result.get("timing") == "wall-clock-cfr":
                indexed = {row["frame"]: row for row in record["index"]}
                seconds = [(indexed[frame]["wall_ms"] - encode_result["origin_wall_ms"]) / 1000 for frame in found]
                video_frames = [round(second * encode_result["fps"]) for second in seconds]
            resolved = {**item, "peer": name, "run": capture["name"], "frames": video_frames,
                          "capture_frames": found, "video_seconds": seconds,
                          "video": record.get("video"), "contact_sheet": record.get("contact_sheet"),
                          "state": "captured" if found else "not seen in the capture",
                          **assertions}
            if not found or item.get("blocked_by") or assertions.get("probe") in ("fail", "not-reached"):
                resolved["finding"] = {"class": "unclassified", "reason": item.get("blocked_by") or assertions.get("reason") or
                                       "Required frames or assertions absent; inspect the retained launch, probe and logs",
                                       "launch": record.get("launch"), "errors": record.get("menu_script_failures", [])}
            items.append(resolved)
    document = {"schema": 1, "scenario": scenario["name"], "title": scenario.get("title", ""),
                "requires": scenario.get("requires", []),
                "reviewer_reads": ["review.json", "<peer>-sheet.png", "<peer>.mp4"],
                "peers": [{k: v for k, v in row.items() if k != "index"} for row in capture["peers"]],
                "checklist": items,
                "failures": {row["peer"]: row["menu_script_failures"] for row in capture["peers"]},
                "verdict": "agent-review-required"}
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


def run_one(options, scenario, run, run_index, out):
    """One scenario run: its peers launched together, each recording its own video."""
    root = Path(out) / run.get("name", f"run{run_index}")
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
    shared = {"REPO": Path(options.repo).resolve(), "PORT": port, "OUT": root, "SIZE": size,
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
        stage.mkdir(parents=True, exist_ok=False)
        tokens = {**shared, "PEER": name, "STAGE": stage, "PROBE_DIR": stage / "probe",
                  "MENU_SCRIPT": stage / "menu.txt", "INPUT_SCRIPT": stage / "input.txt",
                  "VIDEO": peer_root / "video"}
        environment = stage_peer(scenario, peer, stage, tokens)
        args = peer_arguments(peer, tokens, options.fps)
        run_handle = make_run(options.repo, args, peer_root, timeout, env=environment)
        (Path(run_handle.out) / "video").mkdir(parents=True, exist_ok=False)
        for directory in peer.get("output_dirs", []):
            (Path(run_handle.out) / directory).mkdir(parents=True, exist_ok=False)
        seed = {"ResolutionX": width, "ResolutionY": height}
        seed.update(substitute(peer.get("settings", {}), tokens))
        seed_settings(run_handle, seed)
        # Fixture modules a scenario needs land in the private runtime, never in the repository.
        for entry in substitute(peer.get("runtime_files", []), tokens):
            destination = Path(run_handle.cwd) / entry["to"]
            destination.parent.mkdir(parents=True, exist_ok=True)
            if "copy" in entry:
                destination.write_bytes((Path(options.repo) / entry["copy"]).read_bytes())
            else:
                destination.write_text(entry["write"], encoding="utf-8")
        runs[name] = run_handle
        staged[name] = {"args": args, "env": {k: str(v) for k, v in environment.items()},
                        "stage": str(stage), "probe_dir": str(stage / "probe"),
                        "gameplay_signal": str(stage / "gameplay-started.json"),
                        "start_delay_s": peer.get("start_delay_s", 0)}

    def drive(name):
        try:
            records[name] = runs[name].start().finish()
        except Exception as error:  # the peer's record carries the failure; the others still finish
            records[name] = {"error": repr(error)}
        observe = next(peer.get("observe_after_failure", False) for peer in peers if peer["name"] == name)
        if not observe and (records[name].get("error") or records[name].get("exit_code") not in (0, None) and not records[name].get("injected_termination")):
            failed.set()

    threads, killers = [], []
    def gate_met(gate):
        name = gate["peer"]
        if gate.get("ended"):
            return name in records and records[name].get("exit_code") is not None
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
        thread = threading.Thread(target=drive, args=(name,))
        threads.append(thread)
        thread.start()
        # A scenario that drops a peer kills it through the runner, never by name or by PID.
        kill_after = float(peer.get("kill_after_s", 0) or 0)
        if kill_after:
            timer = threading.Timer(kill_after, lambda handle=runs[name]: drop_peer(handle))
            timer.daemon = True
            timer.start()
            killers.append(timer)
        if peer.get("kill_at_tick"):
            watcher = threading.Thread(target=kill_at_tick, args=(name, int(peer["kill_at_tick"])), daemon=True)
            watcher.start()
            killers.append(watcher)
    next_size_check = time.monotonic()
    while any(thread.is_alive() for thread in threads):
        for name in runs:
            signal = Path(staged[name]["gameplay_signal"])
            if not signal.exists():
                first = next((row for row in read_index(shared[f"VIDEO_{name}"]) if row.get("screen") == "game"), None)
                if first:
                    write_json(signal, first)
        if failed.is_set():
            for handle in runs.values():
                drop_peer(handle, "another scenario peer failed")
        for thread in threads:
            thread.join(.1)
        if time.monotonic() >= next_size_check:
            footprint = scratch_bytes(options.scratch_root)
            if footprint >= SCRATCH_LIMIT:
                for handle in runs.values():
                    drop_peer(handle)
                raise RuntimeError(f"scratch footprint {footprint} bytes reaches {SCRATCH_LIMIT}; no cleanup performed")
            next_size_check = time.monotonic() + 10
    stop_watchers.set()
    for timer in killers:
        if isinstance(timer, threading.Timer):
            timer.cancel()
        else:
            timer.join()
    for name, handle in runs.items():
        handle.close()

    collected = []
    for peer in peers:
        name = peer["name"]
        peer_root = root / name
        video_dir = peer_root / "video"
        collected.append({"peer": name, "root": str(peer_root), "video_dir": str(video_dir),
                          "expected_termination": bool(peer.get("kill_after_s") or peer.get("kill_at_tick")),
        "record": {k: records.get(name, {}).get(k) for k in
                                     ("exit_code", "timed_out", "pid", "elapsed_seconds", "exe_sha256",
                                      "private_desktop", "input_desktop_before", "input_desktop_after", "injected_termination")},
                          "launch": str(peer_root / "launch.json"),
                          "error": records.get(name, {}).get("error"),
                          "manifest": read_manifest(video_dir), "index": read_index(video_dir),
                          "menu_script_failures": menu_script_failures(peer_root), **staged[name]})
    return {"name": root.name, "root": str(root), "size": size, "port": port, "peers": collected}


def render(capture_run, fps, every):
    """Encodes and tiles every peer of one run; safe to repeat over an existing capture."""
    ffmpeg = find_ffmpeg()
    for peer in capture_run["peers"]:
        root = Path(peer["root"])
        video = encode(ffmpeg, peer["video_dir"], fps, root.parent / f"{peer['peer']}.mp4")
        sheet = contact_sheet(peer["video_dir"], peer["index"], root.parent / f"{peer['peer']}-sheet.png", every, ffmpeg)
        peer["video"] = video.get("path") if video.get("encoded") else None
        peer["encode"] = video
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
        for item in document["checklist"]:
            print(f"  {item.get('run', path.parent.name)}/{item.get('peer', '?')} {item['id']}: "
                  f"frames={item.get('frames')} probe={item.get('probe', 'none')} {item.get('what', '')}")
            if item.get("finding") or item.get("blocked_by"):
                print(f"    finding: {item.get('finding') or item['blocked_by']}")
            complete &= item.get("frames") is not None and not item.get("blocked_by")
            complete &= not item.get("finding") and item.get("probe") not in ("fail", "not-reached", "unreadable")
    return 0 if complete else 1


def scenario_manifest(capture, out, elapsed):
    peers = []
    for run in capture["runs"]:
        for peer in run["peers"]:
            peers.append({"run": run["name"], "peer": peer["peer"], "size": run["size"],
                          "fps": capture["fps"], "frames": len(peer["index"]),
                          "wall_seconds": peer["record"].get("elapsed_seconds"),
                          "exe_sha256": peer["record"].get("exe_sha256"),
                          "video": file_evidence(peer["video"]) if peer.get("video") else None,
                          "contact_sheet": file_evidence(peer["contact_sheet"]) if peer.get("contact_sheet") else None,
                          "launch": peer.get("launch"), "recorder": peer["manifest"]})
    manifest = {"schema": 1, "scenario": capture["scenario"], "source": capture["source"],
                "exe": capture["exe"], "started": capture["started"], "finished": stamp(),
                "wall_seconds": round(elapsed, 3), "fps": capture["fps"],
                "frame_count": sum(peer["frames"] for peer in peers), "peers": peers}
    write_json(Path(out) / "manifest.json", manifest)
    return manifest


def aggregate_review(capture, out):
    documents = [json.loads((Path(run["root"]) / "review.json").read_text(encoding="utf-8")) for run in capture["runs"]]
    document = {"schema": 1, "scenario": capture["scenario"], "title": capture["scenario_definition"].get("title"),
                "source": capture["source"], "manifest": str(Path(out) / "manifest.json"),
                "command": capture["command"], "verdict": "agent-review-required",
                "checklist": [item for review_doc in documents for item in review_doc["checklist"]],
                "reviews": [str(Path(run["root"]) / "review.json") for run in capture["runs"]]}
    write_json(Path(out) / "review.json", document)
    return document


def feel_probes(run, capture, source):
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
            peer["gates"] = result[peer["peer"]]["pins"]
        except Exception as error:
            result[peer["peer"]] = {"pass_check": False, "error": repr(error), "pins": {}}
            peer["gates"] = {}
    write_json(root / "feel-gates.json", result)


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
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path)
    parser.add_argument("--scenario")
    parser.add_argument("--size")
    parser.add_argument("--fps", type=int, default=DEFAULT_FPS)
    parser.add_argument("--port", type=int, help=f"the run block base; defaults to the scenario's port_base inside {PORT_LO}-{PORT_HI}")
    parser.add_argument("--sheet-every", type=int, default=15)
    parser.add_argument("--review-only", type=Path)
    parser.add_argument("--scratch-root", type=Path)
    parser.add_argument("--list", action="store_true")
    options = parser.parse_args()

    if options.list:
        for path in sorted(SCENARIO_DIR.glob("*.json")):
            scenario = json.loads(path.read_text(encoding="utf-8"))
            if not scenario.get("name"):
                continue
            print(f"{scenario.get('name', path.stem):<22} {scenario.get('title', '')}")
        return 0
    if options.review_only:
        options.port = options.port or PORT_LO
        return review_only(options)
    if not options.scenario or not options.out:
        parser.error("--scenario and --out are required unless --review-only or --list is given")
    if not 1 <= options.fps <= 60:
        parser.error("the engine records at 1-60 fps")
    if options.sheet_every < 1:
        parser.error("--sheet-every must be positive")
    if options.port is not None and not PORT_LO <= options.port <= PORT_HI:
        parser.error(f"this driver owns ports {PORT_LO}-{PORT_HI}")
    if os.environ.get("CCCP_HEADLESS", "1") != "1":
        parser.error("CCCP_HEADLESS must stay 1: nothing this driver launches may reach a desktop")

    scenario = load_scenario(options.scenario)
    # Each scenario keeps its own slice of the block, so two of them can record side by side.
    if options.port is None:
        options.port = int(scenario.get("port_base", PORT_LO))
        if not PORT_LO <= options.port <= PORT_HI:
            parser.error(f"{scenario['name']}: port_base {options.port} is outside {PORT_LO}-{PORT_HI}")
    out = Path(options.out).resolve()
    out.mkdir(parents=True, exist_ok=False)
    options.scratch_root = options.scratch_root or next(
        (parent for parent in out.parents if parent.parent == Path("D:/mx")), out)
    footprint = scratch_bytes(options.scratch_root)
    if footprint >= SCRATCH_LIMIT:
        raise SystemExit(f"scratch footprint {footprint} bytes reaches {SCRATCH_LIMIT}; no cleanup performed")
    started = time.monotonic()
    source = source_evidence(options.repo)
    exe = file_evidence(Path(options.repo) / ("Cortex Command.exe" if sys.platform == "win32" else "build-gns/CortexCommand"))
    runs = scenario.get("runs") or [{"name": "run0", "peers": scenario.get("peers", [])}]
    capture = {"schema": 1, "scenario": scenario["name"], "repo": str(Path(options.repo).resolve()),
               "fps": options.fps, "size": options.size, "runs": [], "source": source, "exe": exe,
               "started": stamp(), "command": [sys.executable, *sys.argv], "scenario_definition": scenario}
    missing = [str(Path(options.repo) / "Data" / name) for name in scenario.get("requires", [])
               if not (Path(options.repo) / "Data" / name).is_dir()]
    if missing:
        capture["requires_missing"] = missing
        write_json(out / "capture.json", capture)
        write_json(out / "review.json", {"schema": 1, "scenario": scenario["name"], "verdict": "requires-missing",
                   "checklist": [{**item, "frames": None, "probe": "not-run", "state": "requires-missing",
                                  "finding": {"class": "data", "reason": "Required module absent", "paths": missing}}
                                 for item in scenario["checklist"]]})
        scenario_manifest(capture, out, time.monotonic() - started)
        print(f"{scenario['name']}: requires-missing: {missing}")
        return 2
    complete = True
    for index, run in enumerate(runs):
        service = nullcontext({})
        if scenario.get("directory_port"):
            from e2e.directory import serve
            service = serve(out / f"{run.get('name', f'run{index}')}-directory", scenario["directory_port"])
        with service as tokens:
            options.service_tokens = tokens
            captured = run_one(options, scenario, run, index, out)
            captured["services"] = {key: str(value) for key, value in tokens.items()}
        feel_probes(run, captured, source)
        render(captured, options.fps, options.sheet_every)
        review(scenario, captured, Path(captured["root"]))
        capture["runs"].append(captured)
        for peer in captured["peers"]:
            complete &= peer_completed(peer)
        write_json(out / "capture.json", capture)
    scenario_manifest(capture, out, time.monotonic() - started)
    document = aggregate_review(capture, out)
    complete &= not any(item.get("finding") or item.get("blocked_by") for item in document["checklist"])
    for run in capture["runs"]:
        for peer in run["peers"]:
            peer.pop("index", None)
    (out / "capture.json").write_text(json.dumps(capture, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"capture": str(out / "capture.json"),
                      "review": [str(Path(run["root"]) / "review.json") for run in capture["runs"]]}, indent=2))
    return 0 if complete else 1


if __name__ == "__main__":
    raise SystemExit(main())
