#!/bin/zsh
set -eu
export PATH=/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:$PATH
export CCCP_HEADLESS=1
LANE=${CROSS_LANE_ROOT:?set CROSS_LANE_ROOT to the lane directory, e.g. /Users/erol/cortex-workers/<lane>}
GUARD=${CROSS_MAC_GUARD:?set CROSS_MAC_GUARD to the live marker of the Mac inventory that must be finished}
D=/Users/erol/projects/cccp/deps-audit-20260907
REPO=$LANE/repo
SHA=$1
test -f "$GUARD" || { echo 'REFUSED: Mac inventory owns the box'; exit 3; }
test -f "$LANE/include-scan.json" || { echo 'REFUSED: include scan missing'; exit 3; }
test "$(git -C "$REPO" rev-parse HEAD)" = "$SHA" || exit 4
EVIDENCE=$LANE/build-evidence/$SHA-$(date +%Y%m%dT%H%M%S)
mkdir -p "$EVIDENCE"
trap 'crossBuildExit=$?; print -r -- "$crossBuildExit" > "$EVIDENCE/exit.txt"' EXIT
python3 - "$LANE/include-scan.json" "$SHA" <<'PY'
import json,sys
scan=json.load(open(sys.argv[1]))
assert scan['passed'] and scan['commit']==sys.argv[2], 'include scan is not for this commit'
PY
test -f "$GUARD" || exit 3
if [[ ! -f "$REPO/build-gcc/build.ninja" ]]; then
  CC=/opt/homebrew/bin/gcc-13 CXX=/opt/homebrew/bin/g++-13 PKG_CONFIG_PATH=$D/protobuf/lib/pkgconfig:/opt/homebrew/opt/openssl@3/lib/pkgconfig \
    meson setup "$REPO/build-gcc" "$REPO" --buildtype=release -Db_lto=false -Db_pch=false -Dcpp_std=c++20 \
    -Dwith_gns=enabled -Dgns_root=$D/gns --wrap-mode=nodownload > "$EVIDENCE/setup.log" 2>&1
fi
test -f "$GUARD" || exit 3
ninja -C "$REPO/build-gcc" -j8 -k0 > "$EVIDENCE/build.log" 2>&1 &
crossBuildPid=$!
while kill -0 "$crossBuildPid" 2>/dev/null; do
  crossBuildKB=$(du -sk "$LANE" | awk '{print $1}')
  if (( crossBuildKB * 1024 >= 4000000000 )); then
    print -r -- "STOP: scratch reached 4 GB ($crossBuildKB KiB)" > "$EVIDENCE/budget-stop.txt"
    kill -TERM "$crossBuildPid"
    wait "$crossBuildPid" || true
    exit 5
  fi
  sleep 5
done
wait "$crossBuildPid"
python3 - "$REPO" "$SHA" <<'PY'
from pathlib import Path
import hashlib,json,subprocess,sys
repo=Path(sys.argv[1]); binary=repo/'build-gcc/CortexCommand'
record=dict(commit=sys.argv[2],executable_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
            compiler=subprocess.check_output(['/opt/homebrew/bin/g++-13','--version'],text=True).splitlines()[0])
(repo/'tools/cross_peers/build.json').write_text(json.dumps(record,indent=2)+'\n')
print('BUILT',record['commit'],record['executable_sha256'])
PY
echo 0 > "$EVIDENCE/exit.txt"
