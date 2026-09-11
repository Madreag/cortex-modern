"""Record the exact wrapper invocation for s41b4. The caller runs the command."""
from pathlib import Path

cmd = [
    "python", "-B",
    r"D:\Projects\reviews\takeover-20260909\run_breadth.py",
    "--source", "41",
    "--build-manifest",
    r"D:\Projects\reviews\recovery-2026-09-07\contract-audit\grouped-build-bb3cf264\build.json",
    "--out", r"D:\mx\s41b4",
    "--cases",
    "heal",
    "compat_deferral_source22",
    "compat_deferral_approved",
    "compat_extra_source22",
    "compat_extra_approved",
    "fl200",
    "fl100",
]
Path(__file__).with_name("launch_s41b4.cmd.txt").write_text(" ".join(cmd) + "\n", encoding="utf-8")
print(" ".join(cmd))
