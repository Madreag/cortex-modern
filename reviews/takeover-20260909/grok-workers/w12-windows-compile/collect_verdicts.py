import json
from pathlib import Path

root = Path(r"D:\mx\w12")
out = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w12-windows-compile")
names = [
    "net-protocol",
    "net-identity",
    "net-session",
    "net-lockstep",
    "net-match",
    "net-auth",
    "net-admission",
    "net-reconnect",
    "net-reconnect-session",
    "net-discovery",
    "controller-frame",
    "native-graph",
]
summary = []
for name in names:
    log = root / name / "stdout.log"
    launch = root / name / "launch.json"
    lines = []
    fails = []
    if log.exists():
        for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
            if "PASS" in line or "FAIL" in line:
                lines.append(line)
            if "FAIL" in line:
                fails.append(line)
    rec = {}
    if launch.exists():
        rec = json.loads(launch.read_text(encoding="utf-8"))
    summary.append(
        {
            "name": name,
            "path": str(root / name),
            "stdout_log": str(log) if log.exists() else None,
            "engine_exit": rec.get("exit_code"),
            "timed_out": rec.get("timed_out"),
            "fail_lines": fails,
            "verdict_count": len(lines),
            "last_verdicts": lines[-12:],
        }
    )
    (out / f"verdicts-{name}.txt").write_text("\n".join(lines) + ("\n" if lines else ""), encoding="utf-8")
(out / "selftest-summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
print(json.dumps([{k: r[k] for k in ("name", "engine_exit", "timed_out", "verdict_count", "fail_lines")} for r in summary], indent=2))
