#!/bin/bash
# The Linux inventory stream on the 3090 box (Ubuntu 24.04), the Mac's stream.zsh for this platform:
#   S5   the wave at SHA, the gcc build, the self-test suite and the official 13 on it;
#   the clang+libc++ build (built when the box's libc++ and deps allow it, else its compiler line is the verdict);
#   S1   the rejoin/capture rows through the inventory's run_stream.py on the gcc binary, alone on the box;
#   one GPU row (a menu readback case) on the :0 session;
#   S4b  the clang TSan build and its suite; S4 the clang ASan+UBSan build and its suite without leak checks (the Mac's
#   configuration); S4L the same binary's suite with LeakSanitizer on, where a leaking row exits 23 and is red for it;
#   every product failure is retained; no sanitizer failure is replaced by an unproved quiet rerun.
# The load-sensitive legs (S5, S1) run alone; the TSan suite runs in three shards beside the ASan build and suite, whose
# wall-clock budgets a sanitizer build reports instead of judging.
# Usage, from a lane directory holding this script, run_official13.py, sanitizer_digest.py and inventory/ (a copy of
# lead-tools/inventory):
#   SHA=<full sha> [STEPS="repo gcc s5 libcxx s1 readback tsan asan rerun defects"] nohup bash stream.sh > job.out 2>&1 &
# A repeated evidence path is refused. exit.txt is written at the end; no retained evidence is removed.
set -u
: "${SHA:?set SHA to the full tip sha}"
HERE=$(cd "$(dirname "$0")" && pwd)
LANE=${LANE:-$HERE}
BRANCH=${BRANCH:-stage2/fixgroup-6-lead-wave-a}
STEPS=${STEPS:-repo gcc s5 libcxx s1 readback tsan asan rerun defects}
export PATH=$HOME/.local/bin:/usr/local/bin:/usr/bin:/bin
export PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1 CCCP_HEADLESS=1
# The confirming arms run without the full-state instrument (the lead's 2026-09-24 ruling, as the Windows streams run).
export INVENTORY_NO_FULLSTATE=1
# A job started over ssh has no desktop environment; the GPU rows need the :0 session's.
if [ -z "${XAUTHORITY:-}" ]; then
  eval "$(systemctl --user show-environment 2>/dev/null | grep -E '^(DISPLAY|XAUTHORITY|XDG_RUNTIME_DIR)=' | sed 's/^/export /')"
fi
export DISPLAY=${DISPLAY:-:0}
PY=/usr/bin/python3
REPO=$LANE/repo
INV=$LANE/inventory
EV=$LANE/evidence
DEPS=$HOME/deps
# GameNetworkingSockets v1.6.0 with the TURN patch, as ~/cortex-workers/setup-3090.sh left it.
GNS_SRC=$HOME/cortex-workers/setup-20260927/gns/gns-src
GNS_COMMIT=2cb93a06350bb065db53abdb0d87cf297e0bfd34
SYMBOLIZER=/usr/lib/llvm-18/bin/llvm-symbolizer
GCC_BIN=$REPO/build-gcc/CortexCommand
TSAN_BIN=$REPO/build-tsan/CortexCommand
ASAN_BIN=$REPO/build-asan/CortexCommand
GCC_OPTS=(--buildtype=release -Db_lto=false -Db_pch=false -Dcpp_std=c++20 -Dwith_gns=enabled -Dgns_root=$DEPS/gns --wrap-mode=nodownload)
SAN_OPTS=(--buildtype=debugoptimized -Db_lto=false -Db_pch=false -Db_lundef=false -Dcpp_std=c++20 -Dwith_gns=enabled --wrap-mode=nodownload)
# The TSan suite's shards: the script-graph walk alone, the long rows, then every other row of SELFTESTS.
TSAN_SHARD_A="script-graph"
TSAN_SHARD_B="preview-binding-exhaustive preview-invariance save-refusal-diagnosis net-match"
TBBLIB=${TBBLIB:-$LANE/deps/tbb-tsan/lib}
TSAN_ENV=(CCCP_TEST_BINARY=$TSAN_BIN LD_LIBRARY_PATH=$TBBLIB
  TSAN_OPTIONS=suppressions=$REPO/tools/sanitizers/tsan.supp:halt_on_error=0:second_deadlock_stack=1:external_symbolizer_path=$SYMBOLIZER)
