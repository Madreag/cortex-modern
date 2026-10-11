"""Play a scene on the lead's named boxes with the existing game runners.

    python tools/e2e_peers.py --scenario fight15-s02 --out <capture> \
        --peer-boxes host=<BOX>,joiner=<BOX> --boxes-file <box facts>
    python tools/e2e_peers.py --check-spread <capture>

Box facts stay outside the game tree. Each box supplies name, ssh, os, hostname,
repo, scratch, python and build_receipt. Windows also supplies session_dir and
task; the Mac supplies binary. Builds come from each box's own checkout.
"""
from __future__ import annotations

import argparse
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
import time
from types import SimpleNamespace
import uuid

import e2e_video as video


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


def scene_runs(document):
    return document.get("runs") or [{"name": "run0", "peers": document["peers"]}]


def check_scene(document):
    if not document.get("public_directory"):
        raise ValueError("scene must use its public directory")
    for run in scene_runs(document):
        peers = run.get("peers", document.get("peers", []))
        if {peer["name"] for peer in peers} != {"host", "joiner"} or len(peers) != 2:
            raise ValueError("scene needs exactly one host and one joiner per run")
        for peer in peers:
            settings = peer.get("settings", {})
            if settings.get("NetworkConnectionMode") != "RelayOnly" or settings.get("NetworkPortMapEnable") != 0:
                raise ValueError("scene must select RelayOnly and disable router mapping")
            if settings.get("NetworkHostRelayMode") != "Directory" or not settings.get("SessionDirectoryUrl"):
                raise ValueError("scene must get its relay from the directory")
            if any(peer.get(key) for key in ("start_when", "retain_runtime_from", "kill_when", "kill_after_s", "kill_at_tick")):
                raise ValueError("scene needs another peer's process control; this launcher cannot run it")
            scripts = [video.scenario_text(document, peer[key]) for key in ("menu_script", "probe", "input_script") if peer.get(key)]
            if any(re.search(r"\{(?:STAGE|PROBE_DIR|VIDEO)_(?:host|joiner)\}", text) for text in scripts):
                raise ValueError("scene needs shared files between boxes; this launcher cannot run it")


class PlayerRun:
    def __init__(self, request, started):
        self.request, self.started = request, started

    def make_run(self, repo, args, out, timeout, env=None, **kwargs):
        environment = dict(env or {})
        if self.request["box"]["os"] == "darwin":
            environment.pop("CCCP_HEADLESS", None)
            environment.update(CC_TEST_VISIBLE_WINDOW="1", CCCP_POSIX_HOP="ssh")
        run = video.make_run(repo, args, out, timeout, env=environment, **kwargs)
        run.record.update(box=self.request["box"]["name"], engine_hostname=socket.gethostname(), topology="spread", proof=False,
                          source_commit=self.request["commit"], scene_sha256=self.request["scene_sha256"])
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
        raise ValueError("player is on a different box from the lead's assignment")
    repo = Path(box["repo"])
    source = video.source_evidence(repo)
    if source["tip"] != request["commit"] or source["dirty"]:
        raise ValueError("player checkout differs from the committed scene launcher")
    if box["os"] == "darwin":
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
    if video.file_evidence(document["path"])["sha256"] != request["scene_sha256"]:
        raise ValueError("player scene differs from the original scene")
    check_scene(document)
    out = Path(request["out"])
    role, index = request["role"], request["run_index"]
    run = copy.deepcopy(scene_runs(document)[index])
    name = run.get("name", "run" + str(index))
    run["peers"] = [peer for peer in run.get("peers", document.get("peers", [])) if peer["name"] == role]
    local_scene = copy.deepcopy(document)
    local_scene["checklist"] = [item for item in document.get("checklist", []) if not item.get("peer") or item["peer"] == role]
    options = SimpleNamespace(repo=Path(request["box"]["repo"]), size=None, port=request["port"], fps=request["fps"],
                              setting=[], render_cap=60, fullstate_every=0, sheet_every=request["sheet_every"],
                              tokens={}, resume_tokens={}, service_tokens={}, completed_runs=[],
                              scratch_root=Path(request["box"]["scratch"]), scratch_limit_bytes=video.SCRATCH_LIMIT,
                              remote_capture=PlayerRun(request, out / (role + "-started.json")))
    missing = video.requirement_findings(options.repo, document)
    if missing:
        raise ValueError("scene requirements failed: " + json.dumps(missing))
    video.check_scratch_budget(options.scratch_root, options.scratch_limit_bytes)
    captured = video._run_one(options, local_scene, run, index, out)
    captured.update(repo=str(options.repo), topology="spread", proof=False, peer_boxes={role: request["box"]["name"]})
    for peer in captured["peers"]:
        record_path = Path(peer["root"]) / "record.json"
        record = read(record_path) if record_path.is_file() else {}
        peer["record"].update({key: record.get(key) for key in ("box", "engine_hostname", "source_commit", "scene_sha256")})
        peer.update(box=request["box"]["name"], box_hostname=request["box"]["hostname"],
                    native_source=source, native_exe=executable, build_receipt=receipt)
    video.finish_run(local_scene, run, captured, source, options)
    (Path(captured["root"]) / "review.json").rename(Path(captured["root"]) / (role + "-review.json"))
    video.write_json(Path(captured["root"]) / (role + "-capture.json"), captured)
    video.write_json(out / (role + "-finished.json"), {"finished": video.stamp()})
    passed = all(video.peer_completed(peer) for peer in captured["peers"])
    print(f"{role}: {'GREEN' if passed else 'RED'} {captured['root']}", flush=True)
    return 0 if passed else 1


