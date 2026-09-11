"""Count Mac porcelain classes from saved status output."""
from pathlib import Path
import json

p = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w6-mac-base-provenance\mac-git-state\mac_collect_stdout.txt")
text = p.read_text(encoding="utf-8")
start = text.index("===== STATUS_PORCELAIN =====\n") + len("===== STATUS_PORCELAIN =====\n")
end = text.index("===== LOG_ONELINE_5 =====")
body = text[start:end]
rows = [ln for ln in body.splitlines() if ln.strip()]
kinds = {}
twenty_one = [
    "Source/Activities/GAScripted.h",
    "Source/Entities/Actor.cpp",
    "Source/Entities/Actor.h",
    "Source/Entities/Gib.cpp",
    "Source/Entities/GlobalScript.cpp",
    "Source/Entities/GlobalScript.h",
    "Source/GUI/GUIBanner.cpp",
    "Source/GUI/GUIBanner.h",
    "Source/GUI/GUIFont.h",
    "Source/GUI/GUIInput.cpp",
    "Source/Managers/PostProcessMan.cpp",
    "Source/Managers/PostProcessMan.h",
    "Source/Managers/PrimitiveMan.h",
    "Source/Menus/BuyMenuGUI.h",
    "Source/Menus/InventoryMenuGUI.h",
    "Source/Renderer/GraphicalPrimitive.cpp",
    "Source/Renderer/GraphicalPrimitive.h",
    "Source/System/ContractAudit.h",
    "tools/contracts/AUDIT.md",
    "tools/fixtures/mod_checkpoint.lua",
    "tools/run_restoration_tests.py",
]
present = []
for ln in rows:
    code = ln[:2]
    path = ln[3:]
    kinds[code] = kinds.get(code, 0) + 1
    if path in twenty_one:
        present.append({"status": code, "path": path})
out = {
    "porcelain_lines": len(rows),
    "by_code": kinds,
    "twenty_one_in_status": present,
    "twenty_one_count": len(present),
}
dest = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w6-mac-base-provenance\mac-git-state\status-counts.json")
dest.write_text(json.dumps(out, indent=2), encoding="utf-8")
print(json.dumps(out, indent=2))
