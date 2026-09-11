"""SCP 21 Mac files and diff them against Windows 60cb698146 blobs."""
from __future__ import annotations

import hashlib
import json
import subprocess
import sys
from pathlib import Path

BASE = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w6-mac-base-provenance")
MAC_FILES = BASE / "mac-files"
WIN_BLOBS = BASE / "win-blobs-60cb698146"
WIN_DIFFS = BASE / "win-vs-mac-diffs"
CCCP = Path(r"D:\Projects\cccp")
COMMIT = "60cb698146"
MAC_REPO = "/Users/erol/Documents/Codex/cortex-b2-review-20260909/positive"
DIFF_EXE = Path(r"C:\Program Files\Git\usr\bin\diff.exe")
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


def unified_diff(win_bytes: bytes, mac_bytes: bytes, rel: str) -> bytes:
    def norm_lines(data: bytes) -> list[str]:
        text = data.decode("utf-8", "replace")
        text = text.replace("\r\n", "\n").replace("\r", "\n")
        return text.splitlines(keepends=True)

    a = norm_lines(win_bytes)
    b = norm_lines(mac_bytes)
    if DIFF_EXE.exists():
        # Write temp already exist as files; caller uses them.
        return b""
    import difflib

    out = list(
        difflib.unified_diff(
            a,
            b,
            fromfile=f"60cb698146:{rel}",
            tofile=f"mac-wt:{rel}",
            n=3,
        )
    )
    return "".join(out).encode("utf-8")


def main() -> int:
    MAC_FILES.mkdir(parents=True, exist_ok=True)
    WIN_BLOBS.mkdir(parents=True, exist_ok=True)
    WIN_DIFFS.mkdir(parents=True, exist_ok=True)
    summary = []
    for rel in FILES:
        dest = MAC_FILES / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        if not dest.exists() or dest.stat().st_size == 0:
            src = f"Erol-Mac:{MAC_REPO}/{rel}"
            print(f"SCP {rel}", flush=True)
            r = subprocess.run(
                ["scp", "-o", "BatchMode=yes", "-o", "ConnectTimeout=30", src, str(dest)],
                capture_output=True,
            )
            if r.returncode != 0:
                summary.append({"path": rel, "scp_ok": False, "stderr": r.stderr.decode("utf-8", "replace")})
                print("SCP FAIL", rel, r.stderr.decode("utf-8", "replace"), flush=True)
                continue
        else:
            print(f"SCP cached {rel}", flush=True)
        mac_bytes = dest.read_bytes()
        blob = subprocess.run(
            ["git", "-C", str(CCCP), "show", f"{COMMIT}:{rel}"],
            capture_output=True,
        )
        win_path = WIN_BLOBS / rel
        win_path.parent.mkdir(parents=True, exist_ok=True)
        if blob.returncode != 0:
            summary.append({"path": rel, "scp_ok": True, "git_show_ok": False, "stderr": blob.stderr.decode("utf-8", "replace")})
            continue
        win_path.write_bytes(blob.stdout)
        diff_out = WIN_DIFFS / (rel.replace("/", "__") + ".diff")
        if DIFF_EXE.exists():
            d = subprocess.run(
                [str(DIFF_EXE), "-u", "--strip-trailing-cr", str(win_path), str(dest)],
                capture_output=True,
            )
            diff_out.write_bytes(d.stdout)
            diff_exit = d.returncode
            diff_bytes = len(d.stdout)
            if d.stderr:
                (WIN_DIFFS / (rel.replace("/", "__") + ".diff.err")).write_bytes(d.stderr)
        else:
            data = unified_diff(blob.stdout, mac_bytes, rel)
            diff_out.write_bytes(data)
            diff_exit = 0 if not data else 1
            diff_bytes = len(data)
        summary.append(
            {
                "path": rel,
                "scp_ok": True,
                "git_show_ok": True,
                "mac_bytes": len(mac_bytes),
                "win_bytes": len(blob.stdout),
                "byte_delta": len(mac_bytes) - len(blob.stdout),
                "mac_sha256": sha256(mac_bytes),
                "win_sha256": sha256(blob.stdout),
                "diff_exit": diff_exit,
                "diff_bytes": diff_bytes,
                "diff_path": str(diff_out),
            }
        )
        print(
            f"OK {rel} mac={len(mac_bytes)} win={len(blob.stdout)} delta={len(mac_bytes)-len(blob.stdout)} diff_exit={diff_exit} diff_bytes={diff_bytes}",
            flush=True,
        )
    (BASE / "win-vs-mac-summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print("wrote summary", len(summary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
