"""Write LF-only follow-up Mac read-only commands."""
from pathlib import Path

out = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w6-mac-base-provenance\mac_more.sh")
text = r"""#!/bin/zsh
set -u
REPO="/Users/erol/Documents/Codex/cortex-b2-review-20260909/positive"
echo "===== CAT_FILE_60cb698146 ====="
git -C "$REPO" cat-file -t 60cb698146 2>&1; echo "EXIT:$?"
echo "===== LOG_1_60cb698146 ====="
git -C "$REPO" log --oneline -1 60cb698146 2>&1; echo "EXIT:$?"
echo "===== MERGE_BASE ====="
git -C "$REPO" merge-base HEAD 60cb698146 2>&1; echo "EXIT:$?"
echo "===== LOG_60cb_TO_HEAD ====="
git -C "$REPO" log --oneline 60cb698146..HEAD 2>&1; echo "EXIT:$?"
echo "===== LOG_HEAD_TO_60cb ====="
git -C "$REPO" log --oneline HEAD..60cb698146 2>&1; echo "EXIT:$?"
echo "===== REV_PARSE_FULL ====="
git -C "$REPO" rev-parse HEAD
git -C "$REPO" rev-parse --abbrev-ref HEAD
echo "===== SHOW_HEAD ====="
git -C "$REPO" log -1 --format=fuller HEAD
echo "===== STATUS_COUNT ====="
git -C "$REPO" status --porcelain | wc -l
echo "===== STATUS_21 ====="
for p in \
  "Source/Activities/GAScripted.h" \
  "Source/Entities/Actor.cpp" \
  "Source/Entities/Actor.h" \
  "Source/Entities/Gib.cpp" \
  "Source/Entities/GlobalScript.cpp" \
  "Source/Entities/GlobalScript.h" \
  "Source/GUI/GUIBanner.cpp" \
  "Source/GUI/GUIBanner.h" \
  "Source/GUI/GUIFont.h" \
  "Source/GUI/GUIInput.cpp" \
  "Source/Managers/PostProcessMan.cpp" \
  "Source/Managers/PostProcessMan.h" \
  "Source/Managers/PrimitiveMan.h" \
  "Source/Menus/BuyMenuGUI.h" \
  "Source/Menus/InventoryMenuGUI.h" \
  "Source/Renderer/GraphicalPrimitive.cpp" \
  "Source/Renderer/GraphicalPrimitive.h" \
  "Source/System/ContractAudit.h" \
  "tools/contracts/AUDIT.md" \
  "tools/fixtures/mod_checkpoint.lua" \
  "tools/run_restoration_tests.py"
do
  echo "----- STATUS $p -----"
  git -C "$REPO" status --porcelain -- "$p"
  echo -n "WT_SHA256="
  shasum -a 256 "$REPO/$p" | awk '{print $1}'
  echo -n "HEAD_BLOB="
  git -C "$REPO" rev-parse "HEAD:$p" 2>&1
  echo -n "INDEX_BLOB="
  git -C "$REPO" rev-parse ":$p" 2>&1
done
echo "===== END ====="
"""
out.write_bytes(text.encode("utf-8").replace(b"\r\n", b"\n").replace(b"\r", b"\n"))
print("wrote", out, "bytes", out.stat().st_size)
