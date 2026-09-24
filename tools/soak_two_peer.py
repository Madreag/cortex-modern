"""The two-peer soak: one long service match with autosave on, forced holds that each end in a rejoin, and memory
sampled on both peers every minute.

    python tools/soak_two_peer.py --out <dir> [--minutes 45] [--autosave-seconds 60] [--holds 3] [--stall-ms 1500]
                                  [--port 49880] [--fullstate-every N] [--sample-seconds 60]

Both engines run through run_sim_test.make_run and the private-desktop runner with CCCP_HEADLESS=1. The client carries
the forced-hold lever (-net-test-live-stall TICK:MS) once per hold, spread evenly through the match, so its seat is held
and it has to rejoin while the host plays on. Exit 0 only when both peers exit 0, every shared tick's live hash agrees,
each forced hold was taken and ended in a completed private catch-up, no client named a new host while the host
lived, the host published the autosaves its cadence owes, and a memory sample exists for every minute. result.json carries the counts; memory.jsonl the samples.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_sim_test import make_run  # noqa: E402
from compare_sim_traces import compare_fullstate  # noqa: E402
from feel.retained_resume import compare_live_hashes  # noqa: E402
from feel_measure import input_pattern, private_settings, stage_baseline  # noqa: E402

PORT_LO, PORT_HI = 49880, 49889
TICKS_PER_SECOND = 60


class MemoryCounters(ctypes.Structure):
    _fields_ = [("cb", ctypes.c_ulong), ("PageFaultCount", ctypes.c_ulong), ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t), ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t), ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t), ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t)]


def process_memory(run) -> dict | None:
    """The engine's resident and private bytes, read from its own process; None once it has exited."""
    if sys.platform == "win32":
        handle = getattr(run, "process", None)
        if not handle:
            return None
        counters = MemoryCounters()
        counters.cb = ctypes.sizeof(counters)
        if not ctypes.windll.psapi.GetProcessMemoryInfo(ctypes.c_void_p(handle), ctypes.byref(counters), counters.cb):
            return None
        return {"working_set": counters.WorkingSetSize, "private": counters.PagefileUsage,
                "peak_working_set": counters.PeakWorkingSetSize}
    pid = (getattr(run, "record", {}) or {}).get("pid")
    if not pid:
        return None
    result = subprocess.run(["ps", "-o", "rss=,vsz=", "-p", str(pid)], capture_output=True, text=True)
    fields = result.stdout.split()
    return {"working_set": int(fields[0]) * 1024, "virtual": int(fields[1]) * 1024} if len(fields) == 2 else None


