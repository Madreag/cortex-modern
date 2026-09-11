"""Dump finding A/B sites at c8f8188ae0. Read-only git show."""
from __future__ import annotations

import subprocess
from pathlib import Path

REPO = Path(r"D:\Projects\p4b-interp-validation")
OUT = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w10-h4-gates")
COMMIT = "c8f8188ae0"


def show(path: str, start: int, end: int) -> str:
    blob = subprocess.check_output(
        ["git", "-C", str(REPO), "show", f"{COMMIT}:{path}"],
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    lines = blob.splitlines()
    chunk = []
    for i in range(start, min(end, len(lines)) + 1):
        chunk.append(f"{i}:{lines[i - 1]}")
    return "\n".join(chunk)


def main() -> None:
    text = []
    text.append(f"commit {COMMIT}")
    text.append("===== NetMatchService.cpp PumpSessionEvents / AdmissionNowMs =====")
    text.append(show("Source/Network/NetMatchService.cpp", 608, 612))
    text.append(show("Source/Network/NetMatchService.cpp", 718, 755))
    text.append("===== NetSession.cpp TickAdmissionPlane / ExpireSilent =====")
    text.append(show("Source/Network/NetSession.cpp", 166, 175))
    text.append(show("Source/Network/NetSession.cpp", 762, 775))
    text.append("===== NetAdmissionClock =====")
    text.append(show("Source/Network/NetReconnectSession.h", 45, 55))
    dest = OUT / "c8f8188ae0_findings.txt"
    dest.write_text("\n".join(text), encoding="utf-8")
    print("wrote", dest)


if __name__ == "__main__":
    main()
