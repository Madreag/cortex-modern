"""Shared plumbing for the H4 Phase A §9a two-peer gate drivers.

Every driver in this directory:
  * refuses to run unless the executable hashes to the value in build-manifest.json,
  * launches each peer through tools/run_sim_test.py's make_run, so every process gets a private
    writable runtime, a muted private desktop and its own Temp,
  * keeps the recovery record OUTSIDE the per-process runtime, because the whole point of the
    crash/relaunch cases is that a second process finds the first one's ticket,
  * terminates only processes it started itself, by job handle, never by name,
  * writes verdict.json next to its evidence and prints one GATE line.

Nothing here opens a socket by itself; the engine does, on loopback ports this module hands out.
"""

from __future__ import annotations

import datetime
import hashlib
import json
import os
import subprocess
import sys
import threading
import time
from pathlib import Path

# LANE EDIT: the gates run against the approved Source33 path, not the isolated Phase-A
# worktree, and the manifest is supplied by --build-manifest (env, so the eight drivers
# keep their own argparse untouched).
REPO = Path("D:/Projects/p4b-interp-validation")
LANE = Path("D:/Projects/reviews/claude-review-2026-09-08/lanes/source33-lanes")
GATES = Path(__file__).resolve().parent
MANIFEST = Path(
    os.environ.get("CC_H4_BUILD_MANIFEST", str(GATES / "build-manifest.json"))
)
# LANE EDIT: a short run root; the deep lane path plus the junctioned Data blows MAX_PATH.
RUN_ROOT = Path(os.environ.get("CC_H4_RUN_ROOT", "D:/mx/s33lanes/h4"))
EXE = REPO / "Cortex Command.exe"

sys.path.insert(0, str(REPO / "tools"))
from run_sim_test import make_run  # noqa: E402  (path is set above on purpose)
from h4_gate_evidence import reseat_evidence


def sha256(path: Path) -> str:
    with Path(path).open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def verify_executable() -> dict:
    """Refuses to produce evidence against a binary other than the one the manifest names."""
    if not MANIFEST.exists():
        raise SystemExit(
            f"no build manifest at {MANIFEST}; write one before running a gate"
        )
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    # LANE EDIT: accept the contract-audit manifest spelling as well as the gate one.
    expected = manifest.get("executable_sha256") or manifest["exe_sha256"]
    manifest["executable_sha256"] = expected
    manifest.setdefault("built_from", manifest.get("head"))
    actual = sha256(EXE)
    if actual != expected:
        raise SystemExit(
            "executable hash does not match the build manifest\n"
            f"  expected {expected}\n"
            f"  actual   {actual}\n"
            "rebuild, or update the manifest deliberately."
        )
    manifest["verified_sha256"] = actual
    return manifest


def out_root(name: str) -> Path:
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d_%H%M%S")
    root = RUN_ROOT / f"{name}_{stamp}"
    root.mkdir(parents=True, exist_ok=False)
    return root


def peer_run(args, out_dir: Path, timeout: float, expected=None):
    """One engine process with a private runtime. args excludes the executable and -headless."""
    return make_run(REPO, args, out_dir, timeout, expected=expected)


def drive(runs: dict, stagger_s: float = 1.5) -> dict:
    """Starts each run on its own thread, in insertion order, and joins them all.

    The first peer is the host in every driver here, so the stagger is what gives it time to bind
    its listening socket before a joiner arrives.
    """
    records: dict = {}
    threads = []

    def run_one(key, run):
        try:
            run.start()
            records[key] = run.finish()
        except (
            Exception
        ) as exc:  # a launch failure is a gate result, not a crash of the driver
            records[key] = {"error": repr(exc)}

    for index, (key, run) in enumerate(runs.items()):
        thread = threading.Thread(target=run_one, args=(key, run), name=f"peer-{key}")
        threads.append(thread)
        thread.start()
        if index + 1 < len(runs):
            time.sleep(stagger_s)
    for thread in threads:
        thread.join()
    return records


def start_one(run):
    """Starts a run without waiting, for the drivers that terminate a peer mid-match."""
    run.start()
    return run


