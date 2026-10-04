"""Verify retained world-image runs and publish the size table and open gates."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re

from acceptance_rows import judge
from acceptance_runtime import write_json
from world_image_sizes import image_measurement, markdown, offered_archive, offered_scenes


def load(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def scene_list(path):
    document = load(path)
    scenes = offered_scenes(load(document["source"]))
    if scenes != document["scenes"]:
        raise ValueError("scene list differs from its native lobby dump")
    digest = document.get("executable_sha256")
    if document.get("configuration") != "Final" or not re.fullmatch(r"[0-9a-f]{64}", str(digest)):
        raise ValueError("scene list lacks its Final executable identity")
    return scenes, digest


def read_run(root, executable_sha256):
    root = Path(root)
    records = {peer: load(root/peer/"launch.json") for peer in ("host", "bootstrap", "client")}
    if any(record.get("exe_sha256") != executable_sha256 for record in records.values()):
        raise ValueError("scene peers and native lobby list ran different release binaries")
    if any(record.get("runner") != "win32_test_runner.py" or record.get("headless_env") != "1"
           or not record.get("private_desktop") for record in records.values()):
        raise ValueError("a scene peer lacks its private headless runner receipt")
    native = load(root/"host-report.json")
    config = native.get("service", {}).get("runner", {}).get("match_config", {})
    if (config.get("activity_preset") != "Persistent World" or config.get("persistent_world") is not True
            or config.get("rules", {}).get("activity_module") != "Base.rte"):
        raise ValueError("host did not adopt the native persistent-world activity")
    scene = dict(name=config.get("scene_name"), module=config.get("rules", {}).get("scene_module"))
    if not all(isinstance(value, str) and value for value in scene.values()):
        raise ValueError("native world configuration has no scene identity")
    host_log = (root/"host/stdout.log").read_text(encoding="utf-8", errors="replace")
    client_log = (root/"client/stdout.log").read_text(encoding="utf-8", errors="replace")
    archive, digest, tick = offered_archive(host_log, root/"host/runtime")
    row = image_measurement(scene, archive, client_log, digest)
    row.update(capture_tick=tick, capture_count=1, executable_sha256=executable_sha256,
               engine_run_passed=all(r.get("exit_code") == 0 and r.get("timed_out") is False for r in records.values()),
               source=str(root), host_log=str(root/"host/stdout.log"), client_log=str(root/"client/stdout.log"))
    # This file is reserved for native, clocked transfer/label receipts. Its absence stays RED.
    transfer_path = root/"client/acceptance-transfer.json"
    row["transfer"] = load(transfer_path) if transfer_path.is_file() else {}
    if row["transfer"] and row["transfer"].get("received_bytes") != row["received_bytes"]:
        raise ValueError("clocked transfer receipt differs from the native StateChunk byte total")
    return row


def report(list_path, run_roots, out):
    scenes, executable_sha256 = scene_list(list_path)
    measurements = [read_run(root, executable_sha256) for root in run_roots]
    table, complete = markdown(scenes, measurements)
    facts = dict(build=dict(configuration="Final", sanitizer=False, executable_sha256=executable_sha256,
                            evidence="task-designated engineer Final release build; every runner digest matches the native lobby-list run"),
                 offered_scenes=[row["module"]+"/"+row["name"] for row in scenes],
                 scenes=[{**row, "name":row["module"]+"/"+row["name"]} for row in measurements])
    result = judge("image-sizes", facts)
    result.update(byte_table_complete=complete, scenes_measured=len(measurements), scenes_offered=len(scenes),
                  completed_engine_runs=sum(row["engine_run_passed"] for row in measurements))
    table += ("\nVerified receipt index\n\n"
              f"Executable SHA-256: `{executable_sha256}`.\n\n"
              f"Native lobby list: `{Path(list_path)}`. Each scene identity below was also checked against the host's adopted match configuration.\n\n"
              "Received bytes are the reassembled StateChunk payload total printed by the receiver; they are not a packet-capture count and exclude transport retransmissions and headers. A larger stream/archive ratio means there is no measured byte reduction.\n\n"
              "| Scene | Capture tick | Archive SHA-256 | Retained run |\n|---|---:|---|---|\n")
    for row in measurements:
        name = row["name"].replace("|", "\\|")
        table += f"| {name} | {row['capture_tick']} | `{row['archive_sha256']}` | `{row['source']}` |\n"
    table += (f"\nVerified measurements: {len(measurements)}/{len(scenes)}. "
              f"Engine runs completed successfully: {result['completed_engine_runs']}/{len(measurements)}. "
              f"Full R4/R6 receipt gate: {'PASS' if result['passed'] else 'FAIL'}; see image-sizes-result.json for every missing requirement.\n")
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    write_json(out/"image-sizes-facts.json", facts)
    write_json(out/"image-sizes-result.json", result)
    (out/"image-sizes.md").write_text(table, encoding="utf-8")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scenes", type=Path, required=True)
    parser.add_argument("--run", type=Path, action="append", default=[])
    parser.add_argument("--batch", type=Path, action="append", default=[])
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    roots = args.run[:]
    for batch in args.batch:
        # Only completed measurements are eligible; the running scene remains absent.
        roots.extend(path.parent for path in sorted(batch.glob("scene-*/measurement.json")))
    result = report(args.scenes, roots, args.out)
    print(f"image sizes: {result['scenes_measured']}/{result['scenes_offered']} measured; "
          f"byte table {'complete' if result['byte_table_complete'] else 'incomplete'}; "
          f"full receipt gate {'PASS' if result['passed'] else 'FAIL'}")
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
