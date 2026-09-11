"""Record the exact wrapper invocation for s41b5. The caller runs the command."""
from pathlib import Path

cmd = [
    "python", "-B",
    r"D:\Projects\reviews\takeover-20260909\run_breadth.py",
    "--source", "41",
    "--build-manifest",
    r"D:\Projects\reviews\recovery-2026-09-07\contract-audit\grouped-build-bb3cf264\build.json",
    "--out", r"D:\mx\s41b5",
    "--cases", "heal",
]
Path(__file__).with_name("launch_s41b5.cmd.txt").write_text(" ".join(cmd) + "\n", encoding="utf-8")
print(" ".join(cmd))
