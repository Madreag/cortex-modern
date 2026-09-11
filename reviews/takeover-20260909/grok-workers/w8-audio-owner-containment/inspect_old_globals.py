from pathlib import Path
import json
import zipfile
import check_save

# one truncated p5snap from the priority list
inv = json.loads(Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w8-audio-owner-containment\save_inventory.json").read_text())
path = None
for rec in inv["saves"]:
    if rec["size"] == 3727661 and rec["path"].endswith(".ccsave"):
        path = Path(rec["path"])
        break
print("path", path)
with zipfile.ZipFile(path) as zf:
    ini = zf.read("Save.ini").decode("latin-1")
print("has RuntimeGlobals", "RuntimeGlobals" in ini)
idx = ini.find("RuntimeGlobals = ")
print("idx", idx)
if idx >= 0:
    line_end = ini.find("\n", idx)
    val = ini[idx + len("RuntimeGlobals = "):line_end].strip()
    text = check_save.decode_b64_text(val)
    print("decoded", len(text), "head", repr(text[:80]))
    for tag in ("RuntimeGlobals", "AudioRuntime", "GUISound", "MusicMan", "AudioVoice"):
        print(tag, text.find(tag), text.count(tag))
