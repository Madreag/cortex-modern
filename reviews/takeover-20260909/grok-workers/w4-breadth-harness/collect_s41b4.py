"""Collect s41b4 per-case verdicts, fl delays, and extra PRINT lines. Read-only on D:\\mx."""
from __future__ import annotations

import json
import re
from pathlib import Path

ROOT = Path(r"D:\mx\s41b4")
SCRATCH = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w4-breadth-harness")
AUTO = re.compile(r"\[net-match\] auto input delay: peer 2 rtt (\d+)ms -> (\d+) frames")
ERROR = re.compile(r"ERROR:")


def read_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def delay_line(path: Path):
    if not path.is_file():
        return None
    text = path.read_text(encoding="utf-8-sig", errors="replace")
    found = AUTO.search(text)
    return found.group(0) if found else None


def peer_delays(path: Path):
    if not path.is_file():
        return None
    data = read_json(path)
    service = data.get("service", data)
    return service.get("runner", {}).get("lockstep", {}).get("peer_input_delays")


def console_errors(job: Path, rung: str):
    hits = []
    for rel in (
        job / rung / "runtime" / "LogConsole.txt",
        job / rung / "stdout.log",
    ):
        if not rel.is_file():
            continue
        for i, line in enumerate(rel.read_text(encoding="utf-8-sig", errors="replace").splitlines(), 1):
            if ERROR.search(line):
                hits.append(f"{rel}:{i}: {line.strip()}")
    return hits


def main() -> int:
    breadth = read_json(ROOT / "breadth.json")
    steps = []
    for row in breadth.get("steps", []):
        steps.append({
            "id": row.get("id"),
            "family": row.get("family"),
            "passed": row.get("passed"),
            "exit": row.get("exit"),
            "failures": row.get("failures"),
            "log": row.get("log"),
            "out": None,
        })
    plan = read_json(ROOT / "plan.json")
    out_by_id = {job["id"]: job["out"] for job in plan}
    for row in steps:
        row["out"] = out_by_id.get(row["id"])

    fl = {}
    for name in ("fl100", "fl200"):
        job = Path(out_by_id[name])
        folder = job / "fl" / name
        fl[name] = {
            "host_line": delay_line(folder / "host" / "stdout.log"),
            "client_line": delay_line(folder / "client" / "stdout.log"),
            "host_peer_input_delays": peer_delays(folder / "host_report.json"),
            "client_peer_input_delays": peer_delays(folder / "client_report.json"),
            "result_pass": read_json(folder / "result.json").get("pass") if (folder / "result.json").is_file() else None,
            "host_stdout": str(folder / "host" / "stdout.log"),
            "client_stdout": str(folder / "client" / "stdout.log"),
        }

    extra = {}
    for name, rung in (("compat_extra_source22", "source22"), ("compat_extra_approved", "approved")):
        job = Path(out_by_id[name])
        summary = read_json(job / "summary.json")
        rows = summary.get("results", [])
        extra[name] = {
            "out": str(job),
            "fixture_sha256": summary.get("fixture_sha256"),
            "lines": rows[0].get("lines") if rows else None,
            "cases": rows[0].get("cases") if rows else None,
            "exit_code": rows[0].get("exit_code") if rows else None,
            "errors": console_errors(job, rung),
            "summary": str(job / "summary.json"),
        }

    s22 = extra["compat_extra_source22"]
    appr = extra["compat_extra_approved"]
    lines_equal = s22["lines"] == appr["lines"]
    no_errors = not s22["errors"] and not appr["errors"]
    diffs = []
    a, b = s22["lines"] or [], appr["lines"] or []
    for i, (left, right) in enumerate(zip(a, b)):
        if left != right:
            diffs.append({"index": i, "source22": left, "approved": right})
    if len(a) != len(b):
        diffs.append({"index": "len", "source22_len": len(a), "approved_len": len(b),
                      "source22_only": a[len(b):], "approved_only": b[len(a):]})

    out = {
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
        },
        "steps": steps,
        "fl": fl,
        "extra": extra,
        "extra_compare": {
            "lines_identical": lines_equal,
            "neither_has_ERROR": no_errors,
            "diffs": diffs,
            "source22_line_count": len(a),
            "approved_line_count": len(b),
            "may_update_pin": lines_equal and no_errors,
        },
    }
    dest = SCRATCH / "s41b4-collect.json"
    dest.write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({
        "execution_pass": breadth.get("execution_pass"),
        "validation_errors": breadth.get("validation_errors"),
        "steps": [{k: row[k] for k in ("id", "passed", "exit", "failures")} for row in steps],
        "fl": fl,
        "extra_compare": out["extra_compare"],
        "extra_fixture": s22["fixture_sha256"],
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
