"""Copy quoted evidence files into w10-h4-gates/quotes. Read-only on D:\\mx."""
from __future__ import annotations

import json
import shutil
from pathlib import Path

OUT = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w10-h4-gates")
QUOTES = OUT / "quotes"
QUOTES.mkdir(exist_ok=True)

FILES = [
    Path(r"D:\mx\s41b3\j33\clean_leave_20260910_021812\verdict.json"),
    Path(r"D:\mx\s41b3\j33\clean_leave_20260910_021812\clean_stale_report.json"),
    Path(r"D:\mx\s41b3\j33\clean_leave_20260910_021812\clean_client_report.json"),
    Path(r"D:\mx\s41b3\j33\clean_leave_20260910_021812\clean_host_report.json"),
    Path(r"D:\mx\s41b3\j33\clean_leave_20260910_021812\clean_host2_report.json"),
    Path(r"D:\mx\s41b3\j33\clean_leave_20260910_021812\ambiguous_host_report.json"),
    Path(r"D:\mx\s41b3\j33\clean_leave_20260910_021812\ambiguous_client2_report.json"),
    Path(r"D:\mx\s41b3\j34\reclaim_socket_20260910_021916\verdict.json"),
    Path(r"D:\mx\s41b3\j34\reclaim_socket_20260910_021916\host_report.json"),
    Path(r"D:\mx\s41b3\j34\reclaim_socket_20260910_021916\client2_report.json"),
    Path(r"D:\mx\s41b3\j36\fencing_two_transports_20260910_022107\verdict.json"),
    Path(r"D:\mx\s41b3\j36\fencing_two_transports_20260910_022107\host_report.json"),
    Path(r"D:\mx\s41b3\j36\fencing_two_transports_20260910_022107\client1_report.json"),
    Path(r"D:\mx\s41b3\j36\fencing_two_transports_20260910_022107\client2_report.json"),
    Path(r"D:\mx\s41b3\j37\rejoin_after_resync_20260910_022132\verdict.json"),
    Path(r"D:\mx\s41b3\j37\rejoin_after_resync_20260910_022132\resync_host_report.json"),
    Path(r"D:\mx\s41b3\j37\rejoin_after_resync_20260910_022132\resync_client2_report.json"),
    Path(r"D:\mx\s41b3\j37\rejoin_after_resync_20260910_022132\rematch_host_report.json"),
]


def copy_text(src: Path, dest_name: str) -> None:
    dest = QUOTES / dest_name
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(src.read_text(encoding="utf-8-sig", errors="replace"), encoding="utf-8")


def main() -> None:
    listing = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w10-h4-gates\job_listing.txt")
    copied = 0
    for line in listing.read_text(encoding="utf-8").splitlines():
        if not line.startswith("FILE "):
            continue
        path = Path(line.split(" bytes=")[0][5:])
        rel = path.relative_to(Path(r"D:\mx\s41b3"))
        dest = QUOTES / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(path.read_bytes())
        copied += 1
    print("copied", copied)


if __name__ == "__main__":
    main()
