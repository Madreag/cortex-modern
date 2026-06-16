#!/bin/bash
# r20 arm64 3-way suite: build @ 9647a0285 already done; run all 11 scenarios at 900 ticks, seed 42, 4 lua states.
cd /Users/erol/projects/cccp/cortex-modern
BIN=./builddir/CortexCommand
PROG=suite_r20_progress.log
: > "$PROG"
SCENARIOS="SimBaseline LuaBaseline LuaOsStubTest LuaPairsStress LuaRandomStress ModSmokeLoading TerrainStress TerrainCarveStress ActorStress ThreadStress PerfBench"
for s in $SCENARIOS; do
  echo "[$(date '+%H:%M:%S')] START $s" >> "$PROG"
  CTRLSTATE_DUMP= timeout -s KILL 120 "$BIN" -scenario "$s" -seed 42 -max-ticks 900 -tick-hashes -num-lua-states 4 -out "suite_${s}.json" > "suite_${s}_run.log" 2>&1
  ec=$?
  n=$(python3 -c "import json;print(len(json.load(open('suite_${s}.json'))['runs'][0]['tick_hashes']))" 2>/dev/null || echo "ERR")
  echo "[$(date '+%H:%M:%S')] DONE  $s exit=$ec ticks=$n" >> "$PROG"
done
echo "ALL_DONE" >> "$PROG"
