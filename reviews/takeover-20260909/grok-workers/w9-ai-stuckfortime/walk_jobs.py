"""Reparse-safe walk of heal_global_d0/d3 job trees. Writes only under this folder."""
from __future__ import annotations

import hashlib
import json
import os
import stat
from pathlib import Path

OUT = Path(__file__).resolve().parent
SKIP_DIR_NAMES = {"data", "external", "modules"}
JOB_NAMES = ("heal_global_d0", "heal_global_d3")
ROOTS = [Path(r"D:\mx\s41"), Path(r"D:\mx\s41r2"), Path(r"D:\mx\s41b3"), Path(r"D:\mx\s41b4")]
MAX_BYTES = 20 * 1024 * 1024
TEXT_SUFFIXES = {".json", ".log", ".txt"}


def is_reparse(path: Path) -> bool:
    try:
        return bool(os.lstat(path).st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT)
    except OSError:
        return False


def walk_files(root: Path):
    if not root.exists() or is_reparse(root):
        return
    stack = [root]
    while stack:
        current = stack.pop()
        try:
            entries = list(os.scandir(current))
        except OSError:
            continue
        for entry in entries:
            name = entry.name
            try:
                if entry.is_symlink() or is_reparse(Path(entry.path)):
                    continue
                if entry.is_dir(follow_symlinks=False):
                    if name.lower() in SKIP_DIR_NAMES:
                        continue
                    stack.append(Path(entry.path))
                elif entry.is_file(follow_symlinks=False):
                    yield Path(entry.path)
            except OSError:
                continue


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> None:
    found_jobs = {}
    for root in ROOTS:
        if not root.exists() or is_reparse(root):
            found_jobs.setdefault("_roots", []).append({"path": str(root), "exists": root.exists(), "reparse": is_reparse(root)})
            continue
        found_jobs.setdefault("_roots", []).append({"path": str(root), "exists": True, "reparse": False})
        try:
            for entry in os.scandir(root):
                if entry.name in JOB_NAMES and entry.is_dir(follow_symlinks=False) and not is_reparse(Path(entry.path)):
                    found_jobs.setdefault(entry.name, []).append(str(Path(entry.path)))
        except OSError as exc:
            found_jobs.setdefault("_errors", []).append(f"{root}: {exc}")

    inventory = []
    extracts = {}
    keywords = (
        "AI_StuckForTime",
        "resync",
        "Resync",
        "desync",
        "Desync",
        "perturb",
        "1048653",
        "Green Dummy",
        "simdump",
        "heal",
    )
    for name in JOB_NAMES:
        for job_root in found_jobs.get(name, []):
            job_path = Path(job_root)
            for path in walk_files(job_path):
                suffix = path.suffix.lower()
                if suffix not in TEXT_SUFFIXES:
                    continue
                try:
                    size = path.stat().st_size
                except OSError:
                    continue
                if size > MAX_BYTES:
                    continue
                rel = str(path)
                item = {"path": rel, "bytes": size, "sha256": sha256_file(path)}
                inventory.append(item)
                if size == 0:
                    continue
                try:
                    text = path.read_text(encoding="utf-8", errors="replace")
                except OSError:
                    continue
                hits = []
                for i, line in enumerate(text.splitlines(), 1):
                    if any(key in line for key in keywords):
                        hits.append({"line": i, "text": line[:2000]})
                if hits:
                    extracts[rel] = hits

    (OUT / "job_roots.json").write_text(json.dumps(found_jobs, indent=2), encoding="utf-8")
    (OUT / "inventory.json").write_text(json.dumps(inventory, indent=2), encoding="utf-8")
    (OUT / "extracts.json").write_text(json.dumps(extracts, indent=2), encoding="utf-8")
    print(f"jobs={ {k: v for k, v in found_jobs.items() if k != '_roots'} }")
    print(f"inventory={len(inventory)} extracts={len(extracts)}")


if __name__ == "__main__":
    main()
