"""Collect s41b5 heal verdict and prediction counters. Read-only on D:\\mx."""
from __future__ import annotations

import json
from pathlib import Path

ROOT = Path(r"D:\mx\s41b5")
SCRATCH = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w4-breadth-harness")


def read_json(path: Path):
    if not path.is_file():
        return None
    return json.loads(path.read_text(encoding="utf-8-sig"))


def main() -> int:
    breadth = read_json(ROOT / "breadth.json") or {}
    heal_out = ROOT / "j73"
    result = read_json(heal_out / "result.json") or {}
    checks = []
    for row in result.get("checks", []):
        checks.append({
            "name": row.get("name"),
            "status": row.get("status"),
            "detail": row.get("detail"),
        })
    host = read_json(heal_out / "fresh/e2e/resync_heal/host_report.json") or {}
    client = read_json(heal_out / "fresh/e2e/resync_heal/client_report.json") or {}
    log = (ROOT / "logs" / "heal.log")
    log_text = log.read_text(encoding="utf-8-sig", errors="replace") if log.is_file() else None
    payload = {
        "breadth": {
            "path": str(ROOT / "breadth.json"),
            "execution_pass": breadth.get("execution_pass"),
            "complete": breadth.get("complete"),
            "stop_reason": breadth.get("stop_reason"),
            "missing_jobs": breadth.get("missing_jobs"),
            "validation_errors": breadth.get("validation_errors"),
            "expected_jobs": breadth.get("expected_jobs"),
            "source_head": breadth.get("source_head"),
            "exe_sha256": breadth.get("exe_sha256"),
            "steps": breadth.get("steps"),
        },
        "heal_out": str(heal_out),
        "result_path": str(heal_out / "result.json"),
        "input_delay": result.get("input_delay"),
        "lane": result.get("lane"),
        "pass": result.get("pass"),
        "complete": result.get("complete"),
        "source_unchanged": result.get("source_unchanged"),
        "checks": checks,
        "heal_log": log_text,
        "host_local_prediction": host.get("local_prediction"),
        "client_local_prediction": client.get("local_prediction"),
        "host_resyncs": host.get("resyncs"),
        "client_resyncs": client.get("resyncs"),
    }
    dest = SCRATCH / "s41b5-collect.json"
    dest.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({
        "execution_pass": payload["breadth"]["execution_pass"],
        "steps": payload["breadth"]["steps"],
        "input_delay": payload["input_delay"],
        "pass": payload["pass"],
        "checks": checks,
        "heal_log": log_text,
        "host_local_prediction": payload["host_local_prediction"],
        "client_local_prediction": payload["client_local_prediction"],
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