def spread_check(capture):
    reasons, observed = [], []
    if capture.get("topology") != "spread":
        reasons.append("capture topology is not spread")
    runs = capture.get("runs", [])
    if not runs:
        reasons.append("capture has no runs")
    for run in runs:
        boxes, hosts = [], []
        peers = run.get("peers", [])
        if len(peers) != 2 or {peer.get("peer") for peer in peers} != {"host", "joiner"}:
            reasons.append(run["name"] + ": host or joiner record is absent")
        for peer in peers:
            label = run["name"] + "/" + peer["peer"]
            box = peer.get("box")
            path = Path(peer["root"]) / "record.json"
            record = read(path) if path.is_file() else {}
            host = record.get("engine_hostname")
            boxes.append(str(box or "").casefold())
            hosts.append(str(host or "").casefold())
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
                             "pid": record.get("pid"), "record": str(path)})
        if len(set(boxes)) != len(peers):
            reasons.append(run["name"] + ": peers ran on one named box")
        if len(set(hosts)) != len(peers):
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
    root = Path(request["out"])
    archive = root.with_suffix(".tar")
    run_name = scene_runs(scene(str(Path(request["box"]["repo"]) / request["scene_relative"])))[request["run_index"]].get("name", "run" + str(request["run_index"]))
    with tarfile.open(archive, "w") as output:
        for directory, folders, files in os.walk(root, followlinks=False):
            folders[:] = [name for name in folders if name != "runtime" and not video.is_link(Path(directory) / name)]
            for name in files:
                path = Path(directory) / name
                if not video.is_link(path) and stat.S_ISREG(path.stat().st_mode):
                    relative = path.relative_to(root).as_posix()
                    if relative.startswith(run_name + "/"):
                        output.add(path, arcname=relative, recursive=False)
                    elif name in ("session.log", "session.done", "foreground_probe.py"):
                        output.add(path, arcname="commands/" + run_name + "-" + request["role"] + "-" + name, recursive=False)
    print(json.dumps(video.file_evidence(archive)), flush=True)


def unpack(archive, out):
    with tarfile.open(archive) as source:
        for member in source.getmembers():
            target = (out / member.name).resolve()
            if not member.isfile() or not target.is_relative_to(out) or "runtime" in Path(member.name).parts:
                raise ValueError("collected file escapes the capture or contains a runtime")
            if target.exists():
                raise ValueError("collected file would overwrite another player's evidence")
            target.parent.mkdir(parents=True, exist_ok=True)
            with source.extractfile(member) as stream, target.open("wb") as destination:
                import shutil
                shutil.copyfileobj(stream, destination)


def ssh(box, args, **kwargs):
    return subprocess.run(["ssh", "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=20", box["ssh"], shlex.join(list(map(str, args)))],
                          **kwargs)


def copy_file(box, source, destination, download=False):
    remote = box["ssh"] + ":" + str(destination)
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


def launch(box, request_path, log):
    tool = str(Path(box["repo"]) / "tools/e2e_peers.py").replace("\\", "/")
    request = read(request_path)
    remote = request["out"] + "/request.json"
    ssh(box, [box["python"], tool, "--prepare", request["out"]], check=True)
    copy_file(box, request_path, remote)
    if box["os"] == "win32":
        args = ["pwsh", "-NoProfile", "-File", box["repo"] + "/tools/e2e/peer_box.ps1", "-Request", remote]
    else:
        args = ["/bin/zsh", box["repo"] + "/tools/e2e/peer_mac.sh", remote, box["repo"]]
    process = subprocess.Popen(["ssh", "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=20", box["ssh"], shlex.join(args)],
                               stdout=log, stderr=subprocess.STDOUT)
    return process, remote, tool


def selected_boxes(value, path):
    assignments = dict(part.split("=", 1) for part in value.split(","))
    if set(assignments) != {"host", "joiner"} or len(set(name.casefold() for name in assignments.values())) != 2:
        raise ValueError("name two different boxes for host and joiner")
    entries = {box["name"].casefold(): box for box in read(path)["boxes"]}
    result = {role: entries[name.casefold()] for role, name in assignments.items()}
    if result["host"]["os"] != "win32" or result["joiner"]["os"] != "darwin":
        raise ValueError("this scene launcher needs a Windows host and a Mac joiner")
    if result["host"]["hostname"].casefold() == result["joiner"]["hostname"].casefold():
        raise ValueError("both assignments name one engine host")
    return result


