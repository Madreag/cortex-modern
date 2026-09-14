"""Wait until no Cortex Command process is running and the lead locks are gone."""

import subprocess
import sys
import time
from pathlib import Path

LOCK_BATTERY = Path(r"D:\mx\LEAD_BATTERY.lock")
LOCK_EXCLUSIVE = Path(r"D:\mx\LEAD_EXCLUSIVE.lock")


def cortex_pids():
    proc = subprocess.run(
        [
            "pwsh",
            "-NoProfile",
            "-Command",
            "Get-Process 'Cortex Command*' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id",
        ],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    return [line.strip() for line in proc.stdout.splitlines() if line.strip().isdigit()]


def wait_engine_idle(timeout_s=7200.0, interval_s=30.0):
    deadline = time.time() + timeout_s
    while True:
        locks = [str(path) for path in (LOCK_BATTERY, LOCK_EXCLUSIVE) if path.exists()]
        pids = cortex_pids()
        if not locks and not pids:
            print("engine idle", flush=True)
            return
        if time.time() >= deadline:
            raise SystemExit(f"STOP: still blocked after {timeout_s:.0f}s locks={locks} pids={pids}")
        print(f"waiting locks={locks} pids={pids}", flush=True)
        time.sleep(interval_s)


if __name__ == "__main__":
    wait_engine_idle()
    sys.exit(0)
