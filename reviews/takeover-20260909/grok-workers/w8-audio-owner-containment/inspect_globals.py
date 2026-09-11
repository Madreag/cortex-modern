from pathlib import Path
import zipfile
import check_save

path = Path(r"D:\mx\s38\archive_loads\success\runtime\Userdata\UserSavedGames.rte\load_seed.ccsave")
with zipfile.ZipFile(path) as zf:
    ini = zf.read("Save.ini").decode("utf-8", errors="replace")
idx = ini.find("RuntimeGlobals = ")
line_end = ini.find("\n", idx)
val = ini[idx + len("RuntimeGlobals = ") : line_end].strip()
text = check_save.decode_b64_text(val)
print("decoded_len", len(text))
print("head", repr(text[:120]))
r = check_save.CheckpointReader(text)
ver = r.expect_version("RuntimeGlobals9")
print("version", ver)
for i in range(20):
    try:
        pos = r.i
        preview = r.remaining()[:40]
        s = r.read_string()
        print(f"str[{i}] pos={pos} len={len(s)} head={s[:60]!r}")
    except Exception as exc:
        print(f"str[{i}] FAIL at {r.i}: {exc} preview={preview!r}")
        # try int
        r.i = pos
        try:
            n = r.read_int()
            print(f"  as_int={n}")
        except Exception as exc2:
            print(f"  as_int fail {exc2}")
        break
