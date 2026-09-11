import os
from pathlib import Path

REPARSE = 0x400
root = Path(r"D:\mx\w12")
total = 0
files = 0
skipped_dirs = []
for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
    p = Path(dirpath)
    keep = []
    for d in dirnames:
        child = p / d
        try:
            attrs = os.stat(child, follow_symlinks=False).st_file_attributes
        except OSError:
            continue
        if attrs & REPARSE:
            skipped_dirs.append(str(child))
            continue
        keep.append(d)
    dirnames[:] = keep
    for name in filenames:
        fp = p / name
        try:
            st = os.stat(fp, follow_symlinks=False)
            if st.st_file_attributes & REPARSE:
                continue
            total += st.st_size
            files += 1
        except OSError:
            pass
print(f"files={files}")
print(f"bytes={total}")
print(f"mib={total/1024/1024:.2f}")
print(f"skipped_reparse_dirs={len(skipped_dirs)}")
for s in skipped_dirs:
    print(f"skip {s}")
