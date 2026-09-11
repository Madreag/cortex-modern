"""Read-only evidence extract for W10. Writes only under this folder."""
from __future__ import annotations

import json
from pathlib import Path

OUT = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w10-h4-gates")
JOBS = {
    "clean_leave": Path(r"D:\mx\s41b3\j33"),
    "reclaim_socket": Path(r"D:\mx\s41b3\j34"),
    "fencing_two_transports": Path(r"D:\mx\s41b3\j36"),
    "rejoin_after_resync": Path(r"D:\mx\s41b3\j37"),
}


SKIP_DIRS = {"data", "external", "modules"}
FILE_ATTR_REPARSE = 0x400


def is_reparse(path: Path) -> bool:
    try:
        return bool(path.stat().st_file_attributes & FILE_ATTR_REPARSE)
    except OSError:
        return False


def walk(root: Path):
    if not root.exists():
        return
    stack = [root]
    while stack:
        cur = stack.pop()
        try:
            children = list(cur.iterdir())
        except OSError as exc:
            yield cur, f"listdir_error {exc}"
            continue
        for p in children:
            name = p.name.lower()
            if p.is_dir() and (name in SKIP_DIRS or is_reparse(p)):
                continue
            yield p, None
            if p.is_dir():
                stack.append(p)


def main() -> None:
    listing = []
    for name, root in JOBS.items():
        listing.append(f"=== {name} {root} exists={root.exists()} ===")
        if not root.exists():
            continue
        for p in sorted(root.iterdir(), key=lambda x: x.name.lower()):
            listing.append(f"  {p.name} dir={p.is_dir()} reparse={bool(p.stat().st_file_attributes & 0x400) if hasattr(p.stat(), 'st_file_attributes') else '?'}")
        # find verdict.json without entering Data
        for p, err in walk(root):
            if err:
                listing.append(f"ERR {p} {err}")
                continue
            if p.is_file() and (
                p.name
                in {
                    "verdict.json",
                    "stdout.log",
                    "LogConsole.txt",
                    "launch.json",
                }
                or p.name.endswith("_report.json")
            ):
                listing.append(f"FILE {p} bytes={p.stat().st_size}")
    (OUT / "job_listing.txt").write_text("\n".join(listing), encoding="utf-8")
    print("wrote", OUT / "job_listing.txt", "lines", len(listing))


if __name__ == "__main__":
    main()
