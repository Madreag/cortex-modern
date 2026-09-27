"""Launch one engine through the runner and sample what the scheduler sees: the process's CPU mask and every
thread's base priority, from the first moment until exit (or --seconds).

    python tools/runner_thread_probe.py --repo <tree> --out <dir> [--seconds 40] -- <engine flags>

Writes <out>/probe.json and prints its summary line. The engine is stopped after --seconds when still running.
"""

from __future__ import annotations

import argparse
import ctypes as C
from ctypes import wintypes as W
import json
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_sim_test import make_run  # noqa: E402
import win32_test_runner as runner  # noqa: E402

K = C.WinDLL("kernel32", use_last_error=True)


class THREADENTRY32(C.Structure):
    _fields_ = [("dwSize", W.DWORD), ("cntUsage", W.DWORD), ("th32ThreadID", W.DWORD), ("th32OwnerProcessID", W.DWORD),
                ("tpBasePri", W.LONG), ("tpDeltaPri", W.LONG), ("dwFlags", W.DWORD)]


snapshot = runner.api(K, "CreateToolhelp32Snapshot", [W.DWORD, W.DWORD], W.HANDLE)
first_thread = runner.api(K, "Thread32First", [W.HANDLE, C.POINTER(THREADENTRY32)], W.BOOL)
next_thread = runner.api(K, "Thread32Next", [W.HANDLE, C.POINTER(THREADENTRY32)], W.BOOL)
process_affinity = runner.api(K, "GetProcessAffinityMask", [W.HANDLE, C.POINTER(C.c_size_t), C.POINTER(C.c_size_t)], W.BOOL)


def affinity_of(handle) -> int:
    mine, system = C.c_size_t(), C.c_size_t()
    if not process_affinity(handle, C.byref(mine), C.byref(system)):
        raise C.WinError(C.get_last_error())
    return mine.value


def thread_priorities(pid: int) -> dict[int, int]:
    """Base priority per thread id of one process."""
    handle = snapshot(0x4, 0)  # TH32CS_SNAPTHREAD
    if handle in (None, runner.INVALID_HANDLE):
        return {}
    found: dict[int, int] = {}
    try:
        entry = THREADENTRY32()
        entry.dwSize = C.sizeof(THREADENTRY32)
        more = first_thread(handle, C.byref(entry))
        while more:
            if entry.th32OwnerProcessID == pid:
                found[entry.th32ThreadID] = entry.tpBasePri
            more = next_thread(handle, C.byref(entry))
    finally:
        runner.close_handle(handle)
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=40.0)
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("flags", nargs=argparse.REMAINDER)
    options = parser.parse_args()
    flags = options.flags[1:] if options.flags[:1] == ["--"] else options.flags
    run = make_run(options.repo.resolve(), flags, options.out.resolve(), timeout=options.timeout)
    run.start()
    pid = run.record["pid"]
    masks: set[str] = set()
    top: dict[int, int] = {}
    samples = 0
    began = time.monotonic()
    stopped = False
    while run.poll() is None:
        if time.monotonic() - began >= options.seconds:
            run.terminate(137, "probe window ended")
            stopped = True
            break
        try:
            masks.add(f"0x{affinity_of(run.process):08x}")
        except OSError:
            pass
        for thread, priority in thread_priorities(pid).items():
            top[thread] = max(priority, top.get(thread, 0))
        samples += 1
        time.sleep(0.25)
    record = run.finish()
    histogram: dict[int, int] = {}
    for priority in top.values():
        histogram[priority] = histogram.get(priority, 0) + 1
    result = {"pid": pid, "flags": flags, "exe_sha256": record.get("exe_sha256"), "samples": samples,
              "stopped_by_probe": stopped, "exit_code": record.get("exit_code"),
              "launch_affinity_mask": record.get("affinity_mask"),
              "launch_engine_memory_limit_bytes": record.get("engine_memory_limit_bytes"),
              "process_affinity_seen": sorted(masks), "threads_seen": len(top),
              "max_thread_base_priority": max(top.values(), default=None),
              "threads_at_or_above_15": sum(p >= 15 for p in top.values()),
              "base_priority_histogram": {str(k): histogram[k] for k in sorted(histogram)}}
    (options.out / "probe.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({k: result[k] for k in ("pid", "process_affinity_seen", "threads_seen", "max_thread_base_priority",
                                             "threads_at_or_above_15", "base_priority_histogram", "exit_code")}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
