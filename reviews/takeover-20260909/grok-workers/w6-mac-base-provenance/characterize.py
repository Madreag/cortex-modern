"""One-line characterization of Mac-WT vs Mac-HEAD diffs, and parked-patch overlap."""
from __future__ import annotations

import json
import re
from pathlib import Path

BASE = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w6-mac-base-provenance")
DIFFS = BASE / "mac-diffs"
WIN_DIFFS = BASE / "win-vs-mac-diffs"
PARKED = Path(r"D:\Projects\native-fidelity-work\parked-20260908-tools-contracts\tools\contracts\primitive_checkpoint_pending.patch")
FILES = [
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


def added_lines(diff_text: str) -> list[str]:
    out = []
    for line in diff_text.splitlines():
        if line.startswith("+++") or line.startswith("---"):
            continue
        if line.startswith("+"):
            out.append(line[1:])
    return out


def keywords(lines: list[str]) -> list[str]:
    blob = "\n".join(lines)
    keys = [
        "SaveCheckpoint",
        "LoadCheckpoint",
        "ResolveCheckpoint",
        "RunCheckpointSelfTest",
        "SetAsideQueues",
        "ReinstateQueues",
        "ContractAudit",
        "GUICheckpoint",
        "NativeCheckpoint",
        "OwnBitmap",
        "OwnVertices",
        "m_ImageOwners",
        "m_VertexOwners",
        "m_SpriteOwner",
        "PrimitiveQueuesSetAside",
        "QueuesSetAside",
        "GetCheckpointPrimitive",
        "FindCheckpoint",
        "mod_checkpoint",
        "restoration",
        "PostProcess",
        "GUIBanner",
        "BuyMenu",
        "InventoryMenu",
        "GlobalScript",
        "GAScripted",
        "Actor",
    ]
    return [k for k in keys if k in blob]


parked_text = PARKED.read_text(encoding="utf-8", errors="replace")
parked_by_file: dict[str, set[str]] = {}
current = None
buf: list[str] = []
for line in parked_text.splitlines():
    m = re.match(r"^diff --git a/(.+) b/(.+)$", line)
    if m:
        if current is not None:
            parked_by_file[current] = set(added_lines("\n".join(buf)))
        current = m.group(2)
        buf = [line]
    else:
        buf.append(line)
if current is not None:
    parked_by_file[current] = set(added_lines("\n".join(buf)))

rows = []
for rel in FILES:
    mac_diff = (DIFFS / (rel.replace("/", "__") + ".diff")).read_text(encoding="utf-8", errors="replace")
    win_diff_path = WIN_DIFFS / (rel.replace("/", "__") + ".diff")
    win_diff = win_diff_path.read_text(encoding="utf-8", errors="replace") if win_diff_path.exists() else ""
    mac_added = added_lines(mac_diff)
    parked_added = parked_by_file.get(rel, set())
    mac_added_set = set(mac_added)
    if not parked_added:
        parked_verdict = "UNEXPLAINED"
        parked_note = "parked patch does not touch this path"
        overlap = 0
    else:
        overlap = len(parked_added & mac_added_set)
        if parked_added <= mac_added_set and overlap == len(parked_added):
            # all parked added lines appear in Mac-vs-HEAD; Mac may have more
            extra = len(mac_added_set - parked_added)
            parked_verdict = "EXPLAINED_BY_PARKED_PATCH" if extra == 0 else "PARTIAL"
            parked_note = f"parked_added_lines={len(parked_added)} overlap={overlap} mac_only_added_lines={extra}"
        elif overlap:
            parked_verdict = "PARTIAL"
            parked_note = f"parked_added_lines={len(parked_added)} overlap={overlap} mac_only_added_lines={len(mac_added_set - parked_added)}"
        else:
            parked_verdict = "UNEXPLAINED"
            parked_note = f"parked touches path but 0 added-line overlap; parked_added_lines={len(parked_added)}"
    # vs 60cb parked comparison: use win-vs-mac added lines
    win_added = added_lines(win_diff)
    if not win_added:
        vs60_verdict = "UNEXPLAINED"
        vs60_note = "win-vs-mac unified diff empty after strip-trailing-cr; parked hunks do not account for a content delta that is not present"
    elif not parked_added:
        vs60_verdict = "UNEXPLAINED"
        vs60_note = "parked patch does not touch this path"
    else:
        o2 = len(parked_added & set(win_added))
        if parked_added <= set(win_added) and o2 == len(parked_added) and len(set(win_added) - parked_added) == 0:
            vs60_verdict = "EXPLAINED_BY_PARKED_PATCH"
            vs60_note = f"overlap={o2}"
        elif o2:
            vs60_verdict = "PARTIAL"
            vs60_note = f"overlap={o2} parked={len(parked_added)} win_added={len(win_added)}"
        else:
            vs60_verdict = "UNEXPLAINED"
            vs60_note = f"overlap=0 parked={len(parked_added)} win_added={len(win_added)}"

    rows.append(
        {
            "path": rel,
            "mac_vs_head_added_lines": len(mac_added),
            "mac_vs_head_keywords": keywords(mac_added),
            "parked_vs_mac_head": parked_verdict,
            "parked_vs_mac_head_note": parked_note,
            "parked_vs_60cb": vs60_verdict,
            "parked_vs_60cb_note": vs60_note,
            "win_vs_mac_diff_bytes": len(win_diff.encode("utf-8")),
        }
    )
    print(f"{rel}: keywords={rows[-1]['mac_vs_head_keywords']} parked_vs_head={parked_verdict} parked_vs_60cb={vs60_verdict}")

(BASE / "characterization.json").write_text(json.dumps(rows, indent=2), encoding="utf-8")
print("wrote characterization.json")