UBSAN_ENV=UBSAN_OPTIONS=suppressions=$REPO/tools/sanitizers/ubsan.supp:print_stacktrace=1:halt_on_error=0:external_symbolizer_path=$SYMBOLIZER
ASAN_ENV=(CCCP_TEST_BINARY=$ASAN_BIN $UBSAN_ENV
  ASAN_OPTIONS=detect_leaks=0:abort_on_error=0:halt_on_error=0:symbolize=1:external_symbolizer_path=$SYMBOLIZER)
# exitcode is a flag common to ASan and LSan, so it is left at its defaults: 1 for an ASan error, 23 for a leak.
LSAN_ENV=(CCCP_TEST_BINARY=$ASAN_BIN $UBSAN_ENV
  ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:halt_on_error=0:symbolize=1:external_symbolizer_path=$SYMBOLIZER)

mkdir -p $EV
stamp() { TZ=America/Phoenix date '+%Y-%m-%d %I:%M:%S %p MST'; }
say() { echo "[$(stamp)] $*" | tee -a $EV/steps.log; }
fail() { say "FAIL: $*"; exit 1; }
want() { case " $STEPS " in *" $1 "*) return 0;; esac; return 1; }
fresh() { test ! -e "${1:?}" || fail "evidence already exists: $1"; mkdir -p "$1"; }
receipt() {
  [ -n "${ACCEPTANCE_COLLECTION:-}" ] || return 0
  "$PY" "$REPO/tools/acceptance_collection.py" "$@" --root "$LANE" --share-root "$LANE" --share X.linux \
    --inventory "$INV" --plan "$LANE/acceptance-plan.json" --schedule "$LANE/split-plan.json"
}
row() {
  local id=$1 log=$2; shift 2
  receipt begin --id "$id" --argv-json "$("$PY" -c 'import json,sys; print(json.dumps(sys.argv[1:]))' "$@")" || return 1
  "$@" > "$log" 2>&1; local rc=$?
  receipt finish --id "$id" --exit-code "$rc" --log "$log" || rc=1
  return "$rc"
}
finish_collection() {
  local rc=$?
  trap - EXIT
  for pid in "${TSAN_PIDS[@]:-}"; do [ -z "$pid" ] || wait "$pid"; done
  if [ -f "$LANE/S1root/S1/progress.json" ]; then receipt import-progress --progress "$LANE/S1root/S1/progress.json"; fi
  # The RUN chain adds the declared tools/endgame rows after this stream; its final collect closes the full share.
  if [ -f "$REPO/tools/acceptance_collection.py" ]; then receipt collect; fi
  if [ -n "${ACCEPTANCE_COLLECTION:-}" ] && [ "$(cat "$GUARD/owner" 2>/dev/null)" = "$SHA:$LANE:$$" ]; then
    rm "$GUARD/owner"; rmdir "$GUARD"
  fi
  echo "$rc" > "$LANE/exit.txt"
  exit "$rc"
}
suite_line() { $PY - "$1" <<'EOF'
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except (OSError, ValueError) as error:
    print(f"no result.json ({error.__class__.__name__})"); raise SystemExit
red = [k for k, v in d.get("results", {}).items() if not v.get("pass")]
print(f"{d.get('passed')}/{d.get('total')} complete={d.get('complete')} sanitizer={d.get('sanitizer')} red={red}")
EOF
}

test ! -e "$LANE/exit.txt" || fail 'the stream already has an exit receipt; use a new lane'
GUARD=$HOME/cortex-workers/ACCEPTANCE-STREAM-RUNNING
trap finish_collection EXIT
if [ -n "${ACCEPTANCE_COLLECTION:-}" ]; then
  mkdir "$GUARD" || fail 'another acceptance stream owns the box'
  printf '%s\n' "$SHA:$LANE:$$" > "$GUARD/owner"
fi
say "start lane=$LANE branch=$BRANCH sha=$SHA steps=[$STEPS]"
{ echo "date=$(stamp)"; uname -a; lsb_release -ds; echo "threads=$(nproc)"; free -g | head -2; $PY --version
  gcc --version | head -1; clang --version | head -1; meson --version; ninja --version; rr --version; df -h $HOME | tail -1
  env | grep -E '^(DISPLAY|XAUTHORITY|XDG_SESSION_TYPE)='; } > $EV/env.txt 2>&1

