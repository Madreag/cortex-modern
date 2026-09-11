"""Compare 316e vs 60cb blob IDs for the 21 files. Read-only git."""
from __future__ import annotations

import json
import subprocess
from pathlib import Path

CCCP = Path(r"D:\Projects\cccp")
OUT = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w6-mac-base-provenance\mac-git-state\win-blob-compare.json")
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
MAC_HEAD_BLOBS = {
    "Source/Activities/GAScripted.h": "691faab90ab66aff3dc4017c84ecbd8bef13aead",
    "Source/Entities/Actor.cpp": "d96e5ef8505e940dfd056d6d7c9ea19e0d8ccaf0",
    "Source/Entities/Actor.h": "14286442643f797d2682e32224abfc866458288a",
    "Source/Entities/Gib.cpp": "2a3f7bda3f6b47780cbb8c66f404981ef3894913",
    "Source/Entities/GlobalScript.cpp": "a205cb7e06704f798a6c8e0ff9ef6ae7bfdd02cf",
    "Source/Entities/GlobalScript.h": "dd547b04631046e6540c1249f3443590fd0a17ac",
    "Source/GUI/GUIBanner.cpp": "eefb7fbbbf7ea2ec4f093b0015cd0ca22b900806",
    "Source/GUI/GUIBanner.h": "1d4144c2d7de7e337b1c9748dbc93da9179d76cb",
    "Source/GUI/GUIFont.h": "53cb07ed848d300a2197b009a3eb62d786b84a2c",
    "Source/GUI/GUIInput.cpp": "fb068ddd6f6d6b1358c0dfeda1e45660b105c4c9",
    "Source/Managers/PostProcessMan.cpp": "2992c2e662bae6bdb9e0077b8f5c21b15b81e644",
    "Source/Managers/PostProcessMan.h": "1bae2922eb7705261b2e9c3424c7481a97c87268",
    "Source/Managers/PrimitiveMan.h": "3bfa9dc48d2b332f8b4ee4d991bbda825796cb3a",
    "Source/Menus/BuyMenuGUI.h": "955da7afc3b9968bdf2e41a1b1df0e7ce2ee368b",
    "Source/Menus/InventoryMenuGUI.h": "8090877789d86a13b82c6523667a19c4d734de11",
    "Source/Renderer/GraphicalPrimitive.cpp": "9b9797c78d0b18024dc09ae270ce82dc79375abb",
    "Source/Renderer/GraphicalPrimitive.h": "4d366a90268047cb3111ea55b785fc57131edc74",
    "Source/System/ContractAudit.h": "207c3413245b7241d33eaee7aef9989ae576cc7d",
    "tools/contracts/AUDIT.md": "7faf3e7ad743d29ae2aa61ddc9f0e831a8614d8f",
    "tools/fixtures/mod_checkpoint.lua": "895e52e901ed303bddb5a3c74a18691a94fd8bcf",
    "tools/run_restoration_tests.py": "027951e047c11e8168f2a9e2b16488a22ddb8951",
}


def rev_parse(commit: str, path: str) -> str:
    r = subprocess.run(
        ["git", "-C", str(CCCP), "rev-parse", f"{commit}:{path}"],
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        return f"ERR:{r.stderr.strip()}"
    return r.stdout.strip()


rows = []
for path in FILES:
    b316e = rev_parse("316e96375702eff5b0118714dc4762a0ee6eebd7", path)
    b60 = rev_parse("60cb698146", path)
    rows.append(
        {
            "path": path,
            "mac_head_blob": MAC_HEAD_BLOBS[path],
            "win_316e_blob": b316e,
            "win_60cb_blob": b60,
            "mac_head_eq_win_316e": MAC_HEAD_BLOBS[path] == b316e,
            "win_316e_eq_60cb": b316e == b60,
        }
    )
    print(f"{path}: 316e={b316e[:12]} 60cb={b60[:12]} same={b316e==b60} mac_head_match={MAC_HEAD_BLOBS[path]==b316e}")

OUT.write_text(json.dumps(rows, indent=2), encoding="utf-8")
print("wrote", OUT)
