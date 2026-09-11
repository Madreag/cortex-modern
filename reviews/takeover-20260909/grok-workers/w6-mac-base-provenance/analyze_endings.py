"""Measure why Mac files are larger than 60cb blobs while strip-cr diffs are empty."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path

BASE = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w6-mac-base-provenance")
MAC = BASE / "mac-files"
WIN = BASE / "win-blobs-60cb698146"
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


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def ending_stats(data: bytes) -> dict:
    crlf = data.count(b"\r\n")
    # lone CR: CR not followed by LF. Count CRs then subtract those in CRLF.
    cr_total = data.count(b"\r")
    lf_total = data.count(b"\n")
    lone_cr = cr_total - crlf
    lone_lf = lf_total - crlf
    return {
        "bytes": len(data),
        "crlf": crlf,
        "lone_cr": lone_cr,
        "lone_lf": lone_lf,
        "cr_total": cr_total,
        "lf_total": lf_total,
        "sha256": sha256(data),
    }


def to_lf(data: bytes) -> bytes:
    return data.replace(b"\r\n", b"\n").replace(b"\r", b"\n")


def to_crlf(data: bytes) -> bytes:
    return to_lf(data).replace(b"\n", b"\r\n")


def line_pairs(a: bytes, b: bytes) -> dict:
    """Compare after splitting on LF, stripping one trailing CR per line."""
    def lines(data: bytes) -> list[bytes]:
        # keep last empty if file ends with newline
        parts = data.split(b"\n")
        return [p[:-1] if p.endswith(b"\r") else p for p in parts]

    la, lb = lines(a), lines(b)
    if la == lb:
        return {"strip_cr_lines_equal": True, "line_count_a": len(la), "line_count_b": len(lb)}
    diffs = []
    n = max(len(la), len(lb))
    for i in range(n):
        sa = la[i] if i < len(la) else None
        sb = lb[i] if i < len(lb) else None
        if sa != sb:
            diffs.append(
                {
                    "i": i,
                    "win_len": None if sa is None else len(sa),
                    "mac_len": None if sb is None else len(sb),
                    "win_head": None if sa is None else sa[:80].decode("utf-8", "replace"),
                    "mac_head": None if sb is None else sb[:80].decode("utf-8", "replace"),
                }
            )
            if len(diffs) >= 8:
                break
    return {
        "strip_cr_lines_equal": False,
        "line_count_a": len(la),
        "line_count_b": len(lb),
        "first_diffs": diffs,
    }


rows = []
for rel in FILES:
    mac = (MAC / rel).read_bytes()
    win = (WIN / rel).read_bytes()
    mac_lf = to_lf(mac)
    win_lf = to_lf(win)
    win_to_crlf = to_crlf(win)
    mac_to_crlf = to_crlf(mac)
    win_to_lf = win_lf
    row = {
        "path": rel,
        "mac": ending_stats(mac),
        "win": ending_stats(win),
        "byte_delta": len(mac) - len(win),
        "cr_delta": mac.count(b"\r") - win.count(b"\r"),
        "lf_normalized_equal": mac_lf == win_lf,
        "win_crlf_equals_mac": win_to_crlf == mac,
        "mac_crlf_equals_win": mac_to_crlf == win,
        "win_lf_equals_mac": win_to_lf == mac,
        "mac_lf_equals_win": mac_lf == win,
        "lf_norm_sha_mac": sha256(mac_lf),
        "lf_norm_sha_win": sha256(win_lf),
        "lines": line_pairs(win, mac),
    }
    rows.append(row)
    print(
        f"{rel}: delta={row['byte_delta']} cr_delta={row['cr_delta']} "
        f"lf_eq={row['lf_normalized_equal']} win_crlf==mac={row['win_crlf_equals_mac']} "
        f"strip_lines={row['lines']['strip_cr_lines_equal']} "
        f"mac_crlf={row['mac']['crlf']} mac_lf={row['mac']['lone_lf']} "
        f"win_crlf={row['win']['crlf']} win_lf={row['win']['lone_lf']}"
    )

out = BASE / "ending-analysis.json"
out.write_text(json.dumps(rows, indent=2), encoding="utf-8")
print("wrote", out)