def wait_for_log(run, needle: str, timeout_s: float) -> bool:
    """Waits until the run's stdout carries `needle`. Polls the log the runner already writes."""
    log = Path(run.out) / "stdout.log" if hasattr(run, "out") else None
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if log and log.exists():
            text = log.read_text(encoding="utf-8-sig", errors="replace")
            if needle in text:
                return True
        if run.poll() is not None:
            return False
        time.sleep(0.25)
    return False


def wait_in_match(run, settle_s: float = 4.0, timeout_s: float = 240.0) -> bool:
    """Blocks until this peer is really in the running match, then settles briefly.

    LANE FIX: replaces the fixed *_AFTER_S sleeps. Those were never calibrated (the A3 lane
    only ever ran these drivers with --dry-run) and on a fast machine they fire at or after
    the end of the match instead of in the middle of it.
    """
    if not wait_for_log(run, "lobby_snapshot: state=Running", timeout_s):
        return False
    time.sleep(settle_s)
    return run.poll() is None


def read_json(path: Path):
    try:
        parsed = json.loads(Path(path).read_text(encoding="utf-8-sig"))
    except Exception as exc:
        return {"parse_error": repr(exc)}
    # LANE FIX: surface the service sub-report's reconnect/runner blocks where the drivers
    # look for them. Only fills a key that is absent, so a real top-level block still wins.
    if isinstance(parsed, dict):
        service = parsed.get("service")
        if isinstance(service, dict):
            for key in ("reconnect", "runner"):
                if parsed.get(key) is None and isinstance(service.get(key), dict):
                    parsed[key] = service[key]
    return parsed


def log_text(out_dir: Path) -> str:
    log = Path(out_dir) / "stdout.log"
    return log.read_text(encoding="utf-8-sig", errors="replace") if log.exists() else ""


def check_reseat_issued(checks, name: str, host_log: str, reconnect: dict, evidence) -> bool:
    ok, detail = reseat_evidence(reconnect, host_log)
    return checks.check(name, ok, detail, evidence)


class Checks:
    """Accumulates named pass/fail checks and renders the driver's verdict."""

    def __init__(self, gate: str, manifest: dict, root: Path):
        self.gate = gate
        self.manifest = manifest
        self.root = Path(root)
        self.items: list[dict] = []

    def check(self, name: str, ok: bool, detail: str = "", evidence=()) -> bool:
        self.items.append(
            {
                "name": name,
                "status": "pass" if ok else "fail",
                "detail": detail,
                "evidence": [str(Path(item)) for item in evidence],
            }
        )
        return bool(ok)

    def finish(self, extra: dict | None = None) -> int:
        failed = [item for item in self.items if item["status"] != "pass"]
        verdict = {
            "gate": self.gate,
            "passed": not failed,
            "executable_sha256": self.manifest["verified_sha256"],
            "built_from": self.manifest.get("built_from"),
            "configuration": self.manifest.get("configuration"),
            "finished_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "checks": self.items,
        }
        if extra:
            verdict.update(extra)
        (self.root / "verdict.json").write_text(
            json.dumps(verdict, indent=2), encoding="utf-8"
        )
        for item in self.items:
            print(
                f"  {item['status'].upper():4} {item['name']} {item['detail']}".rstrip()
            )
        print(f"GATE {self.gate} {'PASS' if not failed else 'FAIL'} -> {self.root}")
        return 0 if not failed else 1


def common_args(port: int, ticks: int, ticket: Path) -> list[str]:
    """The arguments every peer in these gates shares."""
    return [
        "-net-match-service-e2e",
        "-net-port",
        str(port),
        "-net-match-ticks",
        str(ticks),
        "-net-reconnect-ticket",
        str(ticket),
    ]


def dry_run_banner(gate: str, argvs: dict) -> int:
    """Prints the exact commands a gate would launch, without launching anything."""
    print(f"DRY-RUN {gate}")
    for key, argv in argvs.items():
        printable = subprocess.list2cmdline([str(EXE), "-headless", *map(str, argv)])
        print(f"  {key}: {printable}")
    return 0