if want repo; then
  if [ -d $REPO/.git ]; then
    git -C $REPO fetch -q origin $BRANCH > $EV/clone.log 2>&1 || fail "fetch (clone.log)"
  else
    git clone -q --branch $BRANCH --single-branch https://github.com/Madreag/cortex-modern.git $REPO > $EV/clone.log 2>&1 || fail "clone (clone.log)"
  fi
  git -C $REPO checkout -q --detach $SHA >> $EV/clone.log 2>&1 || fail "checkout $SHA (clone.log)"
fi
HEAD=$(git -C $REPO rev-parse HEAD 2>/dev/null)
[ "$HEAD" = "$SHA" ] || fail "HEAD $HEAD != $SHA"
N=$($PY -c "import sys; sys.path.insert(0, '$REPO/tools'); import run_selftests as r; print(len(r.SELFTESTS))")
say "repo HEAD $HEAD; suite N=$N (len(SELFTESTS))"
$PY $REPO/tools/sanitizers/check_ubsan_supp.py --self-test > $EV/ubsan-suppression-check.log 2>&1 || fail "UBSan suppression validation"
cp $REPO/tools/sanitizers/ubsan.supp $EV/ubsan.supp

if want gcc; then
  say "gcc build start"
  if [ ! -f $REPO/build-gcc/build.ninja ]; then
    ( CC=gcc CXX=g++ meson setup $REPO/build-gcc $REPO "${GCC_OPTS[@]}" ) > $EV/engine-gcc-setup.log 2>&1 || fail "gcc meson setup (engine-gcc-setup.log)"
  fi
  ninja -C $REPO/build-gcc -j8 > $EV/engine-gcc-build.log 2>&1; RC=$?
  echo $RC > $EV/gcc-build-exit.txt; tail -30 $EV/engine-gcc-build.log > $EV/engine-gcc-tail.txt
  say "gcc build exit=$RC gns-turnfix includes=$(grep -c 'gns-turnfix/include' $REPO/build-gcc/compile_commands.json) sha256=$(sha256sum $GCC_BIN 2>/dev/null | cut -c1-64)"
  [ $RC -eq 0 ] && [ -f $GCC_BIN ] || fail "gcc build red (engine-gcc-tail.txt)"
fi

if [ -n "${ACCEPTANCE_COLLECTION:-}" ]; then
  "$PY" "$REPO/tools/acceptance_identity.py" --box Linux --repo "$REPO" --exe "$GCC_BIN" --source-sha "$SHA" \
    --collection-id "$ACCEPTANCE_COLLECTION" --build-receipt "$EV/build.json" --write-build-receipt \
    --build-exit-code "$(cat "$EV/gcc-build-exit.txt")" --build-log "$EV/engine-gcc-build.log" --out "$EV/identity.json" || fail 'native identity'
fi

if want s5; then
  [ -f $GCC_BIN ] || fail "s5: no gcc binary"
  fresh $EV/S5
  say "s5 suite start"
  row linux.selftests "$EV/S5/suite-stdout.log" env CCCP_TEST_BINARY=$GCC_BIN $PY -u $REPO/tools/run_selftests.py --repo $REPO --out $EV/S5/suite --timeout 600 --quiet-rows last; RC=$?
  echo $RC > $EV/S5/suite-exit.txt
  say "s5 suite exit=$RC $(suite_line $EV/S5/suite/result.json)"
  row linux.official13 "$EV/S5/official13-stdout.log" $PY -u $HERE/run_official13.py --repo $REPO --binary $GCC_BIN --out $EV/S5/official13 --head $SHA; RC=$?
  echo $RC > $EV/S5/official13-exit.txt
  say "s5 official13 exit=$RC $(suite_line $EV/S5/official13/result.json)"
fi

