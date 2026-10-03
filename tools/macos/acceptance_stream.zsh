#!/bin/zsh
# Fresh Mac acceptance stream. The RUN chain ships this file by hash and starts it in the launchd GUI session.
export PATH=/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:$PATH
PY=/opt/homebrew/bin/python3
/usr/bin/caffeinate -dimsu -w $$ &

# A red row never stops the stream: every step is recorded and the list is complete; only the build chain is fatal.
set -uo pipefail
: "${SHA:?set SHA to the exact acceptance tip}"
HERE=$(cd "$(dirname "$0")" && pwd)
LANE=${LANE:-$HERE}
INV=$LANE/inventory
REPO=$LANE/repo
EV=$LANE/evidence
export CCCP_HEADLESS=1 PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1 INVENTORY_NO_FULLSTATE=1
: "${ACCEPTANCE_COLLECTION:?the RUN chain supplies the owning collection id}"
test ! -e "$EV"
mkdir "$EV"
PLAN=$EV/required.txt
plan() { printf '%s\n' "$EV/status/$1/DEFECTS.json" >> "$PLAN"; }
receipt() {
  "$PY" "$REPO/tools/acceptance_collection.py" "$@" --root "$LANE" --share-root "$LANE" --share X.mac \
    --inventory "$INV" --plan "$LANE/acceptance-plan.json" --schedule "$LANE/split-plan.json"
}
for name in helper-hashes clone fetch checkout tip gcc-setup gcc-build clang-setup clang-build selftests official13 tools S1 readback asan-setup asan-build asan-receipt asan-suite tsan-setup tsan-build tsan-receipt tsan-suite sanitizer-reports; do plan "$name"; done
printf '%s\n' "$EV/scan/DEFECTS.json" "$EV/S1-DEFECTS.json" >> "$PLAN"
step() {
  local name=$1; shift
  local rc=0
  local row_id=
  case "$name" in selftests|official13|readback|asan-suite|tsan-suite) row_id=mac.$name ;; tools) row_id=mac.tools-suites ;; esac
  if [ -n "$row_id" ]; then
    receipt begin --id "$row_id" --argv-json "$("$PY" -c 'import json,sys; print(json.dumps(sys.argv[1:]))' "$@")" || return 1
  fi
  "$@" > "$EV/$name.log" 2>&1 || rc=$?
  if [ -n "$row_id" ]; then receipt finish --id "$row_id" --exit-code "$rc" --log "$EV/$name.log" || rc=1; fi
  "$PY" "$INV/merge_defects.py" --record-step "$name" --exit-code "$rc" --log "$EV/$name.log" --tip "$SHA" --out "$EV/status/$name"
  echo "$name exit=$rc"
  return "$rc"
}
finish() {
  local rc=$?
  trap - EXIT
  set +e
  if [ -f "$LANE/S1root/S1/progress.json" ]; then receipt import-progress --progress "$LANE/S1root/S1/progress.json"; fi
  receipt fail-missing --reason "stream ended before all declared rows; stream exit=$rc" --log "$LANE/job.out"
  receipt collect
  "$PY" "$INV/extract_defects.py" "$EV" --feel-gates --out "$EV/scan/DEFECTS.json" > "$EV/extract.log" 2>&1
  "$PY" "$INV/merge_defects.py" --plan "$PLAN" --tip "$SHA" --out "$EV/LIST" --followups "$INV/ENGINE-FOLLOWUPS.txt" > "$EV/merge.log" 2>&1
  local collected=$?
  if [ "$rc" = 0 ]; then rc=$collected; else rc=1; fi
  # Completion is published only once every foreground engine/build is gone.
  if pgrep -x CortexCommand >/dev/null || pgrep -x ninja >/dev/null; then rc=1; fi
  if [ "$(cat "$GUARD/owner" 2>/dev/null)" = "$SHA:$LANE:$$" ]; then
    rm "$GUARD/owner"; rmdir "$GUARD"
  fi
  "$PY" - "$LANE" "$SHA" "$rc" <<'PY'
import json, pathlib, sys
root, tip, code = pathlib.Path(sys.argv[1]), sys.argv[2], int(sys.argv[3])
joined=json.loads((root/'evidence/LIST/DEFECTS-all.json').read_text()) if (root/'evidence/LIST/DEFECTS-all.json').is_file() else {}
execution=0 if code in (0,3) else code
d=dict(tip=tip,exit_code=execution,collection_exit=code,complete=True,status='RED' if execution else joined.get('status','RED'),
       awaiting_input=joined.get('awaiting_input',[]),awaiting_review=joined.get('awaiting_review',[]))
(root/'exit.txt').write_text(str(execution)+'\n')
temp=root/'completion.tmp'; temp.write_text(json.dumps(d,indent=2)+'\n'); temp.replace(root/'completion.json')
PY
  exit "$rc"
}
GUARD=$HOME/cortex-workers/ACCEPTANCE-STREAM-RUNNING
mkdir "$GUARD" # exclusive: another stream cannot overwrite the guard
trap finish EXIT
printf '%s\n' "$SHA:$LANE:$$" > "$GUARD/owner"
! pgrep -x CortexCommand >/dev/null
! pgrep -x ninja >/dev/null
step helper-hashes "$PY" - "$INV" <<'PY' || exit 1
import hashlib, json, pathlib, sys
root=pathlib.Path(sys.argv[1]); expected=json.loads((root/'HELPERS-SHA256.json').read_text())
assert expected and all(hashlib.sha256((root/name).read_bytes()).hexdigest()==sha for name,sha in expected.items())
print('current PC inventory hashes verified', len(expected))
PY
step clone git clone --no-checkout https://github.com/Madreag/cortex-modern.git "$REPO" || exit 1
step fetch git -C "$REPO" fetch origin "$SHA" || exit 1
step checkout git -C "$REPO" checkout --detach "$SHA" || exit 1
step tip "$PY" - "$REPO" "$SHA" <<'PY' || exit 1
import subprocess, sys
assert subprocess.check_output(['git','-C',sys.argv[1],'rev-parse','HEAD'],text=True).strip()==sys.argv[2]
print(sys.argv[2])
PY

