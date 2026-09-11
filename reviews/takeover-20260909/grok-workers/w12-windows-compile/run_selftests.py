"""Launch Mac-positive20 selftests via win32_test_runner.py into D:\\mx\\w12."""

import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(r"D:\Projects\takeover-build")
RUNNER = REPO / "tools" / "win32_test_runner.py"
EXE = REPO / "Cortex Command.exe"
ROOT = Path(r"D:\mx\w12")
REPORT_DIR = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w12-windows-compile")
PREPARE = REPO / "tools" / "run_sim_test.py"

# Mac argv tails from native-positive20-verified-0244 launch.json / selftests.json
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
NATIVE = ("native-graph", ["-headless", "-script-graph-selftest", "-num-lua-states", "4"], 300)

PASS_RE = re.compile(r".*(PASS|FAIL).*")


def sha256_of(path: Path) -> str:
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def prepare_runtime(out: Path) -> Path:
    sys.path.insert(0, str(REPO / "tools"))
    from run_sim_test import prepare_runtime as _prepare

    return _prepare(REPO, out)


def launch(name: str, args: list[str], timeout: int) -> dict:
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
        "runner_stdout": proc.stdout,
        "runner_stderr": proc.stderr,
        "out": str(out),
        "stdout_log": str(stdout_log) if stdout_log.exists() else None,
        "verdict_lines": verdicts,
    }
    launch_json = out / "launch.json"
    if launch_json.exists():
        try:
            lj = json.loads(launch_json.read_text(encoding="utf-8"))
            record["engine_exit"] = lj.get("exit_code")
            record["timed_out"] = lj.get("timed_out")
            record["launch_verdict_lines"] = lj.get("verdict_lines")
        except json.JSONDecodeError as exc:
            record["launch_json_error"] = str(exc)
    (REPORT_DIR / f"selftest-{name}.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
    print(f"DONE {name} runner_exit={proc.returncode} verdicts={len(verdicts)}", flush=True)
    for line in verdicts[-8:]:
        print(" ", line, flush=True)
    return record


def main() -> int:
    ROOT.mkdir(parents=True, exist_ok=True)
    if not EXE.is_file():
        raise SystemExit(f"exe missing: {EXE}")
    meta = {
        "exe": str(EXE),
        "exe_bytes": EXE.stat().st_size,
        "exe_sha256": sha256_of(EXE),
        "head": "be0a2672a9",
    }
    (REPORT_DIR / "exe-meta.json").write_text(json.dumps(meta, indent=2), encoding="utf-8")
    print(json.dumps(meta, indent=2), flush=True)
    results = []
    for name, args, timeout in [*NETWORK, CONTROLLER, NATIVE]:
        results.append(launch(name, args, timeout))
    (REPORT_DIR / "selftest-results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
