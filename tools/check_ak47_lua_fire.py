"""Quote OnFire script-call and Mech Ronin AK-47 voice lines from a replay stdout.

    python tools/check_ak47_lua_fire.py D:/mx/w90/some/stdout.log
"""

import re
import sys
from pathlib import Path


def main() -> int:
    path = Path(sys.argv[1])
    text = path.read_text(encoding="utf-8", errors="replace")
    on_fire = [line for line in text.splitlines() if "[script-call]" in line and "function=OnFire" in line]
    mech = [line for line in text.splitlines() if "[preview-event] voice" in line and "Mech Ronin AK-47" in line]
    selftest = [line for line in text.splitlines() if "the_lua_fire_sound_starts_on_the_preview_tick" in line or "[preview-event-selftest]" in line]
    print(f"on_fire={len(on_fire)} mech={len(mech)}")
    for line in on_fire[:12]:
        print(line)
    if on_fire:
        print("---")
    for line in mech[:4]:
        print(line)
    if mech:
        print(f"... last mech: {mech[-1]}")
    for line in selftest:
        print(line)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
