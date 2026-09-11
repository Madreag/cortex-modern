import hashlib
from pathlib import Path

exe = Path(r"D:\Projects\takeover-build\Cortex Command.exe")
st = exe.stat()
with exe.open("rb") as f:
    digest = hashlib.file_digest(f, "sha256").hexdigest()
print(f"path={exe}")
print(f"bytes={st.st_size}")
print(f"sha256={digest}")
print(f"mtime={st.st_mtime}")
