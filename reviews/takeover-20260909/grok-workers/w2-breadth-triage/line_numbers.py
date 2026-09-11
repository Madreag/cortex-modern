"""Record exact line numbers for quotes used in TRIAGE.md."""
from pathlib import Path

OUT = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w2-breadth-triage")
needles_by_file = {
    r"D:\mx\s41b3\breadth.json": [
        "host: auto delay differs from 14",
        "client: auto delay differs from 14",
        "compat output differs from Source22; no UID exclusions",
        "ModuleNotFoundError: No module named 'recovery_e2e'",
        "B1 running activity is not evidenced",
    ],
    r"D:\mx\s41b3\logs\fl200.log": ["fl200 PASS"],
    r"D:\mx\s41b3\j28\fl\fl200\host\stdout.log": ["auto input delay"],
    r"D:\mx\s41b3\j28\fl\fl200\client_report.json": ['"2": 13', '"2":13'],
    r"D:\mx\s41b3\logs\heal.log": ["ModuleNotFoundError"],
    r"D:\Projects\reviews\takeover-20260909\run_breadth.py": [
        "auto delay differs from",
        "no UID exclusions",
        "import recovery_e2e",
        "else STAGE / \"recovery_expanded_mod.py\"",
        "module.heal(out)",
        "ERRORS = re.compile",
    ],
    r"D:\Projects\stage2_p4\recovery_expanded_mod.py": ["import recovery_e2e"],
    r"D:\Projects\p4b-interp-validation\Source\Network\NetMatchRunner.cpp": [
        "autoInputDelay",
        "GetPeerPingMs",
        "auto input delay",
    ],
    r"D:\Projects\p4b-interp-validation\Source\Network\GnsTransport.cpp": [
        "m_nPing",
    ],
    r"D:\Projects\p4b-interp-validation\Source\Managers\MovableMan.cpp": [
        "Rejected a ",
    ],
}

rows = []
for path, needles in needles_by_file.items():
    p = Path(path)
    if not p.exists():
        rows.append(f"MISSING {path}")
        continue
    text = p.read_text(encoding="utf-8", errors="replace").splitlines()
    for n in needles:
        found = False
        for i, line in enumerate(text, 1):
            if n in line:
                rows.append(f"{path}:{i}:{line[:240]}")
                found = True
                if n in ("Rejected a ",):
                    continue
                break
        if not found:
            rows.append(f"NOTFOUND {path} :: {n}")

(OUT / "quote_line_numbers.txt").write_text("\n".join(rows), encoding="utf-8")
print("\n".join(rows[:80]))
print("total", len(rows))
