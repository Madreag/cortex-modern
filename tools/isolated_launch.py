"""Launch the game through the isolated runner on behalf of a harness that would otherwise start it directly.

    python isolated_launch.py --out <record dir> --cwd <runtime dir> [--timeout S] [--stdout <file>] -- "<exe>" <args...>
    python isolated_launch.py --hold

The PowerShell harnesses (run_interp_e2e.ps1 and the desync scripts) used to create the engine process themselves,
which put its window, its GL context and its foreground activation on the user's desktop. This wrapper gives them
the same launch every Python harness gets from win32_test_runner.IsolatedRun: a private desktop the user never
sees, SW_HIDE, a job object that kills the engine when this process dies, the fullscreen hold and the job memory
cap. The engine's combined stdout+stderr goes to <out>/stdout.log; at exit it is copied to --stdout when given and
echoed on this process's stdout, so a harness that captured the engine's output captures the same text. The exit
code is the engine's (124 on the runner's timeout), passed through ExitProcess so a 32-bit code survives.

--hold blocks while the user is in a fullscreen application and exits 0, for a harness whose own deadline would
otherwise start counting before the runner's hold releases the launch.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import sys
from pathlib import Path

if __package__:
    from .win32_test_runner import IsolatedRun, wait_while_user_fullscreen
else:
    from win32_test_runner import IsolatedRun, wait_while_user_fullscreen


def hold() -> int:
    record: dict = {}
    wait_while_user_fullscreen(record, lambda: None)
    print(json.dumps(record or {"waited_for_user_fullscreen_seconds": 0}))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--out", type=Path, help="directory for launch.json and stdout.log"
    )
    parser.add_argument(
        "--cwd",
        type=Path,
        help="the engine's working directory (a prepared private runtime)",
    )
    parser.add_argument("--timeout", type=float, default=900.0)
    parser.add_argument(
        "--stdout",
        type=Path,
        help="also copy the engine's combined output to this file",
    )
    parser.add_argument(
        "--hold",
        action="store_true",
        help="wait while the user is in a fullscreen application, then exit",
    )
    parser.add_argument("argv", nargs=argparse.REMAINDER)
    options = parser.parse_args()
    if options.hold:
        return hold()
    argv = options.argv[1:] if options.argv[:1] == ["--"] else options.argv
    if not options.out or not options.cwd or not argv:
        parser.error(
            "--out, --cwd and the executable with its explicit test arguments after -- are required"
        )
    run = IsolatedRun(argv, options.cwd, options.out, options.timeout)
    try:
        record = run.start().finish()
    finally:
        run.close()
    log = options.out / "stdout.log"
    text = log.read_bytes() if log.exists() else b""
    if options.stdout:
        options.stdout.parent.mkdir(parents=True, exist_ok=True)
        options.stdout.write_bytes(text)
    sys.stdout.buffer.write(text)
    sys.stdout.flush()
    code = int(record.get("exit_code") or 0) & 0xFFFFFFFF
    if sys.platform == "win32":
        ctypes.windll.kernel32.ExitProcess(ctypes.c_uint32(code))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