if want libcxx; then
  fresh $EV/libcxx
  printf '#include <vector>\nint main() { return std::vector<int>{1}.size() == 1 ? 0 : 1; }\n' > $EV/libcxx/probe.cpp
  clang++ -stdlib=libc++ $EV/libcxx/probe.cpp -o $EV/libcxx/probe > $EV/libcxx/probe.log 2>&1; PRC=$?
  test ! -e "$REPO/build-libcxx" || fail 'libcxx build already exists; use a new lane'
  ( CC=clang CXX=clang++ CXXFLAGS=-stdlib=libc++ LDFLAGS=-stdlib=libc++ meson setup $REPO/build-libcxx $REPO "${GCC_OPTS[@]}" ) > $EV/libcxx/setup.log 2>&1; SRC=$?
  BRC=not-run
  if [ $SRC -eq 0 ]; then
    ninja -C $REPO/build-libcxx -j8 > $EV/libcxx/build.log 2>&1; BRC=$?
    tail -30 $EV/libcxx/build.log > $EV/libcxx/build-tail.txt
  fi
  { echo "probe exit=$PRC meson setup exit=$SRC ninja exit=$BRC"; grep -m3 -E 'error|fatal' $EV/libcxx/probe.log; grep -m3 -iE 'error|cannot' $EV/libcxx/setup.log
    [ -f $EV/libcxx/build.log ] && grep -m5 -E 'error|undefined reference' $EV/libcxx/build.log; } > $EV/libcxx/verdict.txt 2>&1
  say "libcxx $(head -1 $EV/libcxx/verdict.txt): $(sed -n 2p $EV/libcxx/verdict.txt)"
fi

if want s1; then
  [ -f $GCC_BIN ] || fail "s1: no gcc binary"
  if [ ! -f $INV/run_stream.py ]; then
    say "s1 unavailable: no inventory copy at $INV"; echo unavailable > $EV/s1-exit.txt
  else
    fresh $LANE/S1root
    say "s1 start (native POSIX executable selection)"
    S1SHA=$(sha256sum $GCC_BIN | cut -c1-64)
    S1_IDS=()
    if [ -n "${ACCEPTANCE_COLLECTION:-}" ]; then
      mapfile -t S1_IDS < <("$PY" -c 'import json,sys; p=json.load(open(sys.argv[1])); print("\n".join(r["id"] for r in p["rows"] if r["share"]=="X.linux" and r["id"].startswith("S1.")))' "$LANE/acceptance-plan.json")
    else
      "$PY" "$INV/run_stream.py" S1 --repo "$REPO" --out "$LANE/S1root" --exe "$S1SHA" --confirming --plan-json > "$EV/S1-plan.json"
      mapfile -t S1_IDS < <("$PY" -c 'import json,sys; print("\n".join(r["id"] for r in json.load(open(sys.argv[1])) if r["id"] not in {"S1.turn-hold","S1.turn-renew","S1.relay-compare"}))' "$EV/S1-plan.json")
    fi
    [ "${#S1_IDS[@]}" -gt 0 ] || fail 'no declared Linux S1 commands'
    ( cd "$INV" && CCCP_TEST_BINARY="$GCC_BIN" "$PY" run_stream.py S1 --repo "$REPO" --out "$LANE/S1root" --exe "$S1SHA" --confirming \
      --only "${S1_IDS[@]}" --box Linux --collection-id "${ACCEPTANCE_COLLECTION:-standalone}" --build-receipt "$EV/build.json" ) > "$EV/s1-stream.log" 2>&1; RC=$?
    echo $RC > $EV/s1-exit.txt
    cp $LANE/S1root/S1/DEFECTS.json $EV/S1-DEFECTS.json 2>/dev/null; cp $LANE/S1root/S1/progress.json $EV/S1-progress.json 2>/dev/null
    say "s1 exit=$RC $($PY -c "import json; d=json.load(open('$EV/S1-DEFECTS.json')); print('defects', d['defect_count'], 'hard', d['hard_count'])" 2>&1 | tail -1)"
  fi
fi

if want readback; then
  [ -f $GCC_BIN ] || fail "readback: no gcc binary"
  fresh $EV/readback
  say "readback start (DISPLAY=$DISPLAY)"
  row linux.readback "$EV/readback/driver-stdout.log" env CCCP_TEST_BINARY=$GCC_BIN $PY -u $REPO/tools/test_menu_readback.py --repo $REPO --out $EV/readback/run --case landing --size 640x360 --port 48530; RC=$?
  echo $RC > $EV/readback/exit.txt
  say "readback landing 640x360 exit=$RC: $(tail -1 $EV/readback/driver-stdout.log | cut -c1-200)"
fi

