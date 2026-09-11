"""Adversarial probes for W13. Read-only on the harness; writes only here."""
from __future__ import annotations

import importlib.util
import json
import math
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
FIXED = Path(r"D:\Projects\reviews\takeover-20260909\run_breadth.py")
PRE = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w4-breadth-harness\pre\run_breadth.py")
J80 = Path(r"D:\mx\s41b4\j80\summary.json")
J81 = Path(r"D:\mx\s41b4\j81\summary.json")
HEAL40 = Path(
    r"D:\Projects\reviews\recovery-2026-09-06\expanded-mod-gates"
    r"\20260909_112620_heal_26d39dc2\result.json"
)


def load(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


fixed = load(FIXED, "breadth_fixed_w13")
pre = load(PRE, "breadth_pre_w13")

PIN = "PRINT: [deferral] case=spawn_child ok=1 uid=1049517"
OTHER = "PRINT: [deferral] case=done ok=1 step=150 ai_runs=75"
BASE = [
    "PRINT: [deferral] case=shared_play ok=1 live=1 hold=1 restart=1 live_playing=1",
    PIN,
    OTHER,
]


def old_match(actual, expected) -> bool:
    return actual == expected


compat_cases = []


def add_compat(name, actual, expected=None):
    expected = BASE if expected is None else expected
    new = fixed.compat_lines_match(actual, expected)
    old = old_match(actual, expected)
    compat_cases.append({"name": name, "old_rejects": not old, "new_rejects": not new, "new_accepts": new})


add_compat("identical", list(BASE))
add_compat("uid_only", [BASE[0], "PRINT: [deferral] case=spawn_child ok=1 uid=1049513", OTHER])
add_compat("ok_zero", [BASE[0], "PRINT: [deferral] case=spawn_child ok=0 uid=1049517", OTHER])
add_compat("reordered", [PIN, BASE[0], OTHER])
add_compat("changed_non_uid", [BASE[0], PIN, "PRINT: [deferral] case=done ok=1 step=149 ai_runs=75"])
add_compat("spawn_extra_text", [BASE[0], PIN + " extra", OTHER])
add_compat("missing_spawn", [BASE[0], OTHER])
add_compat("extra_line", BASE + [OTHER])
add_compat("uid_non_digits", [BASE[0], "PRINT: [deferral] case=spawn_child ok=1 uid=abc", OTHER])
add_compat("wrong_case_name", [BASE[0], "PRINT: [deferral] case=spawn_other ok=1 uid=1049517", OTHER])

# formula vs C++ expression
tick = 1000.0 / 30.0
formula_rows = []
for rtt in (199, 200, 201, 202, 215, 216, 233, 367, 399, 400, 401, 415, 416, 433, 434):
    py = fixed.expected_auto_delay_frames(rtt)
    raw = rtt / tick
    formula_rows.append({
        "rtt": rtt,
        "rtt_over_tick": raw,
        "ceil_plus_1": py,
        "old_fl100_accepts": py == 8,
        "old_fl200_accepts": py == 14,
        "new_fl100_band": 200 <= rtt <= 215,
        "new_fl200_band": 400 <= rtt <= 415,
    })

ok_map = {"1": 1, "2": 13}
lag_cases = []


def add_lag(name, case, host_log, client_log, host_d, client_d):
    fails = fixed.fake_lag_auto_delay_failures(case, host_log, client_log, host_d, client_d)
    lag_cases.append({"name": name, "failures": fails, "rejects": bool(fails)})


add_lag("s41b4_fl200_measured", "fl200",
        "[net-match] auto input delay: peer 2 rtt 400ms -> 13 frames (manual floor 0)\n",
        "", {"1": 1, "2": 13}, {"1": 1, "2": 13})
add_lag("s41b4_fl100_measured", "fl100",
        "[net-match] auto input delay: peer 2 rtt 202ms -> 8 frames (manual floor 0)\n",
        "", {"1": 1, "2": 8}, {"1": 1, "2": 8})
add_lag("formula_wrong_400_to_14", "fl200",
        "[net-match] auto input delay: peer 2 rtt 400ms -> 14 frames (manual floor 0)\n",
        "", {"1": 1, "2": 14}, {"1": 1, "2": 14})
add_lag("dropped_plus_one", "fl200",
        "[net-match] auto input delay: peer 2 rtt 401ms -> 13 frames (manual floor 0)\n",
        "", {"1": 1, "2": 13}, {"1": 1, "2": 13})
add_lag("band_low_399", "fl200",
        "[net-match] auto input delay: peer 2 rtt 399ms -> 13 frames (manual floor 0)\n",
        "", {"1": 1, "2": 13}, {"1": 1, "2": 13})
add_lag("band_high_416", "fl200",
        "[net-match] auto input delay: peer 2 rtt 416ms -> 14 frames (manual floor 0)\n",
        "", {"1": 1, "2": 14}, {"1": 1, "2": 14})
add_lag("peer_map_disagree", "fl200",
        "[net-match] auto input delay: peer 2 rtt 400ms -> 13 frames (manual floor 0)\n",
        "", {"1": 1, "2": 13}, {"1": 1, "2": 14})
add_lag("client_log_disagree", "fl200",
        "[net-match] auto input delay: peer 2 rtt 400ms -> 13 frames (manual floor 0)\n",
        "[net-match] auto input delay: peer 2 rtt 401ms -> 14 frames (manual floor 0)\n",
        {"1": 1, "2": 13}, {"1": 1, "2": 13})
add_lag("host_line_missing", "fl200", "", "", ok_map, ok_map)
add_lag("hardcoded_13_at_400", "fl200",
        "[net-match] auto input delay: peer 2 rtt 400ms -> 13 frames (manual floor 0)\n",
        "", {"1": 1, "2": 13}, {"1": 1, "2": 13})

j80 = json.loads(J80.read_text(encoding="utf-8-sig"))
j81 = json.loads(J81.read_text(encoding="utf-8-sig"))
r80, r81 = j80["results"][0], j81["results"][0]
pin = fixed.BASELINE["compat"]["extra"]

select_none = fixed.select_jobs(["a", "b", "c"], None)
select_empty = fixed.select_jobs(["a", "b", "c"], [])

# Job stand-ins for select_jobs against REQUIRED_CASES
class Job:
    def __init__(self, id):
        self.id = id


all_ids = list(fixed.REQUIRED_CASES)
all_jobs = [Job(i) for i in all_ids]
selected_all = fixed.select_jobs(all_jobs, None)
unknown_err = None
try:
    fixed.select_jobs(all_jobs, ["not_a_case"])
except ValueError as exc:
    unknown_err = str(exc)
subset = fixed.select_jobs(all_jobs, ["heal", "fl200"])

heal40 = json.loads(HEAL40.read_text(encoding="utf-8-sig"))
heal_pre = list(pre.BASELINE["heal"])
heal_fixed = list(fixed.BASELINE["heal"])

payload = {
    "command": r"python D:\Projects\reviews\takeover-20260909\grok-workers\w13-harness-review\probe.py",
    "imported": str(FIXED),
    "compat_cases": compat_cases,
    "formula_rows": formula_rows,
    "lag_cases": lag_cases,
    "extra_compare": {
        "j80_vs_j81_cases": r80["cases"] == r81["cases"],
        "j80_vs_j81_lines": r80["lines"] == r81["lines"],
        "j80_line_count": len(r80["lines"]),
        "j81_line_count": len(r81["lines"]),
        "pin_cases_eq_j80": pin["cases"] == r80["cases"],
        "pin_lines_eq_j80": pin["lines"] == r80["lines"],
        "pin_lines_eq_j81": pin["lines"] == r81["lines"],
        "pin_fixture": pin["fixture_sha256"],
        "j80_fixture": j80["fixture_sha256"],
        "j81_fixture": j81["fixture_sha256"],
        "pin_vs_j80_case_keys_only_in_pin": sorted(set(pin["cases"]) - set(r80["cases"])),
        "pin_vs_j80_line_diffs": [pair for pair in zip(pin["lines"], r80["lines"]) if pair[0] != pair[1]],
        "j80_vs_j81_line_diffs": [pair for pair in zip(r80["lines"], r81["lines"]) if pair[0] != pair[1]],
    },
    "select_jobs": {
        "none_returns_same_ids": [j.id for j in selected_all] == all_ids,
        "none_is_copy": selected_all is not all_jobs,
        "empty_list_is_falsy_returns_all": select_empty == ["a", "b", "c"],
        "unknown_error": unknown_err,
        "subset_ids": [j.id for j in subset],
        "driver_inputs_always_adds_fixed_fixtures": True,
    },
    "heal": {
        "source40_input_delay": heal40.get("input_delay"),
        "source40_argv_has_delay_3": "-net-match-input-delay" in heal40.get("argv", {}).get("host", [])
        and heal40.get("argv", {}).get("host", [])[
            heal40.get("argv", {}).get("host", []).index("-net-match-input-delay") + 1
        ] == "3" if "-net-match-input-delay" in heal40.get("argv", {}).get("host", []) else False,
        "pre_vs_fixed_heal_list_equal": heal_pre == heal_fixed,
        "heal_list": heal_fixed,
        "pre_input_delay_literal": 0,
        "fixed_heal_input_delay_literal": 3,
    },
    "old_pin_rtt_to_frames": {
        "note": "frames = ceil(rtt/(1000/30))+1; old pin checked only the frame count",
        "fl100_old_wanted_frames": 8,
        "fl200_old_wanted_frames": 14,
        "rtt_giving_8": [r["rtt"] for r in formula_rows if r["ceil_plus_1"] == 8],
        "rtt_giving_7": [r["rtt"] for r in formula_rows if r["ceil_plus_1"] == 7],
        "rtt_giving_13": [r["rtt"] for r in formula_rows if r["ceil_plus_1"] == 13],
        "rtt_giving_14": [r["rtt"] for r in formula_rows if r["ceil_plus_1"] == 14],
        "exact_400_over_tick": 400 / tick,
        "exact_200_over_tick": 200 / tick,
        "exact_401_over_tick": 401 / tick,
    },
}

out = HERE / "probe.json"
out.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
print(json.dumps(payload, indent=2))
