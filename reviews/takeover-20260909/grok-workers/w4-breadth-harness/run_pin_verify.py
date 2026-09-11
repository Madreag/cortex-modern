"""Run Pin.verify(artifacts=True) and write the result. No engine launch."""
from __future__ import annotations

import importlib.util
import json
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE / "pin-verify-rerun.txt"
BREADTH = Path(r"D:\Projects\reviews\takeover-20260909\run_breadth.py")
MANIFEST = Path(
    r"D:\Projects\reviews\recovery-2026-09-07\contract-audit\grouped-build-bb3cf264\build.json"
)

import sys

spec = importlib.util.spec_from_file_location("breadth_pin_verify", BREADTH)
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)

lines = [
    "command: python grok-workers/w4-breadth-harness/run_pin_verify.py",
    f"run_breadth.py: {BREADTH}",
    f"build_manifest: {MANIFEST}",
    "Pin.verify(artifacts=True)",
]
try:
    pin = module.Pin(41, MANIFEST)
    result = pin.verify(artifacts=True)
    lines.append("result: pass")
    lines.append(json.dumps(result, indent=2, default=str))
    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("pass")
    print(json.dumps(result, indent=2, default=str))
except Exception as error:
    lines.append("result: fail")
    lines.append(f"{type(error).__name__}: {error}")
    lines.append(traceback.format_exc())
    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("fail")
    print(f"{type(error).__name__}: {error}")
    raise SystemExit(1)