TSAN_PIDS=()
if want tsan; then
  if [ ! -f "$TBBLIB/libtbb.so.12" ]; then
    bash $REPO/tools/linux/tsan_tbb.sh "${TBBLIB%/lib}" > $EV/tbb-tsan-build.log 2>&1 || fail "instrumented TBB build"
  fi
  nm -D "$TBBLIB/libtbb.so.12" | grep -q __tsan_func_entry || fail "TBB is not instrumented"
  echo "$TBBLIB" > $EV/tsan-tbb-library.txt
  say "tsan build start"
  if [ ! -f $REPO/build-tsan/build.ninja ]; then
    ( CC=clang CXX=clang++ meson setup $REPO/build-tsan $REPO "${SAN_OPTS[@]}" -Dgns_root=$DEPS/gns -Db_sanitize=thread ) > $EV/engine-tsan-setup.log 2>&1 || say "tsan meson setup FAILED (engine-tsan-setup.log)"
  fi
  ninja -C $REPO/build-tsan -j8 > $EV/engine-tsan-build.log 2>&1; RC=$?
  echo $RC > $EV/tsan-build-exit.txt; tail -30 $EV/engine-tsan-build.log > $EV/engine-tsan-tail.txt
  say "tsan build exit=$RC"
  if [ $RC -eq 0 ] && [ -f $TSAN_BIN ]; then
    if [ -n "${ACCEPTANCE_COLLECTION:-}" ]; then
      "$PY" "$REPO/tools/acceptance_identity.py" --box Linux --repo "$REPO" --exe "$TSAN_BIN" --source-sha "$SHA" \
        --collection-id "$ACCEPTANCE_COLLECTION" --build-receipt "$EV/tsan-build.json" --write-build-receipt --configuration tsan \
        --build-exit-code "$RC" --build-log "$EV/engine-tsan-build.log" --out "$EV/identity-tsan.json" || fail 'TSan identity'
    fi
    fresh $EV/S4b
    SHARD_C=$($PY -c "import sys; sys.path.insert(0, '$REPO/tools'); import run_selftests as r; print(' '.join(n for n in r.SELFTESTS if n not in '$TSAN_SHARD_A $TSAN_SHARD_B'.split()))")
    for shard in a b c; do
      case $shard in a) rows=$TSAN_SHARD_A;; b) rows=$TSAN_SHARD_B;; c) rows=$SHARD_C;; esac
      only=(); for r in $rows; do only+=(--only $r); done
      echo "$rows" > $EV/S4b/shard-$shard-rows.txt
      row "linux.tsan-suite-$shard" "$EV/S4b/suite-$shard-stdout.log" env "${TSAN_ENV[@]}" $PY -u $REPO/tools/run_selftests.py --repo $REPO --out $EV/S4b/suite-$shard --timeout 3600 "${only[@]}" &
      TSAN_PIDS+=($!)
    done
    echo "${TSAN_PIDS[*]}" > $EV/S4b/pids.txt
    say "tsan suite started: shards a b c (pids ${TSAN_PIDS[*]})"
  fi
fi