D=/Users/erol/projects/cccp/deps-audit-20260907
CLANG_ENV=(CC=/usr/bin/clang CXX=/usr/bin/clang++ CXXFLAGS=-stdlib=libc++ LDFLAGS=-stdlib=libc++ OBJCXXFLAGS=-stdlib=libc++ PKG_CONFIG_PATH=$D/protobuf-libcxx/lib/pkgconfig:/opt/homebrew/opt/openssl@3/lib/pkgconfig)
OPTS=(--buildtype=release -Db_lto=false -Db_pch=false -Dcpp_std=c++20 -Dwith_gns=enabled --wrap-mode=nodownload)
SAN_GNS=$D/gns-libcxx
TSAN_GNS=$D/gns-libcxx
step gcc-setup env CC=/opt/homebrew/bin/gcc-13 CXX=/opt/homebrew/bin/g++-13 PKG_CONFIG_PATH="$D/protobuf/lib/pkgconfig:/opt/homebrew/opt/openssl@3/lib/pkgconfig" meson setup "$REPO/build-gcc" "$REPO" "${OPTS[@]}" -Dgns_root="$D/gns" || exit 1
step gcc-build ninja -C "$REPO/build-gcc" -j8 || exit 1
step clang-setup env "${CLANG_ENV[@]}" meson setup "$REPO/build-clang" "$REPO" "${OPTS[@]}" -Dgns_root="$D/gns-libcxx" || exit 1
step clang-build ninja -C "$REPO/build-clang" -j8 || exit 1

export CCCP_TEST_BINARY=$REPO/build-gcc/CortexCommand
step identity "$PY" "$REPO/tools/acceptance_identity.py" --box Mac --repo "$REPO" --exe "$CCCP_TEST_BINARY" \
  --source-sha "$SHA" --collection-id "$ACCEPTANCE_COLLECTION" --build-receipt "$EV/build.json" \
  --write-build-receipt --build-exit-code 0 --build-log "$EV/gcc-build.log" --out "$EV/identity.json" || exit 1
