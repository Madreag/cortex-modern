from pathlib import Path
import check_save
import re

path = Path(r"D:\mx\s40lanes\h4\peers_3_4_regression_20260909_105714\drop3_host\runtime\Userdata\UserSavedGames.rte\p5resync_56264.ccsave")
ini, _ = check_save.load_save_ini(path)
props = check_save.extract_ini_props(ini)
for start, b64 in props["SoundCheckpoints"]:
    parsed = check_save.parse_sound_container_identity(check_save.decode_b64_text(b64))
    if parsed["identity"] != 9310:
        continue
    chunk = ini[max(0, start - 20000) : start]
    # last non-indented or class assignment before this sound
    lines = chunk.splitlines()
    interesting = []
    for line in lines:
        s = line.strip()
        if re.match(r"^[A-Za-z].*=\s*[A-Za-z]", s) and "Content" not in s and "FilePath" not in s and "SpecialBehaviour_Content" not in s:
            interesting.append(s[:180])
    Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w8-audio-owner-containment\clue_9310_parent.txt").write_text(
        "\n".join(interesting[-25:]) + "\n", encoding="utf-8"
    )
    print("\n".join(interesting[-25:]))
    break
