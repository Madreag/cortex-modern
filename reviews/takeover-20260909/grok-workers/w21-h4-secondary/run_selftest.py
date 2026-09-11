import json
import subprocess
import sys
from pathlib import Path

EXE = r"D:\Projects\h4-secondary\Cortex Command.exe"
RUNNER = r"D:\Projects\h4-secondary\tools\win32_test_runner.py"
CWD = r"D:\Projects\h4-secondary"


def main() -> int:
    name = sys.argv[1]
    timeout = int(sys.argv[2])
    extra_env = {}
    args = sys.argv[3:]
    if args and args[0] == "--env" and len(args) >= 2:
        for pair in args[1].split(";"):
            if "=" in pair:
                key, value = pair.split("=", 1)
                extra_env[key] = value
        args = args[2:]
    out = Path(r"D:\mx\w21") / name
    out.mkdir(parents=True, exist_ok=True)
    cmd = [
        sys.executable,
        RUNNER,
        "--cwd",
        CWD,
        "--out",
        str(out),
        "--timeout",
        str(timeout),
        "--",
        EXE,
        *args,
    ]
    env_note = out / "command.txt"
    env_note.write_text(" ".join(cmd) + "\n", encoding="utf-8")
    print("RUN", " ".join(cmd), flush=True)
    completed = subprocess.run(cmd, check=False)
    (out / "runner_exit.txt").write_text(str(completed.returncode) + "\n", encoding="utf-8")
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