def count(path: Path, needle: str, also: str = "") -> int:
    """Lines that start with `needle` (and carry `also`): one per event, never the event's detail lines."""
    text = path.read_text(encoding="utf-8", errors="replace") if path.is_file() else ""
    return sum(line.startswith(needle) and also in line for line in text.splitlines())


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--minutes", type=float, default=45)
    parser.add_argument("--autosave-seconds", type=int, default=60)
    parser.add_argument("--holds", type=int, default=3)
    parser.add_argument("--stall-ms", type=int, default=1500)
    parser.add_argument("--port", type=int, default=PORT_LO)
    parser.add_argument("--fullstate-every", type=int, default=0)
    parser.add_argument("--sample-seconds", type=float, default=60)
    options = parser.parse_args(argv)
    if not PORT_LO <= options.port <= PORT_HI:
        parser.error(f"this driver owns ports {PORT_LO}-{PORT_HI}")
    if options.minutes <= 0 or options.holds < 0 or not 0 < options.stall_ms <= 20000 or options.fullstate_every < 0:
        parser.error("--minutes > 0, --holds >= 0, --stall-ms in 1..20000, --fullstate-every >= 0")
    if not 60 <= options.autosave_seconds <= 3600:
        parser.error("--autosave-seconds stays in the product's 60..3600 range")
    if os.environ.get("CCCP_HEADLESS", "1") != "1":
        parser.error("CCCP_HEADLESS must stay 1")
    os.environ["CCCP_HEADLESS"] = "1"
    repo, root = options.repo.resolve(), options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    ticks = int(options.minutes * 60 * TICKS_PER_SECOND)
    stalls = [ticks * (index + 1) // (options.holds + 1) for index in range(options.holds)]
    script = root / "input.txt"
    input_pattern(script)
    plan = {"started": time.strftime("%Y-%m-%d %H:%M:%S"), "repo": str(repo), "ticks": ticks, "minutes": options.minutes,
            "autosave_seconds": options.autosave_seconds, "stalls": [f"{tick}:{options.stall_ms}" for tick in stalls],
            "port": options.port, "fullstate_every": options.fullstate_every, "sample_seconds": options.sample_seconds}
    (root / "plan.json").write_text(json.dumps(plan, indent=2) + "\n", encoding="utf-8")
    runs, records = {}, {}
    samples: list[dict] = []
    stop = threading.Event()
    started = time.monotonic()

    def sample() -> None:
        with open(root / "memory.jsonl", "a", encoding="utf-8") as handle:
            while True:
                row = {"elapsed_s": round(time.monotonic() - started, 1),
                       **{peer: process_memory(run) for peer, run in runs.items()}}
                samples.append(row)
                handle.write(json.dumps(row) + "\n")
                handle.flush()
                if stop.wait(options.sample_seconds):
                    return

    try:
        for peer in ("host", "client"):
            flags = ["-seed", "42", "-max-ticks", str(ticks), "-tick-hashes", "-out", str(root / f"{peer}_trace.json"),
                     "-net-live-tick-hashes", str(root / f"{peer}-live.jsonl"), "-input-script", str(script),
                     "-net-match-service-e2e", "-net-port", str(options.port), "-net-match-ticks", str(ticks),
                     "-net-match-humans", "2", "-net-match-peers", "2", "-net-match-cpu-slots", "0",
                     "-net-match-service-preset", "Determinism FeelBaseline", "-net-match-service-module", "UserScenes.rte",
                     "-net-match-auto-delay", "-net-reconnect-ticket", str(root / f"{peer}.ticket"),
                     "-net-match-report", str(root / f"{peer}_report.json")]
            if options.fullstate_every:
                flags += ["-net-fullstate-hash-every", str(options.fullstate_every)]
            if peer == "host":
                flags += ["-net-host", "-net-autosave-seconds", str(options.autosave_seconds)]
            else:
                flags += ["-net-join", "127.0.0.1"]
                for tick in stalls:
                    flags += ["-net-test-live-stall", f"{tick}:{options.stall_ms}"]
            run = make_run(repo, flags, root / peer, timeout=options.minutes * 60 + 600, env={"CCCP_HEADLESS": "1"})
            runs[peer] = run
            private_settings(run, 60)
            stage_baseline(run, ticks, 2)
            run.start()
            if peer == "host":
                time.sleep(.75)
        sampler = threading.Thread(target=sample, daemon=True)
        sampler.start()
        finishers = {peer: threading.Thread(target=lambda p=peer: records.__setitem__(p, runs[p].finish())) for peer in runs}
        for thread in finishers.values():
            thread.start()
        for thread in finishers.values():
            thread.join()
    finally:
        stop.set()
        for run in runs.values():
            run.close()
        for peer, run in runs.items():
            records.setdefault(peer, run.record)
    elapsed = time.monotonic() - started
    live = compare_live_hashes(root / "host-live.jsonl", root / "client-live.jsonl", 1)
    hashes_equal = bool(live) and all(row["compared_ticks"] > 0 and row["mismatched_ticks"] == 0
                                      and row["mismatched_applied_input_ticks"] == 0 for row in live)
    fullstate = compare_fullstate(root / "host" / "stdout.log", root / "client" / "stdout.log") if options.fullstate_every else None
    holds = count(root / "host" / "stdout.log", "[net-match] hold peer=")
    rejoins = count(root / "client" / "stdout.log", "[net-match] private catch-up complete")
    # The host is never killed here, so a client that names a new host has split the match in two.
    split = count(root / "client" / "stdout.log", "[net-match] Host left - ")
    autosaves = count(root / "host" / "stdout.log", "[autosave] tick=", "capture_ms=")
    # The cadence is in simulation seconds, so what is owed follows the ticks the host reached, not the wall clock.
    reached = max((row["last_tick"] for row in live), default=0)
    owed = int(reached // (TICKS_PER_SECOND * options.autosave_seconds)) - 1
    minutes_sampled = sum(1 for row in samples if row.get("host") and row.get("client"))
    exits = {peer: {"exit_code": record.get("exit_code"), "timed_out": record.get("timed_out")} for peer, record in records.items()}
    checks = {"exits": all(row["exit_code"] == 0 and not row["timed_out"] for row in exits.values()),
              "hashes_equal": hashes_equal, "fullstate": fullstate is None or bool(fullstate.get("passed")),
              "holds": holds >= options.holds, "rejoins": rejoins >= options.holds, "no_split_brain": split == 0,
              "autosaves": autosaves >= max(0, owed), "memory_sampled": minutes_sampled >= int(options.minutes)}
    first = next((row for row in samples if row.get("host") and row.get("client")), None)
    last = next((row for row in reversed(samples) if row.get("host") and row.get("client")), None)
    growth = {peer: {"first": first[peer]["working_set"], "last": last[peer]["working_set"],
                     "peak": max(row[peer]["working_set"] for row in samples if row.get(peer))}
              for peer in ("host", "client")} if first and last else None
    result = {"pass": all(checks.values()), "checks": checks, "exits": exits, "elapsed_s": round(elapsed, 1),
              "ticks_reached": reached, "holds_taken": holds, "rejoins_completed": rejoins, "client_named_a_new_host": split,
              "autosaves_published": autosaves, "autosaves_owed": owed,
              "live_hashes": live, "fullstate": fullstate, "memory_samples": len(samples), "memory_working_set": growth,
              "plan": plan}
    (root / "result.json").write_text(json.dumps(result, indent=2, default=str) + "\n", encoding="utf-8")
    print(f"[soak] {'PASS' if result['pass'] else 'FAIL'} {json.dumps(checks)} holds={holds} rejoins={rejoins} "
          f"autosaves={autosaves}/{owed} samples={len(samples)} -> {root / 'result.json'}", flush=True)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
