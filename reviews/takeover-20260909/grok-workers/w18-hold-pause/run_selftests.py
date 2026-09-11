"""Launch W18 selftests via hold-pause win32_test_runner.py into D:\\mx\\w18."""

import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(r"D:\Projects\hold-pause")
RUNNER = REPO / "tools" / "win32_test_runner.py"
EXE = REPO / "Cortex Command.exe"
ROOT = Path(r"D:\mx\w18")
REPORT_DIR = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w18-hold-pause")

NETWORK = [
    ("net-protocol", ["-headless", "-net-protocol-selftest"], 120),
    ("net-identity", ["-headless", "-net-identity-selftest"], 120),
    ("net-session", ["-headless", "-net-session-selftest"], 120),
    ("net-lockstep", ["-headless", "-net-lockstep-selftest"], 180),
    ("net-match", ["-headless", "-net-match-selftest"], 120),
    ("net-auth", ["-headless", "-net-auth-selftest"], 120),
    ("net-admission", ["-headless", "-net-admission-selftest"], 120),
    ("net-reconnect", ["-headless", "-net-reconnect-selftest"], 120),
    ("net-reconnect-session", ["-headless", "-net-reconnect-session-selftest"], 120),
    ("net-discovery", ["-headless", "-net-discovery-selftest"], 120),
]
CONTROLLER = ("controller-frame", ["-headless", "-controller-frame-selftest"], 120)
NATIVE = [
    ("native-graph", ["-headless", "-script-graph-selftest", "-num-lua-states", "4"], 300, {}),
    ("native-graph-diag-on", ["-headless", "-script-graph-selftest", "-num-lua-states", "4"], 300, {"CC_TEST_NET_RECLAIM_DIAG": "1"}),
]


def sha256_of(path: Path) -> str:
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def prepare_runtime(out: Path) -> Path:
    sys.path.insert(0, str(REPO / "tools"))
    from run_sim_test import prepare_runtime as _prepare

    return _prepare(REPO, out)


def launch(name: str, args: list[str], timeout: int, env: dict | None = None) -> dict:
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
    proc = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=({**os.environ, **env} if env else None),
    )
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
        "runner_stdout": proc.stdout,
        "runner_stderr": proc.stderr,
        "exe_sha256": sha256_of(EXE) if EXE.exists() else "",
        "verdicts": verdicts[-40:],
        "out": str(out),
    }
    (out / "w18_record.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
    (REPORT_DIR / f"{name}.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
    print(f"EXIT {name} {proc.returncode}", flush=True)
    return record


def main() -> int:
    ROOT.mkdir(parents=True, exist_ok=True)
    names = sys.argv[1:]
    records = []
    suite = list(NETWORK) + [CONTROLLER]
    for name, args, timeout in suite:
        if names and name not in names:
            continue
        records.append(launch(name, args, timeout))
    for name, args, timeout, env in NATIVE:
        if names and name not in names:
            continue
        records.append(launch(name, args, timeout, env))
    (REPORT_DIR / "selftest_summary.json").write_text(json.dumps(records, indent=2), encoding="utf-8")
    return 0 if all(item["runner_exit"] == 0 for item in records) else 1


if __name__ == "__main__":
    raise SystemExit(main())
