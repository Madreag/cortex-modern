#!/bin/zsh
# Read-only Mac collection. Printed to stdout only. No writes on the Mac.
set -u
REPO="/Users/erol/Documents/Codex/cortex-b2-review-20260909/positive"
CODEX="/Users/erol/Documents/Codex"

echo "===== W6_MAC_GIT_STATE ====="
echo "===== REV_PARSE_HEAD ====="
git -C "$REPO" rev-parse HEAD
echo "===== STATUS_PORCELAIN ====="
git -C "$REPO" status --porcelain
echo "===== LOG_ONELINE_5 ====="
git -C "$REPO" log --oneline -5
echo "===== REMOTE_V ====="
git -C "$REPO" remote -v
echo "===== CONFIG_AUTOCRLF ====="
git -C "$REPO" config core.autocrlf; echo "CONFIG_AUTOCRLF_EXIT:$?"
echo "===== DIFF_STAT ====="
git -C "$REPO" diff --stat
echo "===== DIFF_STAT_CACHED ====="
git -C "$REPO" diff --cached --stat
echo "===== BRANCH ====="
git -C "$REPO" rev-parse --abbrev-ref HEAD
echo "===== STATUS_SHORT_B ====="
git -C "$REPO" status -sb
echo "===== CODEX_LS ====="
ls -la "$CODEX"
echo "===== CODEX_REPOS ====="
for d in "$CODEX"/*; do
  echo "----- ENTRY: $d -----"
  if [ -d "$d/.git" ] || [ -f "$d/.git" ]; then
    echo "IS_GIT=1"
    echo -n "HEAD="
    git -C "$d" rev-parse HEAD
    echo -n "STATUS_COUNT="
    git -C "$d" status --porcelain | wc -l
    echo -n "BRANCH="
    git -C "$d" rev-parse --abbrev-ref HEAD
  else
    echo "IS_GIT=0"
    if [ -d "$d" ]; then
      echo "SUBENTRIES:"
      ls -la "$d"
    fi
  fi
done
echo "===== ALSO_NESTED_POSITIVE_PARENT ====="
PARENT="/Users/erol/Documents/Codex/cortex-b2-review-20260909"
echo "----- ENTRY: $PARENT -----"
ls -la "$PARENT"
if [ -d "$PARENT/.git" ] || [ -f "$PARENT/.git" ]; then
  echo "IS_GIT=1"
  echo -n "HEAD="
  git -C "$PARENT" rev-parse HEAD
  echo -n "STATUS_COUNT="
  git -C "$PARENT" status --porcelain | wc -l
fi
# Nested git repos one level deeper under Codex children that were not themselves git roots.
echo "===== NESTED_GIT_UNDER_CODEX ====="
find "$CODEX" -maxdepth 3 \( -name .git -type d -o -name .git -type f \) -print
echo "===== END ====="
