from pathlib import Path
import zipfile
import re

path = Path(r"D:\mx\s38\archive_loads\success\runtime\Userdata\UserSavedGames.rte\load_seed.ccsave")
with zipfile.ZipFile(path) as zf:
    print("entries", zf.namelist())
    data = zf.read("Save.ini")
print("ini_bytes", len(data))
# show first 2k
print("HEAD", data[:400])
# find RuntimeGlobals
idx = data.find(b"RuntimeGlobals")
print("RuntimeGlobals idx", idx)
if idx >= 0:
    chunk = data[idx : idx + 200]
    print("around", chunk)
    # find end of this property line
    nl = data.find(b"\n", idx)
    print("line_len", nl - idx if nl >= 0 else -1)
    line = data[idx:nl] if nl >= 0 else data[idx : idx + 200]
    print("line_start", line[:80])
    print("line_end", line[-40:])
    eq = line.find(b"=")
    val = line[eq + 1 :].strip()
    print("val_len", len(val), "first20", val[:20], "last20", val[-20:])
    print("val_charset_sample", set(val[:200]))
