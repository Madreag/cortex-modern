from pathlib import Path
import check_save

path = Path(r"D:\mx\s40lanes\h4\peers_3_4_regression_20260909_105714\drop3_host\runtime\Userdata\UserSavedGames.rte\p5resync_56264.ccsave")
ini, _ = check_save.load_save_ini(path)
props = check_save.extract_ini_props(ini)
for start, b64 in props["SoundCheckpoints"]:
    parsed = check_save.parse_sound_container_identity(check_save.decode_b64_text(b64))
    if parsed["identity"] != 9310:
        continue
    chunk = ini[max(0, start - 8000) : start + 200]
    # keep lines with class-ish tokens
    keep = []
    for line in chunk.splitlines():
        s = line.strip()
        if any(
            tok in s
            for tok in (
                "CopyOf",
                "PresetName",
                "GibSound",
                "HitSound",
                "Impact",
                "MOSRotating",
                "Actor",
                "AHuman",
                "HDFirearm",
                "AEmitter",
                "AddAttachable",
                "AddInventory",
                "Wound",
                "Metal Impact",
                "SpecialBehaviour_",
            )
        ):
            keep.append(s[:200])
    Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w8-audio-owner-containment\clue_9310_context.txt").write_text(
        "\n".join(keep[-40:]) + "\n", encoding="utf-8"
    )
    print("\n".join(keep[-40:]))
    break