def collect_player(out, name, role, player):
    box, request, process, remote, tool, log = player
    process.wait()
    log.close()
    result = ssh(box, [box["python"], tool, "--pack", remote], check=True, capture_output=True, text=True)
    package = json.loads(result.stdout)
    archive = out / "commands" / (name + "-" + role + ".tar")
    copy_file(box, archive, package["path"], download=True)
    if video.file_evidence(archive).get("sha256") != package["sha256"]:
        raise ValueError("collected player files differ from their box")
    unpack(archive, out)
    archive.unlink()


def capture_scene(args):
    document = scene(args.scenario)
    check_scene(document)
    boxes = selected_boxes(args.peer_boxes, args.boxes_file)
    out, repo = args.out.resolve(), args.repo.resolve()
    source = video.source_evidence(repo)
    if source["dirty"]:
        raise ValueError("commit the scene launcher before playing")
    scene_path = Path(document["path"])
    relative = scene_path.relative_to(repo).as_posix()
    scene_hash = video.file_evidence(scene_path)["sha256"]
    out.mkdir(parents=True, exist_ok=False)
    controls = out / "commands"
    controls.mkdir()
    started = time.monotonic()
    capture = {"schema": 1, "scenario": document["name"], "scenario_definition": document, "scene_sha256": scene_hash,
               "source": source, "exe": {"exists": False, "reason": "driver only; engines build on their boxes"},
               "repo": str(repo), "out": str(out), "platform": sys.platform, "fps": video.DEFAULT_FPS,
               "size": document.get("size"), "settings": {}, "render_cap": 60, "started": video.stamp(),
               "command": [sys.executable, *sys.argv], "scratch_root": str(out.parent),
               "scratch_limit_bytes": video.SCRATCH_LIMIT, "topology": "spread", "proof": False, "runs": []}
    video.write_json(out / "capture.json", capture)
    for index, run in enumerate(scene_runs(document)):
        name = run.get("name", "run" + str(index))
        players, collected = {}, set()
        try:
            for role in ("host", "joiner"):
                box = boxes[role]
                remote_out = box["scratch"].rstrip("/") + "/" + out.name + "-" + name + "-" + uuid.uuid4().hex[:8]
                request = dict(box=box, role=role, run_index=index, out=remote_out, commit=source["tip"],
                               scene_relative=relative, scene_sha256=scene_hash, fps=video.DEFAULT_FPS,
                               sheet_every=15, port=int(document.get("port_base", video.PORT_LO)))
                request_path = controls / (name + "-" + role + ".json")
                video.write_json(request_path, request)
                log = (controls / (name + "-" + role + ".log")).open("w", encoding="utf-8")
                process, remote, tool = launch(box, request_path, log)
                players[role] = (box, request, process, remote, tool, log)
                if role == "host":
                    ready = ssh(box, [box["python"], tool, "--wait-start", remote], capture_output=True, text=True)
                    if ready.returncode:
                        raise RuntimeError("host launch refused: " + ready.stdout.strip())
            for role, player in players.items():
                collected.add(role)
                collect_player(out, name, role, player)
            halves = [move_paths(read(out / name / (role + "-capture.json")), players[role][1]["out"], out)
                      for role in ("host", "joiner")]
            captured = {**halves[0], "peers": [peer for half in halves for peer in half["peers"]],
                        "peer_boxes": {role: boxes[role]["name"] for role in boxes}, "topology": "spread",
                        "interrupted": next((half["interrupted"] for half in halves if half.get("interrupted")), None)}
            capture["runs"].append(captured)
            check = spread_check(capture)
            captured["proof"] = check["passed"]
            for peer in captured["peers"]:
                peer.update(topology="spread", proof=check["passed"])
            captured["repo"] = str(repo)
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
            video.write_json(out / "capture.json", capture)
        if capture.get("interrupted"):
            break
    check = spread_check(capture)
    video.write_json(out / "spread-check.json", check)
    video.scenario_manifest(capture, out, time.monotonic() - started)
    review = video.aggregate_review(capture, out)
    if capture.get("interrupted"):
        review["run_findings"].append({"class": "harness", "reason": capture["interrupted"]})
    if not check["passed"]:
        review["run_findings"].append({"class": "harness", "reason": "; ".join(check["reasons"])})
    for peer in review["peers"]:
        peer.update(box=boxes[peer["peer"]]["name"], engine_hostname=boxes[peer["peer"]]["hostname"])
    passed = check["passed"] and all(video.peer_completed(peer) for run in capture["runs"] for peer in run["peers"])
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
    parser.add_argument("--check-spread", type=Path)
    parser.add_argument("--play", type=Path)
    parser.add_argument("--pack", type=Path)
    parser.add_argument("--prepare", type=Path)
    parser.add_argument("--wait-start", type=Path)
    args = parser.parse_args()
    if args.prepare:
        args.prepare.mkdir(parents=True, exist_ok=False)
        return 0
    if args.wait_start:
        return wait_start(args.wait_start)
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
