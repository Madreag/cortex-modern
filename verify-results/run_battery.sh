#!/bin/bash
# Cumulative macOS determinism battery driver. Audio ON throughout (no SDL_VIDEODRIVER=offscreen,
# no audio-off). Drives the game binary's -determinism-check mode directly.
# Records per-invocation: phase, scenario, threads, runs, ticks, exit code, RESULT word.
# Never aborts on failure (MPerfBench may SIGSEGV = known MOID race) — records and continues.

set -u
cd /Users/erol/projects/cccp/cortex-modern
BIN=./builddir/CortexCommand
DET=verify-work/det
LOGS=verify-work/logs
SUMMARY=verify-work/SUMMARY.txt
mkdir -p "$DET" "$LOGS"
: > "$SUMMARY"

run_check() {
  # args: phase scenario threads runs ticks
  local phase="$1" scen="$2" threads="$3" runs="$4" ticks="$5"
  local tag="$scen"
  local extra=""
  [ "$threads" != "-" ] && tag="${scen}_th${threads//,/-}" && extra="--threads $threads"
  local log="$LOGS/${phase}_${tag}.log"
  local out="$DET/${phase}_${tag}.json"
  echo "[$(date '+%H:%M:%S')] START $phase $scen threads=$threads runs=$runs ticks=$ticks" | tee -a verify-work/progress.txt
  $BIN -determinism-check --scenario "$scen" --seed 42 --ticks "$ticks" --runs "$runs" $extra --output "$out" > "$log" 2>&1
  local ec=$?
  local resultword="UNKNOWN"
  if grep -q 'RESULT: MATCHED' "$log"; then resultword="MATCHED"; fi
  if grep -q 'RESULT: DIVERGED' "$log"; then resultword="DIVERGED"; fi
  if [ "$resultword" = "UNKNOWN" ]; then
    # No RESULT line => orchestrator bailed (e.g., a child crashed before writing JSON)
    if grep -qiE 'produced no JSON|failed to spawn|unparseable' "$log"; then resultword="CRASH_OR_ERR"; fi
  fi
  local fdt
  fdt=$(grep -oE 'first_divergence_tick: [0-9]+' "$log" | head -1 | grep -oE '[0-9]+')
  echo "${phase}|${scen}|threads=${threads}|runs=${runs}|ticks=${ticks}|exit=${ec}|${resultword}|fdt=${fdt:-}" >> "$SUMMARY"
  echo "[$(date '+%H:%M:%S')] DONE  $phase $scen exit=$ec $resultword ${fdt:+fdt=$fdt}" | tee -a verify-work/progress.txt
}

echo "===== PHASE 2: SINGLE-CORE SWEEP (11 scenarios, runs=10, ticks=300) =====" | tee -a verify-work/progress.txt
for s in M1Baseline M1ActorStress M1TerrainStress M2LuaBaseline M2LuaRandomStress M2PairsStress M2OsStubTest M2ModSmokeLoading M3TerrainStress M4ThreadStress MPerfBench; do
  run_check single "$s" - 10 300
done

echo "===== PHASE 3: THREAD MATRIX (threads=1,2,4,8, runs=10, ticks=300) =====" | tee -a verify-work/progress.txt
for s in M4ThreadStress M3TerrainStress M1ActorStress M1TerrainStress M2PairsStress M2LuaBaseline; do
  run_check matrix "$s" 1,2,4,8 10 300
done

echo "===== PHASE 4: AUDIO-ON SOAK (M4ThreadStress, threads=8, runs=30, ticks=900) =====" | tee -a verify-work/progress.txt
run_check soak M4ThreadStress 8 30 900

echo "ALL_DONE" | tee -a verify-work/progress.txt
