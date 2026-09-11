import re
import pathlib

files = [
    r"D:\Projects\p4b-interp-validation\Source\Network\GnsTransport.cpp",
    r"D:\Projects\reviews\claude-review-2026-09-08\lanes\source33-lanes\driver\h4gates\common.py",
    r"D:\Projects\p4b-interp-validation\tools\win32_test_runner.py",
    r"D:\Projects\reviews\takeover-20260909\a7-repaired-driver\a7_driver.py",
    r"D:\Projects\p4b-interp-validation\tools\h4_substitution_gates.py",
    r"D:\Projects\reviews\takeover-20260909\run_breadth.py",
    r"D:\Projects\stage2_p4\recovery_e2e.py",
    r"D:\Projects\stage2_p4\harness_common.ps1",
    r"D:\Projects\p4b-interp-validation\tools\run_sim_test.py",
]
gns = pathlib.Path(files[0]).read_text(encoding="utf-8", errors="replace").split("\n")
for i in range(98, 122):
    print(i + 1, gns[i][:150])
print("---- ports by file")
ports = set()
for f in files[1:]:
    p = pathlib.Path(f)
    if not p.exists():
        print("missing", f)
        continue
    text = p.read_text(encoding="utf-8", errors="replace")
    found = sorted({int(m) for m in re.findall(r"(?i)\bport\w*\W{1,4}(\d{4,5})\b", text)} |
                   {int(m) for m in re.findall(r"\b(4[0-9]{4})\b", text) if 40000 <= int(m) <= 49999})
    ports.update(found)
    print(p.name, found[:20], "..." if len(found) > 20 else "")
print("---- overall min/max:", min(ports) if ports else None, max(ports) if ports else None)
