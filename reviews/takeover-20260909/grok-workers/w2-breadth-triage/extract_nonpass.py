"""Dump every non-pass step with failures, evidence, plan command, and job listing."""
import json
from pathlib import Path

OUT = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w2-breadth-triage")
ROOT = Path(r"D:\mx\s41b3")

breadth = json.loads((ROOT / "breadth.json").read_text(encoding="utf-8"))
plan = json.loads((ROOT / "plan.json").read_text(encoding="utf-8"))
plan_by_id = {p["id"]: p for p in plan}

rows = []
for i, step in enumerate(breadth["steps"]):
    passed = step.get("passed")
    row = {
        "i": i,
        "id": step.get("id"),
        "family": step.get("family"),
        "passed": passed,
        "exit": step.get("exit"),
        "log": step.get("log"),
        "out": plan_by_id.get(step.get("id"), {}).get("out"),
        "command": plan_by_id.get(step.get("id"), {}).get("command"),
        "meta": plan_by_id.get(step.get("id"), {}).get("meta"),
        "n_failures": len(step.get("failures") or []),
        "n_evidence": len(step.get("evidence") or []),
        "failures": step.get("failures") or [],
        "evidence": step.get("evidence") or [],
    }
    rows.append(row)

(OUT / "all_cases.json").write_text(json.dumps(rows, indent=2, default=str), encoding="utf-8")
nonpass = [r for r in rows if r["passed"] is not True]
(OUT / "nonpass_cases.json").write_text(json.dumps(nonpass, indent=2, default=str), encoding="utf-8")

lines = ["# all cases", "i | passed | exit | id | out"]
for r in rows:
    lines.append(f"{r['i']:02d} | {r['passed']} | {r['exit']} | {r['id']} | {r['out']}")
lines.append("")
lines.append("# validation_errors")
for e in breadth.get("validation_errors") or []:
    lines.append(str(e))
(OUT / "all_cases.txt").write_text("\n".join(lines), encoding="utf-8")
print("nonpass", len(nonpass), [r["id"] for r in nonpass])
print("validation_errors", breadth.get("validation_errors"))
print("passed", breadth.get("passed"), "execution_pass", breadth.get("execution_pass"))
