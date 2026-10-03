#!/bin/bash
# The existing Linux endgame drivers, each within its predeclared acceptance row.
set -uo pipefail
: "${LANE:?set LANE}" "${SHA:?set SHA}" "${ACCEPTANCE_COLLECTION:?set ACCEPTANCE_COLLECTION}"
PY=/usr/bin/python3
REPO=$LANE/repo
"$PY" "$REPO/tools/acceptance_posix_guard.py" --check || exit 3
GUARD=$HOME/cortex-workers/ACCEPTANCE-STREAM-RUNNING
mkdir "$GUARD" || { echo "box launch refused: $GUARD held by $(cat "$GUARD/owner" 2>/dev/null)"; exit 3; }
export CC_ACCEPTANCE_BOX_OWNER="$SHA:$LANE:endgame:$$"
printf '%s\n' "$CC_ACCEPTANCE_BOX_OWNER" > "$GUARD/owner"
release_box() { if [ "$(cat "$GUARD/owner" 2>/dev/null)" = "$CC_ACCEPTANCE_BOX_OWNER" ]; then rm "$GUARD/owner"; rmdir "$GUARD"; fi; }
trap release_box EXIT
export CC_INVENTORY_DIR=$LANE/inventory
EV=$LANE/evidence
export CCCP_HEADLESS=1 PYTHONDONTWRITEBYTECODE=1 CCCP_TEST_BINARY=$REPO/build-gcc/CortexCommand
eval "$(systemctl --user show-environment 2>/dev/null | grep -E '^(DISPLAY|XAUTHORITY|XDG_RUNTIME_DIR)=' | sed 's/^/export /')"
export DISPLAY=${DISPLAY:-:0}
receipt() {
  "$PY" "$REPO/tools/acceptance_collection.py" "$@" --root "$LANE" --share-root "$LANE" --share X.linux \
    --inventory "$LANE/inventory" --plan "$LANE/acceptance-plan.json" --schedule "$LANE/split-plan.json"
}
row() {
  local id=$1; shift
  local log=$EV/$id.log rc=0
  receipt begin --id "$id" --argv-json "$("$PY" -c 'import json,sys; print(json.dumps(sys.argv[1:]))' "$@")" || return 1
  "$@" > "$log" 2>&1 || rc=$?
  receipt finish --id "$id" --exit-code "$rc" --log "$log"
}
mkdir -p "$EV/tools" "$EV/endgame"
row linux.tools-suites "$PY" "$REPO/tools/linux/run_tools_suites.py" --repo "$REPO" --out "$EV/tools/result.json"
for c in focus locale chat graceful; do
  row "linux.endgame-$c" "$PY" "$REPO/tools/linux/test_match_endgame.py" --repo "$REPO" --out "$EV/endgame/$c" --case "$c" --port 50302
done
"$PY" "$REPO/tools/linux/prepare_acceptance_inputs.py" --repo "$REPO" --out "$LANE/inputs/admission" --port 50300 > "$EV/admission-inputs.log" 2>&1
row linux.endgame-admission "$PY" "$REPO/tools/linux/test_admission_endgame.py" --repo "$REPO" --out "$EV/endgame/admission" \
  --port 50305 --version-ui "$LANE/inputs/admission/version-ui.json"
row linux.endgame-saves "$PY" "$REPO/tools/linux/test_old_saves.py" --repo "$REPO" --out "$EV/endgame/saves" \
  --original-7 "$LANE/inputs/saves/original7.ccsave" --fork-0920 "$LANE/inputs/saves/fork0920.ccsave" --inputs "$LANE/inputs/saves/inputs.json"
row linux.directory-fallback "$PY" "$REPO/tools/linux/test_directory_fallback.py" --repo "$REPO" --out "$EV/endgame/directory-fallback" --port 50300
row linux.directory-settings "$PY" "$REPO/tools/linux/test_directory_settings.py" --repo "$REPO" --out "$EV/endgame/directory-settings" --url https://community.example.invalid/a//b
receipt fail-missing --reason 'native stream or endgame driver ended before its declared command completed; see retained logs' --log "$LANE/job.out"
receipt collect
