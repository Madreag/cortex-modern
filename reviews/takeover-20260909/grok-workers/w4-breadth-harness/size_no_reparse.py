"""Sum file sizes under a run root without entering reparse points."""
from __future__ import annotations

import os
import stat
from pathlib import Path
import sys


def is_reparse(path: Path) -> bool:
    try:
        return bool(path.lstat().st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT)
    except OSError:
        return False


def walk(root: Path):
    total = 0
    files = 0
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        current = Path(dirpath)
        if is_reparse(current) and current != root:
            dirnames[:] = []
            continue
        dirnames[:] = [name for name in dirnames if not is_reparse(current / name)]
        for name in filenames:
            path = current / name
            if is_reparse(path):
                continue
            try:
                total += path.stat().st_size
                files += 1
            except OSError:
                pass
    return files, total


if __name__ == "__main__":
    root = Path(sys.argv[1])
    files, total = walk(root)
    print(f"{root} files={files} bytes={total} mb={total / (1024 * 1024):.1f}")