if want asan; then
  say "asan build start"
  mkdir -p $LANE/deps
  # meson links <gns_root>-turnfix-ubsan for a UBSan build; the release prefix is reached through the lane's own link.
  [ -e $LANE/deps/gns-turnfix ] || ln -s $DEPS/gns-turnfix $LANE/deps/gns-turnfix
  if [ ! -f $LANE/deps/gns-turnfix-ubsan/lib/libGameNetworkingSockets_s.a ]; then
    ( set -e
      W=$LANE/deps/gns-ubsan-work; test ! -e "$W"; mkdir -p $W
      git clone -q --no-checkout $GNS_SRC $W/src
      git -C $W/src checkout -q $GNS_COMMIT
      git -C $W/src apply $REPO/external/patches/gns-turn-lifetime.patch
      cmake -S $W/src -B $W/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DBUILD_SHARED_LIB=OFF -DBUILD_STATIC_LIB=ON -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TOOLS=OFF -DUSE_CRYPTO=OpenSSL -DSANITIZE_UNDEFINED=ON -DCMAKE_INSTALL_PREFIX=$LANE/deps/gns-turnfix-ubsan
      cmake --build $W/build -j 8
      cmake --install $W/build
      nm -C $LANE/deps/gns-turnfix-ubsan/lib/libGameNetworkingSockets_s.a | grep -q "typeinfo for SteamNetworkingSocketsLib::CSteamNetworkingSockets"
    ) > $EV/gns-ubsan.log 2>&1 && say "gns ubsan prefix built" || say "gns ubsan prefix FAILED (gns-ubsan.log)"
  fi
  if [ ! -f $REPO/build-asan/build.ninja ]; then
    ( CC=clang CXX=clang++ meson setup $REPO/build-asan $REPO "${SAN_OPTS[@]}" -Dgns_root=$LANE/deps/gns -Db_sanitize=address,undefined ) > $EV/engine-asan-setup.log 2>&1 || say "asan meson setup FAILED (engine-asan-setup.log)"
  fi
  # LuaJIT's build-time generator (buildvm) never frees its buffers; LeakSanitizer's exit would fail its build steps.
  ASAN_OPTIONS=detect_leaks=0 ninja -C $REPO/build-asan -j8 > $EV/engine-asan-build.log 2>&1; RC=$?
  echo $RC > $EV/asan-build-exit.txt; tail -30 $EV/engine-asan-build.log > $EV/engine-asan-tail.txt
  say "asan build exit=$RC gns-turnfix-ubsan includes=$(grep -c 'gns-turnfix-ubsan/include' $REPO/build-asan/compile_commands.json 2>/dev/null)"
  if [ $RC -eq 0 ] && [ -f $ASAN_BIN ]; then
    if [ -n "${ACCEPTANCE_COLLECTION:-}" ]; then
      "$PY" "$REPO/tools/acceptance_identity.py" --box Linux --repo "$REPO" --exe "$ASAN_BIN" --source-sha "$SHA" \
        --collection-id "$ACCEPTANCE_COLLECTION" --build-receipt "$EV/asan-build.json" --write-build-receipt --configuration asan \
        --build-exit-code "$RC" --build-log "$EV/engine-asan-build.log" --out "$EV/identity-asan.json" || fail 'ASan identity'
    fi
    fresh $EV/S4
    say "asan suite start (detect_leaks=0)"
    row linux.asan-suite "$EV/S4/suite-stdout.log" env "${ASAN_ENV[@]}" $PY -u $REPO/tools/run_selftests.py --repo $REPO --out $EV/S4/suite --timeout 2400 --quiet-rows last; RC=$?
    echo $RC > $EV/S4/suite-exit.txt
    say "asan suite exit=$RC $(suite_line $EV/S4/suite/result.json)"
    fresh $EV/S4L
    say "lsan suite start (detect_leaks=1)"
    row linux.lsan-suite "$EV/S4L/suite-stdout.log" env "${LSAN_ENV[@]}" $PY -u $REPO/tools/run_selftests.py --repo $REPO --out $EV/S4L/suite --timeout 2400 --quiet-rows last; RC=$?
    echo $RC > $EV/S4L/suite-exit.txt
    say "lsan suite exit=$RC $(suite_line $EV/S4L/suite/result.json)"
  fi
fi

if [ ${#TSAN_PIDS[@]} -gt 0 ]; then
  for pid in "${TSAN_PIDS[@]}"; do wait $pid; done
  for shard in a b c; do say "tsan shard $shard: $(suite_line $EV/S4b/suite-$shard/result.json)"; done
fi

if want rerun; then
  say 'product failures retained; no rerun without a recorded pre-launch HARNESS cause'
fi

if want defects; then
  for s in S5 S4 S4L S4b readback; do
    [ -d $EV/$s ] || continue
    $PY $INV/extract_defects.py $EV/$s --out $EV/$s/DEFECTS.raw.json --driver-hint tools/run_selftests.py > $EV/$s-extract.log 2>&1
    $PY $REPO/tools/linux/normalize_suite_defects.py $EV/$s --input $EV/$s/DEFECTS.raw.json --out $EV/$s/DEFECTS.json >> $EV/$s-extract.log 2>&1
    say "$s DEFECTS: $($PY -c "import json; d=json.load(open('$EV/$s/DEFECTS.json')); print('defects', d['defect_count'], 'hard', d['hard_count'])" 2>&1 | tail -1)"
  done
  for s in S4 S4L S4b; do
    [ -d $EV/$s ] || continue
    $PY $REPO/tools/linux/sanitizer_digest.py $EV/$s --ubsan-supp $REPO/tools/sanitizers/ubsan.supp --out $EV/$s/sanitizer-digest > $EV/$s-digest.log 2>&1
    say "$s sanitizer digest: $(tail -1 $EV/$s-digest.log)"
  done
fi

du -sh $LANE $REPO > $EV/disk.txt 2>&1
say "DONE steps=[$STEPS]"
exit 0
