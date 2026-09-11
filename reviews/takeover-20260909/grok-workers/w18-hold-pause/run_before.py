"""Unmodified be0a2672a9 lockstep/reconnect-session selftests into D:\\mx\\w18\\before-*."""

import hashlib
import json
import subprocess
import sys
from pathlib import Path

REPO = Path(r"D:\Projects\takeover-build")
RUNNER = Path(r"D:\Projects\hold-pause\tools\win32_test_runner.py")
EXE = REPO / "Cortex Command.exe"
ROOT = Path(r"D:\mx\w18")
REPORT_DIR = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w18-hold-pause")

CASES = [
    ("before-lockstep", ["-headless", "-net-lockstep-selftest"], 180),
    ("before-reconnect-session", ["-headless", "-net-reconnect-session-selftest"], 120),
]


def sha256_of(path: Path) -> str:
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def prepare_runtime(out: Path) -> Path:
    sys.path.insert(0, str(REPO / "tools"))
    from run_sim_test import prepare_runtime as _prepare

    return _prepare(REPO, out)


def main() -> int:
    ROOT.mkdir(parents=True, exist_ok=True)
    records = []
    for name, args, timeout in CASES:
        out = ROOT / name
        if out.exists():
            raise SystemExit(f"out already exists: {out}")
        out.mkdir(parents=True)
        runtime = prepare_runtime(out)
        cmd = [
            sys.executable,
            str(RUNNER),
            "--cwd",
            str(runtime),
            "--out",
            str(out),
            "--timeout",
            str(timeout),
            "--",
            str(EXE),
            *args,
        ]
        print("CMD", " ".join(cmd), flush=True)
        proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
        stdout_log = out / "stdout.log"
        verdicts = []
        if stdout_log.exists():
            for line in stdout_log.read_text(encoding="utf-8", errors="replace").splitlines():
                if "PASS" in line or "FAIL" in line:
                    verdicts.append(line)
        record = {
            "name": name,
            "cmd": cmd,
            "runner_exit": proc.returncode,
            "exe_sha256": sha256_of(EXE),
            "verdicts": verdicts[-40:],
            "out": str(out),
            "runner_stdout": proc.stdout,
            "runner_stderr": proc.stderr,
        }
        (out / "w18_record.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
        (REPORT_DIR / f"{name}.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
        print(f"EXIT {name} {proc.returncode}", flush=True)
        records.append(record)
    (REPORT_DIR / "before_summary.json").write_text(json.dumps(records, indent=2), encoding="utf-8")
    return 0 if all(item["runner_exit"] == 0 for item in records) else 1


if __name__ == "__main__":
    raise SystemExit(main())