step selftests "$PY" "$REPO/tools/run_selftests.py" --repo "$REPO" --out "$EV/suite" --timeout 600 --quiet-rows last
step official13 "$PY" "$REPO/tools/linux/run_official13.py" --repo "$REPO" --binary "$CCCP_TEST_BINARY" --out "$EV/official13" --head "$SHA"
step tools "$PY" "$REPO/tools/linux/run_tools_suites.py" --repo "$REPO" --out "$EV/tools/result.json"
EXE=$($PY -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$CCCP_TEST_BINARY")
# S1's restore/world/join arms run here. TURN rows stay on EROL-PC (D5); its login file never leaves that PC.
plan S1-plan
s1_plan() {
  "$PY" "$INV/run_stream.py" S1 --repo "$REPO" --out "$LANE/S1root" --exe "$EXE" --confirming --plan-json > "$EV/S1-plan.json" || return
  "$PY" - "$EV" <<'PY_S1'
import json,pathlib,sys
root=pathlib.Path(sys.argv[1]); plan=json.loads((root/'S1-plan.json').read_text())
windows={'S1.turn-hold','S1.turn-renew','S1.relay-compare'}
selected=[r['id'] for r in plan if r['id'] not in windows]
assert {'S1.restore-all-1','S1.restore-all-2','S1.restore-all-3','S1.mixed-history-join-forced-hold'} <= set(selected)
(root/'S1-ids.txt').write_text('\n'.join(selected)+'\n')
(root/'S1-platform-scope.json').write_text(json.dumps(dict(here=selected,EROL_PC_D5=sorted(windows)),indent=2))
PY_S1
}
step S1-plan s1_plan
S1_ARGS=()
while IFS= read -r id; do S1_ARGS+=("$id"); done < "$EV/S1-ids.txt"
step S1 "$PY" "$INV/run_stream.py" S1 --repo "$REPO" --out "$LANE/S1root" --exe "$EXE" --confirming --only "${S1_ARGS[@]}" \
  --box Mac --collection-id "$ACCEPTANCE_COLLECTION" --build-receipt "$EV/build.json"
cp "$LANE/S1root/S1/DEFECTS.json" "$EV/S1-DEFECTS.json"
step readback "$PY" "$REPO/tools/test_menu_readback.py" --repo "$REPO" --out "$EV/readback" --case landing --size 640x360 --port 48530
# R4: rows 510-516 are Linux-only; the Mac runs suites, sanitizers, S1 and readback.

san_receipt() {
  "$PY" - "$REPO" "$SHA" "$1" "$EV/$1-build.json" <<'PY'
import hashlib, json, pathlib, subprocess, sys
repo, tip, kind, out=pathlib.Path(sys.argv[1]),sys.argv[2],sys.argv[3],pathlib.Path(sys.argv[4])
exe=repo/('build-'+kind)/'CortexCommand'
assert subprocess.check_output(['git','-C',str(repo),'rev-parse','HEAD'],text=True).strip()==tip
options=json.loads(subprocess.check_output(['meson','introspect',str(exe.parent),'--buildoptions'],text=True))
sanitize=next(row['value'] for row in options if row['name']=='b_sanitize')
assert ('address' in sanitize and 'undefined' in sanitize) if kind=='asan' else 'thread' in sanitize
out.write_text(json.dumps(dict(tip=tip, sanitizer=sanitize, executable_sha256=hashlib.sha256(exe.read_bytes()).hexdigest())))
print(out.read_text())
PY
}
verify_san() {
  "$PY" - "$EV/$1-build.json" "$REPO/build-$1/CortexCommand" "$SHA" <<'PY'
import hashlib,json,pathlib,sys
d=json.loads(pathlib.Path(sys.argv[1]).read_text())
assert d['tip']==sys.argv[3] and d['executable_sha256']==hashlib.sha256(pathlib.Path(sys.argv[2]).read_bytes()).hexdigest()
PY
}
SAN_OPTS=(--buildtype=debugoptimized -Db_lto=false -Db_pch=false -Db_lundef=false -Dcpp_std=c++20 -Dwith_gns=enabled --wrap-mode=nodownload)
step asan-setup env "${CLANG_ENV[@]}" meson setup "$REPO/build-asan" "$REPO" "${SAN_OPTS[@]}" -Dgns_root="$SAN_GNS" -Db_sanitize=address,undefined || exit 1
step asan-build env ASAN_OPTIONS=detect_leaks=0 ninja -C "$REPO/build-asan" -j8 || exit 1
step asan-receipt san_receipt asan || exit 1
verify_san asan || exit 1
step identity-asan "$PY" "$REPO/tools/acceptance_identity.py" --box Mac --repo "$REPO" --exe "$REPO/build-asan/CortexCommand" \
  --source-sha "$SHA" --collection-id "$ACCEPTANCE_COLLECTION" --build-receipt "$EV/asan-identity-build.json" \
  --write-build-receipt --build-exit-code 0 --build-log "$EV/asan-build.log" --configuration asan --out "$EV/identity-asan.json" || exit 1
step asan-suite env CCCP_TEST_BINARY="$REPO/build-asan/CortexCommand" ASAN_OPTIONS="detect_leaks=0:halt_on_error=0:log_path=$EV/asan-report" UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=0:log_path=$EV/ubsan-report" "$PY" "$REPO/tools/run_selftests.py" --repo "$REPO" --out "$EV/asan-suite" --timeout 1200 --quiet-rows last
step tsan-setup env "${CLANG_ENV[@]}" meson setup "$REPO/build-tsan" "$REPO" "${SAN_OPTS[@]}" -Dgns_root="$TSAN_GNS" -Db_sanitize=thread || exit 1
step tsan-build ninja -C "$REPO/build-tsan" -j8 || exit 1
step tsan-receipt san_receipt tsan || exit 1
verify_san tsan || exit 1
step identity-tsan "$PY" "$REPO/tools/acceptance_identity.py" --box Mac --repo "$REPO" --exe "$REPO/build-tsan/CortexCommand" \
  --source-sha "$SHA" --collection-id "$ACCEPTANCE_COLLECTION" --build-receipt "$EV/tsan-identity-build.json" \
  --write-build-receipt --build-exit-code 0 --build-log "$EV/tsan-build.log" --configuration tsan --out "$EV/identity-tsan.json" || exit 1
step tsan-suite env CCCP_TEST_BINARY="$REPO/build-tsan/CortexCommand" TSAN_OPTIONS="halt_on_error=0:second_deadlock_stack=1:log_path=$EV/tsan-report" "$PY" "$REPO/tools/run_selftests.py" --repo "$REPO" --out "$EV/tsan-suite" --timeout 1800 --quiet-rows last

step sanitizer-reports "$PY" - "$EV" <<'PY'
import pathlib,sys
root=pathlib.Path(sys.argv[1])
bad=[str(p) for pattern in ('asan-report*','ubsan-report*','tsan-report*','lsan-report*') for p in root.glob(pattern) if p.is_file() and p.stat().st_size]
assert not bad, 'nonempty sanitizer reports: '+repr(bad)
print('zero nonempty sanitizer reports')
PY
exit 0
