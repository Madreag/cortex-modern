#!/bin/zsh
set -eu
export PATH=/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:$PATH
export CCCP_HEADLESS=1
LANE=/Users/erol/cortex-workers/astra-cross-peers-build-20260926
GUARD=/Users/erol/cortex-workers/inventory-confirming-6-20260926/exit.txt
D=/Users/erol/projects/cccp/deps-audit-20260907
REPO=$LANE/repo
SHA=$1
test -f "$GUARD" || { echo 'REFUSED: Mac inventory owns the box'; exit 3; }
test -f "$LANE/include-scan.json" || { echo 'REFUSED: include scan missing'; exit 3; }
test "$(git -C "$REPO" rev-parse HEAD)" = "$SHA" || exit 4
mkdir -p "$LANE/build-evidence"
python3 - "$LANE/include-scan.json" "$SHA" <<'PY'
import json,sys
scan=json.load(open(sys.argv[1]))
assert scan['passed'] and scan['commit']==sys.argv[2], 'include scan is not for this commit'
PY
test -f "$GUARD" || exit 3
if [[ ! -f "$REPO/build-gcc/build.ninja" ]]; then
  CC=/opt/homebrew/bin/gcc-13 CXX=/opt/homebrew/bin/g++-13 PKG_CONFIG_PATH=$D/protobuf/lib/pkgconfig:/opt/homebrew/opt/openssl@3/lib/pkgconfig \
    meson setup "$REPO/build-gcc" "$REPO" --buildtype=release -Db_lto=false -Db_pch=false -Dcpp_std=c++20 \
    -Dwith_gns=enabled -Dgns_root=$D/gns --wrap-mode=nodownload > "$LANE/build-evidence/setup.log" 2>&1
fi
test -f "$GUARD" || exit 3
ninja -C "$REPO/build-gcc" -j8 > "$LANE/build-evidence/build.log" 2>&1
python3 - "$REPO" "$SHA" <<'PY'
from pathlib import Path
import hashlib,json,subprocess,sys
repo=Path(sys.argv[1]); binary=repo/'build-gcc/CortexCommand'
record=dict(commit=sys.argv[2],executable_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
            compiler=subprocess.check_output(['/opt/homebrew/bin/g++-13','--version'],text=True).splitlines()[0])
(repo/'tools/cross_peers/build.json').write_text(json.dumps(record,indent=2)+'\n')
print('BUILT',record['commit'],record['executable_sha256'])
PY
echo 0 > "$LANE/build-evidence/exit.txt"
