import json
import subprocess
import sys
from pathlib import Path

EXE = r"D:\Projects\h4-secondary\Cortex Command.exe"
RUNNER = r"D:\Projects\h4-secondary\tools\win32_test_runner.py"
CWD = r"D:\Projects\h4-secondary"
OUT_ROOT = Path(r"D:\mx\w21\battery")
TABLE = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w21-h4-secondary\battery.json")

CASES = [
    ("protocol", 90, ["-headless", "-net-protocol-selftest"]),
    ("identity", 90, ["-headless", "-net-identity-selftest"]),
    ("session", 90, ["-headless", "-net-session-selftest"]),
    ("lockstep", 180, ["-headless", "-net-lockstep-selftest"]),
    ("match", 90, ["-headless", "-net-match-selftest"]),
    ("auth", 90, ["-headless", "-net-auth-selftest"]),
    ("admission", 90, ["-headless", "-net-admission-selftest"]),
    ("reconnect", 90, ["-headless", "-net-reconnect-selftest"]),
    ("reconnect-session", 120, ["-headless", "-net-reconnect-session-selftest"]),
    ("discovery", 90, ["-headless", "-net-discovery-selftest"]),
    ("controller-frame", 90, ["-headless", "-controller-frame-selftest"]),
    ("script-graph", 180, ["-headless", "-script-graph-selftest", "-num-lua-states", "4"]),
]


def main() -> int:
    OUT_ROOT.mkdir(parents=True, exist_ok=True)
    rows = []
    failed = 0
    for name, timeout, args in CASES:
        out = OUT_ROOT / name
        out.mkdir(parents=True, exist_ok=True)
        cmd = [sys.executable, RUNNER, "--cwd", CWD, "--out", str(out), "--timeout", str(timeout), "--", EXE, *args]
        print("RUN", name, flush=True)
        completed = subprocess.run(cmd, check=False, capture_output=True, text=True)
        launch = {}
        launch_path = out / "launch.json"
        if launch_path.exists():
            launch = json.loads(launch_path.read_text(encoding="utf-8"))
        row = {
            "name": name,
            "runner_exit": completed.returncode,
            "exit_code": launch.get("exit_code"),
            "timed_out": launch.get("timed_out"),
            "last_verdict": launch.get("last_verdict"),
            "out": str(out),
        }
        rows.append(row)
        print(json.dumps(row), flush=True)
        if completed.returncode != 0 or launch.get("exit_code") not in (0, None):
            failed += 1
    TABLE.write_text(json.dumps({"failed": failed, "rows": rows}, indent=2), encoding="utf-8")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
