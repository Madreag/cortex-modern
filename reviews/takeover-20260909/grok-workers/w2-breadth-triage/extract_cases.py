"""Extract per-case verdicts from s41b3 breadth.json + plan.json."""
import json
from pathlib import Path

OUT = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w2-breadth-triage")
ROOT = Path(r"D:\mx\s41b3")

breadth = json.loads((ROOT / "breadth.json").read_text(encoding="utf-8"))
plan = json.loads((ROOT / "plan.json").read_text(encoding="utf-8"))
drivers = json.loads((ROOT / "drivers.json").read_text(encoding="utf-8"))

# top-level summary
summary = {
    k: breadth.get(k)
    for k in (
        "source",
        "build_manifest",
        "exe_sha256",
        "source_head",
        "started",
        "complete",
        "passed",
        "execution_pass",
        "expected_jobs",
        "stop_reason",
        "missing_jobs",
        "finished",
        "validation_errors",
    )
}
(OUT / "breadth_summary.json").write_text(json.dumps(summary, indent=2, default=str), encoding="utf-8")

# plan entries
plan_rows = []
for i, item in enumerate(plan):
    if isinstance(item, dict):
        plan_rows.append(
            {
                "i": i,
                "keys": sorted(item.keys()),
                **{k: item.get(k) for k in item if k not in ("cmd", "command", "env") or True},
            }
        )
    else:
        plan_rows.append({"i": i, "type": type(item).__name__, "val": str(item)[:300]})

# strip huge fields
clean_plan = []
for item in plan:
    if not isinstance(item, dict):
        clean_plan.append(item)
        continue
    d = {}
    for k, v in item.items():
        if k in ("cmd", "command", "argv") and isinstance(v, list):
            d[k] = v
        elif isinstance(v, (str, int, float, bool)) or v is None:
            d[k] = v
        elif isinstance(v, list) and v and isinstance(v[0], str) and len(str(v)) < 2000:
            d[k] = v
        else:
            d[k] = f"<{type(v).__name__} len={len(v) if hasattr(v,'__len__') else '?'}>"
    clean_plan.append(d)
(OUT / "plan_compact.json").write_text(json.dumps(clean_plan, indent=2), encoding="utf-8")

# steps compact
steps = breadth.get("steps") or []
step_rows = []
for i, step in enumerate(steps):
    if not isinstance(step, dict):
        step_rows.append({"i": i, "type": type(step).__name__})
        continue
    row = {"i": i, "keys": sorted(step.keys())}
    for k in (
        "name",
        "id",
        "case",
        "job",
        "job_dir",
        "dir",
        "status",
        "passed",
        "ok",
        "verdict",
        "exit_code",
        "driver",
        "gate",
        "kind",
        "label",
        "result",
        "error",
        "failures",
        "reason",
        "message",
        "stdout",
        "stderr",
    ):
        if k in step:
            v = step[k]
            if isinstance(v, (str, int, float, bool)) or v is None:
                row[k] = v
            elif isinstance(v, list):
                row[k] = v[:40] if all(isinstance(x, (str, int, float, bool, type(None))) for x in v[:40]) else [str(x)[:400] for x in v[:20]]
            else:
                row[k] = str(v)[:400]
    # any other scalar
    for k, v in step.items():
        if k not in row and isinstance(v, (str, int, float, bool)):
            row[k] = v
    step_rows.append(row)

(OUT / "steps_compact.json").write_text(json.dumps(step_rows, indent=2), encoding="utf-8")
(OUT / "drivers_raw.json").write_text(json.dumps(drivers, indent=2, default=str)[:200000], encoding="utf-8")

# print first step keys
print("n_steps", len(steps))
print("n_plan", len(plan))
if steps:
    print("step0 keys", sorted(steps[0].keys()) if isinstance(steps[0], dict) else type(steps[0]))
    print("step0 sample", json.dumps({k: (steps[0][k] if not isinstance(steps[0][k], (dict, list)) else type(steps[0][k]).__name__) for k in steps[0]}, indent=2)[:2000])
if plan:
    print("plan0 keys", sorted(plan[0].keys()) if isinstance(plan[0], dict) else type(plan[0]))
    print("plan0", json.dumps({k: (plan[0][k] if not isinstance(plan[0][k], (dict, list)) else f"{type(plan[0][k]).__name__}:{len(plan[0][k])}") for k in plan[0]}, indent=2)[:2000])
