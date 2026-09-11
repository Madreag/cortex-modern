"""Compare wrapper heal required checks vs Source40 heal result.json. Read-only."""
from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
BREADTH = Path(r"D:\Projects\reviews\takeover-20260909\run_breadth.py")
SRC40 = Path(
    r"D:\Projects\reviews\recovery-2026-09-06\expanded-mod-gates"
    r"\20260909_112620_heal_26d39dc2\result.json"
)

spec = importlib.util.spec_from_file_location("breadth_heal_cmp", BREADTH)
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)

required = list(module.BASELINE["heal"])
data = json.loads(SRC40.read_text(encoding="utf-8-sig"))
present = [row["name"] for row in data.get("checks", [])]
req_set = set(required)
pres_set = set(present)
only_wrapper = sorted(req_set - pres_set)
only_source40 = sorted(pres_set - req_set)
payload = {
    "source40_input_delay": data.get("input_delay"),
    "wrapper_count": len(required),
    "source40_count": len(present),
    "wrapper_names": required,
    "source40_names": present,
    "only_in_wrapper": only_wrapper,
    "only_in_source40": only_source40,
    "equal_as_sets": req_set == pres_set,
    "same_order": required == present,
}
out = HERE / "heal-check-compare.json"
out.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
print(json.dumps(payload, indent=2))
