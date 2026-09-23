"""A writer killed mid-archive leaves `<name>.ccsave.tmp.<pid>` behind. The next writer in that directory
removes it, and never touches a temporary whose writer is still alive.

The run is a two-peer match that autosaves every second, as test_autosave_restore.py's arms do. Before either
peer starts, two temporaries are planted in its Autosaves directory: one named for a process id no process
holds, one named for this driver's own id, which lives for the whole run.

RED before the change: nothing sweeps the dead writer's temporary, so it is still there after the run.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys
import threading

from run_sim_test import make_run


def process_alive(pid: int) -> bool:
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


def dead_pid() -> int:
    for candidate in range(4_000_000, 4_100_000, 4):
        if not process_alive(candidate):
            return candidate
    raise RuntimeError("no free process id to name the dead writer")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=49630)
    options = parser.parse_args()
    os.environ["CCCP_HEADLESS"] = "1"
    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    ticks = 180
    dead = dead_pid()
    runs, records, planted = {}, {}, {}
    for who in ("host", "client"):
        args = ["-net-match-service-e2e", "-net-port", str(options.port), "-net-match-peers", "2",
                "-net-match-ticks", str(ticks), "-net-match-input-delay", "3", "-net-autosave-seconds", "1",
                "-max-ticks", str(ticks), "-net-match-report", str(root / f"{who}_report.json")]
        args += ["-net-host"] if who == "host" else ["-net-join", "127.0.0.1"]
        runs[who] = make_run(options.repo, args, root / who, 300, env={"CCCP_HEADLESS": "1"})
        autosaves = Path(runs[who].cwd) / "Autosaves"
        autosaves.mkdir(parents=True, exist_ok=True)
        stale = autosaves / f"00000000-0000-0000-0000-000000000000-7.ccsave.tmp.{dead}"
        live = autosaves / f"00000000-0000-0000-0000-000000000000-8.ccsave.tmp.{os.getpid()}"
        stale.write_bytes(b"a writer died here")
        live.write_bytes(b"a writer is still at work here")
        planted[who] = (autosaves, stale, live)

    def drive(who: str) -> None:
        try:
            records[who] = runs[who].start().finish()
        except Exception as error:
            records[who] = {"error": repr(error)}

    threads = [threading.Thread(target=drive, args=(who,)) for who in runs]
    try:
        threads[0].start()
        threading.Event().wait(1)
        threads[1].start()
        for thread in threads:
            thread.join()
    finally:
        for run in runs.values():
            run.close()
    checks, archives = {}, {}
    for who, (autosaves, stale, live) in planted.items():
        archives[who] = sorted(path.name for path in autosaves.glob("*.ccsave"))
        checks[f"{who}_archived"] = bool(archives[who])
        checks[f"{who}_dead_writer_swept"] = not stale.exists()
        checks[f"{who}_live_writer_kept"] = live.exists()
    passed = all(checks.values())
    line = (f"{'PASS' if passed else 'FAIL'} autosave temporaries: dead_pid={dead} "
            + " ".join(f"{name}={value}" for name, value in checks.items()))
    result = {"passed": passed, "checks": checks, "archives": archives, "dead_pid": dead, "live_pid": os.getpid(),
              "port": options.port, "records": records, "final_line": line}
    (root / "result.json").write_text(json.dumps(result, indent=2, default=str) + "\n", encoding="utf-8")
    print(line)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
