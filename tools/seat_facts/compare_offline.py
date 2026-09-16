"""Compare offline fixture behavior across retained, broken and repaired executables."""
from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path


def trace_without_wall_seconds(path):
    data = json.loads(Path(path).read_text(encoding="utf-8-sig"))
    value = copy.deepcopy(data)
    for run in value.get("runs", []):
        run.get("numeric", {}).pop("__wall_seconds", None)
    for scenario in value.get("scenarios", {}).values():
        scenario.get("numeric", {}).pop("__wall_seconds", None)
    return value


def compare(root):
    rows = []
    for name in ("SimBaseline", "TerrainStress", "ActorStress", "LuaBaseline", "LuaRandomStress", "LuaPairsStress",
                 "LuaOsStubTest", "ModSmokeLoading", "TerrainCarveStress", "ThreadStress", "PerfBench", "Net_Lifecycle_Test",
                 "p4_sp", "sp_identity", "ak47", "PreviewCompat", "PreviewModuleCompat", "F15Retirement_LateStateGain"):
        traces = {stage: root / stage / name / "trace.json" for stage in ("reference", "red", "green")}
        present = all(path.is_file() for path in traces.values())
        row = {"case": name, "traces": {stage: str(path) for stage, path in traces.items()}, "present": present}
        if not present:
            row["reason"] = "missing traces for offline compare"
            row["red_equal_reference"] = None
            row["green_equal_reference"] = None
        else:
            reference = trace_without_wall_seconds(traces["reference"])
            row["red_equal_reference"] = trace_without_wall_seconds(traces["red"]) == reference
            row["green_equal_reference"] = trace_without_wall_seconds(traces["green"]) == reference
        rows.append(row)
    retained_pie = Path("D:/mx/opus-f40-20260914/sp/control-2/trace.json.simdump.txt")
    retained_ak47 = Path("D:/mx/opus-f40-20260914/ak47/control/trace.json")
    sp = {stage: root / stage / "sp_identity/trace.json.simdump.txt" for stage in ("reference", "red", "green")}
    sp_present = retained_pie.is_file() and all(path.is_file() for path in sp.values())
    retained = {"pie_control": str(retained_pie), "ak47_control": str(retained_ak47), "pie_inputs_present": sp_present}
    if sp_present:
        retained["pie_equal_retained"] = {stage: path.read_bytes() == retained_pie.read_bytes() for stage, path in sp.items()}
    if retained_ak47.is_file():
        reference = trace_without_wall_seconds(retained_ak47)
        retained["ak47_equal_retained"] = {stage: trace_without_wall_seconds(root / stage / "ak47/trace.json") == reference for stage in sp}
        retained["ak47_final_hashes"] = {stage: json.loads((root / stage / "ak47/trace.json").read_text(encoding="utf-8-sig"))["runs"][0]["final_total_hash"] for stage in sp}
    return {"rows": rows, "retained": retained, "ignored_fields": ["runs[].numeric.__wall_seconds", "scenarios.*.numeric.__wall_seconds"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    args = parser.parse_args()
    result = compare(args.root)
    target = args.root / "offline-comparison.json"
    target.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2))
    retained = result["retained"]
    required = [*retained.get("pie_equal_retained", {}).values(), *retained.get("ak47_equal_retained", {}).values()]
    compared = all(row.get("present") for row in result["rows"])
    red_diverges = all(row.get("red_equal_reference") is False for row in result["rows"])
    green_matches = all(row.get("green_equal_reference") is True for row in result["rows"])
    pie_ok = len(required) == 6 and all(required)
    return 0 if compared and red_diverges and green_matches and pie_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
