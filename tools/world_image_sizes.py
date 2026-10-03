"""Read the persistent-world scene list and measure archive and StateChunk bytes separately."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import time

from acceptance_runtime import check_storage, local_reservation, private_run, write_json
from run_sim_test import engine_executable, file_sha256


def offered_scenes(document):
    activities = [row for row in document.get("activity_table", [])
                  if row.get("preset") == "Persistent World" and row.get("module") == "Base.rte"]
    if len(activities) != 1 or not activities[0].get("scenes"):
        raise ValueError("native lobby dump has no unique Persistent World scene list")
    scenes = activities[0]["scenes"]
    if any(set(row) != {"name", "module"} or not row["name"] or not row["module"] for row in scenes):
        raise ValueError("native scene identity is incomplete")
    if len({(r["module"], r["name"]) for r in scenes}) != len(scenes):
        raise ValueError("native world scene list repeats a scene")
    return scenes


def enumerate_scenes(repo, out, timeout=180):
    out.mkdir(parents=True, exist_ok=False)
    script = out / "scenes.menu.txt"
    script.write_text("wait 40\nactivate ButtonMainToMultiplayer\nwait 12\nactivate ButtonMultiplayerHostGame\n"
                      "wait 12\ndump_host_options\nwait 30\nexit\n", encoding="utf-8")
    with local_reservation(out):
        run = private_run(repo, ["-menu-script", str(script)], out/"engine", timeout)
        try:
            record = run.start().finish()
        finally:
            run.close()
    if record.get("exit_code") != 0:
        raise RuntimeError("world scene-list engine failed; see runner evidence")
    dumps = sorted((out/"engine/runtime/ScreenShots").glob("dump_host_options_*.json"))
    if len(dumps) != 1:
        raise ValueError("scene-list run did not emit exactly one native lobby dump")
    scenes = offered_scenes(json.loads(dumps[0].read_text(encoding="utf-8-sig")))
    result = dict(scenes=scenes, source=str(dumps[0]), executable_sha256=record.get("exe_sha256"),
                  configuration="Final", configuration_evidence="task-designated engineer Final release build")
    write_json(out/"scenes.json", result)
    return result


def transfer_totals(log):
    return [int(value) for value in re.findall(r"(?m)^\[net-match\] state transfer complete: (\d+) bytes\s*$", log)]


def image_measurement(scene, archive, client_log, expected_digest=None):
    archive = Path(archive)
    totals = transfer_totals(client_log)
    if len(totals) != 1 or totals[0] <= 0:
        raise ValueError("join did not receive exactly one identifiable StateChunk image")
    size = archive.stat().st_size
    digest = file_sha256(archive)
    if size <= 0 or expected_digest and expected_digest != digest:
        raise ValueError("captured archive is empty or differs from the offered image")
    return dict(**scene, archive=str(archive), archive_bytes=size, archive_sha256=digest,
                received_bytes=totals[0], ratio=totals[0]/size, compresses=totals[0] < size)


def markdown(scenes, measurements):
    identities = [(r["module"], r["name"]) for r in scenes]
    indexed = {(r["module"], r["name"]): r for r in measurements}
    if len(indexed) != len(measurements) or set(indexed)-set(identities):
        raise ValueError("duplicate or unoffered scene measurement")
    lines = ["# Persistent-world image sizes", "",
             "Method: the native lobby's Persistent World activity table names the scenes. Each measurement uses the task-designated Final release build, one retained checkpoint archive, its SHA-256, and the joiner's native StateChunk completion receipt. Archive size and received size are measured independently. No sanitizer run or inferred stream size is substituted.", "",
             "| World scene | Module | Archive bytes | Received StateChunk bytes | Stream / archive | Compresses |", "|---|---|---:|---:|---:|---|"]
    for identity in identities:
        row = indexed.get(identity)
        name = identity[1].replace("|", "\\|")
        if row is None:
            lines.append(f"| {name} | {identity[0]} | NOT RUN | NOT RUN | — | UNPROVEN |")
        else:
            lines.append(f"| {name} | {identity[0]} | {row['archive_bytes']} | {row['received_bytes']} | {row['ratio']:.6f} | {'yes' if row['compresses'] else 'no'} |")
    complete = len(indexed) == len(identities) and bool(identities)
    lines += ["", "Complete: "+("yes" if complete else "no"), "",
              "Transfer label dumps are retained separately at five-second intervals; missing dumps remain an open progress-line evidence requirement.", ""]
    return "\n".join(lines), complete


def collect_scene(repo, scene, out, port, scratch, timeout=600):
    out.mkdir(parents=True, exist_ok=False)
    common = ["-net-match-service-e2e", "-net-port", str(port), "-net-match-peers", "2",
              "-net-match-service-preset", "Persistent World", "-net-match-service-module", "Base.rte",
              "-net-match-service-scene", scene["name"], "-net-match-service-scene-module", scene["module"],
              "-net-match-auto-delay", "-net-autosave-seconds", "0", "-net-match-ticks", "2400", "-max-ticks", "2400"]
    result, handles = dict(scene=scene, passed=False), []
    with local_reservation(out):
        try:
            host = private_run(repo, common+["-net-host", "-net-persistent-world", "-net-world-fresh",
                                           "-net-match-report", str(out/"host-report.json")],
                               out/"host", timeout, {"CC_TEST_WORLD_JOIN_FIRST_IMAGE": "1"})
            handles.append(host)
            host.start()
            deadline = time.monotonic()+timeout
            while time.monotonic() < deadline:
                text = (out/"host/stdout.log").read_text(encoding="utf-8", errors="replace")
                if "[net-lockstep] start round=" in text:
                    break
                if host.poll() is not None:
                    raise RuntimeError("world host ended before its first round")
                check_storage(scratch)
                time.sleep(.2)
            else:
                raise RuntimeError("world host did not start before hang guard")
            client = private_run(repo, common+["-net-join", "127.0.0.1", "-net-match-report", str(out/"client-report.json")], out/"client", timeout)
            handles.append(client)
            client.start()
            result["client_launch_monotonic"] = time.monotonic()
            # A single receipt identifies this join's archive; a later repair is a separate, failing transfer.
            while time.monotonic() < deadline:
                check_storage(scratch)
                if all(handle.poll() is not None for handle in handles):
                    break
                time.sleep(.2)
            records = [handle.finish() for handle in handles]
            result["records"] = records
            if any(r.get("exit_code") != 0 for r in records):
                raise RuntimeError("scene measurement engine failed")
            log = (out/"client/stdout.log").read_text(encoding="utf-8", errors="replace")
            archives = list((out/"client/runtime/Userdata/UserSavedGames.rte").glob("p5join_recv_*.ccsave"))
            if len(archives) != 1:
                raise ValueError("join did not retain a unique checkpoint archive")
            result.update(image_measurement(scene, archives[0], log), passed=True)
        finally:
            for handle in handles:
                handle.close()
            write_json(out/"measurement.json", result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    listing = sub.add_parser("list")
    listing.add_argument("--repo", type=Path, required=True)
    listing.add_argument("--out", type=Path, required=True)
    measure = sub.add_parser("measure")
    measure.add_argument("--repo", type=Path, required=True)
    measure.add_argument("--scenes", type=Path, required=True)
    measure.add_argument("--out", type=Path, required=True)
    measure.add_argument("--scratch", type=Path, required=True)
    measure.add_argument("--port", type=int, required=True)
    measure.add_argument("--scene-index", type=int, action="append", help="measure these lobby-list indices; the table still lists every scene")
    args = parser.parse_args()
    if args.command == "list":
        result = enumerate_scenes(args.repo.resolve(), args.out.resolve())
        print(json.dumps(result, indent=2))
        return 0
    if not 1024 <= args.port < 50000:
        parser.error("choose a free Windows port below 50000")
    scenes = json.loads(args.scenes.read_text(encoding="utf-8-sig"))["scenes"]
    args.out.mkdir(parents=True, exist_ok=False)
    measurements = []
    for index, scene in enumerate(scenes):
        if args.scene_index is not None and index not in args.scene_index:
            continue
        check_storage(args.scratch)
        measurements.append(collect_scene(args.repo.resolve(), scene, args.out/f"scene-{index:02d}", args.port, args.scratch))
        table, complete = markdown(scenes, measurements)
        (args.out/"image-sizes.md").write_text(table, encoding="utf-8")
    return 0 if complete else 1


if __name__ == "__main__":
    raise SystemExit(main())
