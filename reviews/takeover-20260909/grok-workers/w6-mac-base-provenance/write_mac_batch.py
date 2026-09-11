"""Write LF-only Mac read-only follow-up scripts."""
from pathlib import Path

base = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w6-mac-base-provenance")

more = r"""#!/bin/zsh
set -u
REPO="/Users/erol/Documents/Codex/cortex-b2-review-20260909/positive"
CTRL="/Users/erol/Documents/Codex/cortex-b2-review-20260909/control"
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
echo "===== SHOW_HEAD ====="
git -C "$REPO" log -1 --format=fuller HEAD
echo "===== STATUS_COUNT ====="
git -C "$REPO" status --porcelain | wc -l
echo "===== CONTROL_REPO ====="
if [ -d "$CTRL/.git" ] || [ -f "$CTRL/.git" ]; then
  echo -n "CONTROL_HEAD="
  git -C "$CTRL" rev-parse HEAD
  echo -n "CONTROL_BRANCH="
  git -C "$CTRL" rev-parse --abbrev-ref HEAD
  echo -n "CONTROL_STATUS_COUNT="
  git -C "$CTRL" status --porcelain | wc -l
  echo "CONTROL_LOG_5="
  git -C "$CTRL" log --oneline -5
else
  echo "CONTROL_NOT_GIT"
fi
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

# Per-file diffs: emit a marker then git diff. Large files OK.
diffs = r"""#!/bin/zsh
set -u
REPO="/Users/erol/Documents/Codex/cortex-b2-review-20260909/positive"
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
  echo "===== BEGIN_DIFF $p ====="
  git -C "$REPO" diff -- "$p"
  echo "===== END_DIFF $p ====="
done
"""

(base / "mac_more.sh").write_bytes(more.encode("utf-8").replace(b"\r\n", b"\n").replace(b"\r", b"\n"))
(base / "mac_diffs.sh").write_bytes(diffs.encode("utf-8").replace(b"\r\n", b"\n").replace(b"\r", b"\n"))
print("wrote scripts")
