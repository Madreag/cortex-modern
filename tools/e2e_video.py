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
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import threading

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
    command = [ffmpeg, "-y", "-nostdin", "-loglevel", "error", "-framerate", str(fps), "-start_number", "0",
               "-i", str(frames / "frame-%06d.png"), "-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2",
               "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "20", str(destination)]
    result = subprocess.run(command, capture_output=True, text=True)
    return {"encoded": result.returncode == 0 and Path(destination).is_file(),
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
        labels.append(f"#{row['frame']} t{row.get('sim_tick', '?')} {row.get('screen', '?')}")
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
    return [line for line in text.splitlines() if "[menu-script] FAILED" in line or "[record-video] refused" in line]


def review(scenario, capture, out):
    """The reviewing agent's first read: every checklist item, where to look, and what the probe said."""
    items = []
    for item in scenario.get("checklist", []):
        peer = item.get("peer")
        peers = [peer] if peer else [row["peer"] for row in capture["peers"]]
        for name in peers:
            record = next((row for row in capture["peers"] if row["peer"] == name), None)
            if record is None:
                items.append({**item, "peer": name, "frames": None, "state": "no such peer in this capture"})
                continue
            found = frame_range(record["index"], item)
            items.append({**item, "peer": name, "frames": found,
                          "video": record.get("video"), "contact_sheet": record.get("contact_sheet"),
                          "state": "captured" if found else "not seen in the capture",
                          **probe_verdict(record.get("probe_dir", ""), item)})
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
        probe = json.loads(substitute(scenario_text(scenario, peer["probe"]), tokens))
        path.write_text(json.dumps(probe, indent=2) + "\n", encoding="utf-8")
        environment["CC_TEST_NET_UI_SCRIPT"] = str(path)
    if peer.get("input_script"):
        path = Path(root) / "input.txt"
        path.write_text(substitute(scenario_text(scenario, peer["input_script"]), tokens), encoding="utf-8")
    if peer.get("menu_script"):
        path = Path(root) / "menu.txt"
        path.write_text(substitute(scenario_text(scenario, peer["menu_script"]), tokens), encoding="utf-8")
    environment.update(substitute(peer.get("env", {}), tokens))
    return environment


def drop_peer(handle):
    """Kills one peer through its runner. A peer that already finished is not a failure here."""
    try:
        handle.terminate(reason="scenario drop")
    except RuntimeError:
        pass


def run_one(options, scenario, run, run_index, out):
    """One scenario run: its peers launched together, each recording its own video."""
    root = Path(out) / run.get("name", f"run{run_index}")
    root.mkdir(parents=True, exist_ok=False)
    size = run.get("size") or scenario.get("size") or options.size
    width, height = (int(part) for part in size.split("x"))
    port = port_for(run_index, options.port)
    timeout = run.get("timeout_s") or scenario.get("timeout_s") or 300
    runs, records, staged = {}, {}, {}
    peers = run.get("peers") or scenario.get("peers") or []
    if not peers:
        raise SystemExit(f"{scenario['path']}: run {run_index} names no peers")

    # Every peer's staging paths are known before any script is written, so a paired script can name
    # the other peer's probe directory and done file.
    shared = {"REPO": Path(options.repo).resolve(), "PORT": port, "OUT": root, "SIZE": size,
              "WIDTH": width, "HEIGHT": height, "FPS": options.fps}
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
        args = ["-record-video", str(peer_root / "video"), "-record-video-fps", str(options.fps)]
        args += [str(value) for value in substitute(peer.get("args", []), tokens)]
        run_handle = make_run(options.repo, args, peer_root, timeout, env=environment)
        (Path(run_handle.out) / "video").mkdir(parents=True, exist_ok=False)
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
                        "start_delay_s": peer.get("start_delay_s", 0)}

    def drive(name):
        try:
            records[name] = runs[name].start().finish()
        except Exception as error:  # the peer's record carries the failure; the others still finish
            records[name] = {"error": repr(error)}

    threads, killers = [], []
    for peer in peers:
        name = peer["name"]
        delay = float(peer.get("start_delay_s", 0) or 0)
        if delay:
            # A menu-driven join has to find the host's lobby already listening, and a late joiner
            # has to find a world already running.
            threading.Event().wait(delay)
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
    for thread in threads:
        thread.join()
    for timer in killers:
        timer.cancel()
    for name, handle in runs.items():
        handle.close()

    collected = []
    for peer in peers:
        name = peer["name"]
        peer_root = root / name
        video_dir = peer_root / "video"
        collected.append({"peer": name, "root": str(peer_root), "video_dir": str(video_dir),
                          "record": {k: records.get(name, {}).get(k) for k in ("exit_code", "timed_out", "pid")},
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
    capture_path = Path(options.review_only) / "capture.json"
    if not capture_path.is_file():
        raise SystemExit(f"no capture to review at {capture_path}")
    capture = json.loads(capture_path.read_text(encoding="utf-8"))
    scenario = load_scenario(capture["scenario"])
    for run in capture["runs"]:
        for peer in run["peers"]:
            peer["index"] = read_index(peer["video_dir"])
            peer["manifest"] = read_manifest(peer["video_dir"])
        render(run, capture.get("fps", options.fps), options.sheet_every)
        review(scenario, run, Path(run["root"]))
    capture_path.write_text(json.dumps(capture, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"reviewed": str(capture_path), "runs": [run["name"] for run in capture["runs"]]}, indent=2))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path)
    parser.add_argument("--scenario")
    parser.add_argument("--size", default=DEFAULT_SIZE)
    parser.add_argument("--fps", type=int, default=DEFAULT_FPS)
    parser.add_argument("--port", type=int, help=f"the run block base; defaults to the scenario's port_base inside {PORT_LO}-{PORT_HI}")
    parser.add_argument("--sheet-every", type=int, default=15)
    parser.add_argument("--review-only", type=Path)
    parser.add_argument("--list", action="store_true")
    options = parser.parse_args()

    if options.list:
        for path in sorted(SCENARIO_DIR.glob("*.json")):
            scenario = json.loads(path.read_text(encoding="utf-8"))
            print(f"{scenario.get('name', path.stem):<22} {scenario.get('title', '')}")
        return 0
    if options.review_only:
        options.port = options.port or PORT_LO
        return review_only(options)
    if not options.scenario or not options.out:
        parser.error("--scenario and --out are required unless --review-only or --list is given")
    if not 1 <= options.fps <= 60:
        parser.error("the engine records at 1-60 fps")
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
    missing = [name for name in scenario.get("requires", []) if not (Path(options.repo) / "Data" / name).is_dir()]
    if missing:
        raise SystemExit(f"{scenario['name']} needs {missing} installed under {Path(options.repo) / 'Data'}")

    out = Path(options.out).resolve()
    out.mkdir(parents=True, exist_ok=False)
    runs = scenario.get("runs") or [{"name": "run0", "peers": scenario.get("peers", [])}]
    capture = {"schema": 1, "scenario": scenario["name"], "repo": str(Path(options.repo).resolve()),
               "fps": options.fps, "size": options.size, "runs": []}
    for index, run in enumerate(runs):
        captured = run_one(options, scenario, run, index, out)
        render(captured, options.fps, options.sheet_every)
        review(scenario, captured, Path(captured["root"]))
        capture["runs"].append(captured)
    for run in capture["runs"]:
        for peer in run["peers"]:
            peer.pop("index", None)
    (out / "capture.json").write_text(json.dumps(capture, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"capture": str(out / "capture.json"),
                      "review": [str(Path(run["root"]) / "review.json") for run in capture["runs"]]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
