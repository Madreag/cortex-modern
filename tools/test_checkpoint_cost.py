"""Measure checkpoint stalls in a two-peer match and a persistent world."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re

from test_autosave_restore import CAPTURE, _run_world_round, peer_log, run_pair
from run_sim_test import engine_executable, file_sha256  # noqa: E402


BUDGET_MS = 5.0
SPLIT = re.compile(r"^\[autosave\] tick=(\d+) layers_us=(\d+) activity_us=(\d+) graph_us=(\d+) scene_us=(\d+) structure_us=(\d+) scene_runtime_us=(\d+) globals_us=(\d+)$", re.M)
WORKER = re.compile(r"^\[autosave\] tick=(\d+) freeze_us=(\d+) worker_us=(\d+) image_bytes=(\d+) .*", re.M)
DETAIL = re.compile(r"^\[autosave-split\] tick=(\d+) serialize_scene_ms=([0-9.]+) serialize_mos_ms=([0-9.]+) lua_graph_ms=([0-9.]+) compress_write_ms=([0-9.]+) freeze_ms=([0-9.]+) bytes=(\d+)$", re.M)
# The sim thread's wait at a Lua state's gate for page copies still landing, reported at the next tick's first Lua work.
GATE = re.compile(r"^\[autosave-gate\] tick=(\d+) waited_us=(\d+)$", re.M)


def measure(root: Path, records: dict) -> dict:
    peers = {}
    for who in ("host", "client"):
        log = peer_log(root, who)
        captures = [{"tick": int(tick), "capture_ms": float(ms), "bytes": int(size)}
                    for tick, ms, size in CAPTURE.findall(log)]
        splits = {int(row[0]): dict(zip(("layers_ms", "activity_ms", "lua_graph_ms", "scene_and_mos_ms",
                                        "structure_ms", "scene_runtime_ms", "globals_ms"),
                                       (int(value) / 1000 for value in row[1:]))) for row in SPLIT.findall(log)}
        workers = {int(tick): {"freeze_ms": int(freeze) / 1000, "writer_ms": int(worker) / 1000}
                   for tick, freeze, worker, _ in WORKER.findall(log)}
        details = {int(row[0]): dict(zip(("serialize_scene_ms", "serialize_mos_ms", "lua_graph_ms", "compress_write_ms", "freeze_ms"),
                                        map(float, row[1:6]))) for row in DETAIL.findall(log)}
        for capture in captures:
            capture.update(splits.get(capture["tick"], {}))
            capture.update(workers.get(capture["tick"], {}))
            capture.update(details.get(capture["tick"], {}))
            capture["gate_wait_ms"] = 0.0
        # A gate wait belongs to the last capture before it: its copies are the only ones a gate can wait for.
        for tick, waited in GATE.findall(log):
            owner = max((row for row in captures if row["tick"] < int(tick)), key=lambda row: row["tick"], default=None)
            if owner is not None:
                owner["gate_wait_ms"] += int(waited) / 1000
        for capture in captures:
            capture["share_ms"] = capture["capture_ms"] + capture["gate_wait_ms"]
        record = records.get(who, {})
        reached = 0
        trace = root / f"{who}_trace.json"
        if trace.exists():
            data = json.loads(trace.read_text(encoding="utf-8"))
            reached = max((int(row["tick"]) for run in data.get("runs", [])
                           for row in run.get("tick_hashes", [])), default=0)
        peers[who] = {"record": record, "captures": captures, "last_trace_tick": reached,
                      "max_capture_ms": max((row["capture_ms"] for row in captures), default=None),
                      "max_share_ms": max((row["share_ms"] for row in captures), default=None)}
    return peers


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--arm", choices=("match", "world", "all"), default="all")
    parser.add_argument("--port", type=int, default=49520)
    parser.add_argument("--score-existing", action="store_true")
    parser.add_argument("--lua-states", type=int, help="override the engine's default Lua state count on both peers")
    parser.add_argument("--objects", type=int, default=0, help="pinned scripted objects the match arm's scene starts with")
    parser.add_argument("--trace", type=int, default=0, help="CCCP_CHECKPOINT_TRACE for both peers: each capture's spans, nested writes to this depth")
    options = parser.parse_args()
    os.environ["CCCP_HEADLESS"] = "1"
    os.environ["CCCP_CHECKPOINT_SPLIT"] = "1"
    if options.trace:
        os.environ["CCCP_CHECKPOINT_TRACE"] = str(options.trace)
    root = options.out.resolve()
    if not options.score_existing:
        root.mkdir(parents=True, exist_ok=False)
    with engine_executable(options.repo).open("rb") as stream:
        digest = file_sha256(stream)
    result = {"exe_sha256": digest, "budget_ms": BUDGET_MS, "ticks": 600, "lua_states": options.lua_states, "objects": options.objects, "arms": {}}
    extra = {who: ["-num-lua-states", str(options.lua_states)] for who in ("host", "client")} if options.lua_states else {}
    for index, arm in enumerate(("match", "world") if options.arm == "all" else (options.arm,)):
        directory = root / arm
        try:
            if options.score_existing:
                records = {who: json.loads((directory / who / "launch.json").read_text(encoding="utf-8"))
                           for who in ("host", "client")}
                result["exe_sha256"] = records["host"].get("exe_sha256")
            elif arm == "match":
                records = run_pair(options.repo, directory, options.port + 2 * index, 600, 1, extra, load_objects=options.objects)
            else:
                directory.mkdir()
                records = _run_world_round(options.repo, directory, options.port + 2 * index, 600, extra)
            peers = measure(directory, records)
            passed = all(peer["record"].get("exit_code") == 0 and not peer["record"].get("timed_out")
                         and peer["captures"] and peer["last_trace_tick"] >= 600
                         and peer["max_share_ms"] < BUDGET_MS for peer in peers.values())
            result["arms"][arm] = {"passed": passed, "peers": peers}
            costs = ", ".join(f"{who}={peer['max_capture_ms']} ms share={peer['max_share_ms']} ms tick={peer['last_trace_tick']}" for who, peer in peers.items())
            line = f"{'PASS' if passed else 'FAIL'} {arm}: budget < {BUDGET_MS} ms; {costs}"
        except Exception as error:
            result["arms"][arm] = {"passed": False, "error": repr(error)}
            line = f"FAIL {arm}: {error}"
        result["arms"][arm]["final_line"] = line
        print(line, flush=True)
        (root / ("result-rescored.json" if options.score_existing else "result.json")).write_text(json.dumps(result, indent=2, default=str) + "\n", encoding="utf-8")
    return 0 if all(arm["passed"] for arm in result["arms"].values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
