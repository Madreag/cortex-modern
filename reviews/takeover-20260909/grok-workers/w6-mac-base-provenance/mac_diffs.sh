#!/bin/zsh
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
