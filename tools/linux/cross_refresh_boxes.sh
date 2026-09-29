#!/bin/bash
# Rows 499/508: before the multi-box rung, bring EDITH, Mac and Linux to the tip EROL-PC's tree was
# just built at. EDITH gets alias-walk's executable (the harness requires the same Windows bytes on every Windows box), every
# DLL, tools/ and Data/ file whose hash differs, and a receipt written from the hash measured on EDITH after the copy; the Mac
# checks the tip out in the harness's clone, rebuilds with ninja and writes its receipt from the binary's measured hash; then
# every remote box runs the harness's own preflight and the coordinator's checks are applied to all results (content,
# modules, fixture, the Windows hash, the receipts). A box already at the tip (executable and receipt) is a no-op. Nothing is
# moved or deleted on any box except this script's own transfer and exit files.
# usage: cross_refresh_boxes.sh <tip-sha> [<prev-sha>] [--lane L] [--mac-guard FILE] [--work DIR]      (Git Bash)
#   lane and guard default to CC_CROSS_PEERS_LANE / CC_CROSS_PEERS_MAC_GUARD, work to D:/mx/<lane>/refresh; <prev-sha>
#   (default: EDITH's receipt commit) only labels the git diff in the log - the ship list comes from the file hashes.
# Last line: 'REFRESH PASS ...' (exit 0) or 'REFRESH FAIL <box>: <reason>' (exit 1 EDITH, 2 Mac, 3 preflight, 4 EROL-PC/usage).
set -u
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'
TIP=${1:-}; [ $# -gt 0 ] && shift
PREV=""; if [ $# -gt 0 ] && [ "${1#--}" = "$1" ]; then PREV=$1; shift; fi
LANE=${CC_CROSS_PEERS_LANE:-}; GUARD=${CC_CROSS_PEERS_MAC_GUARD:-}; WORK=""
AW=${CC_CROSS_PEERS_REPO:-D:/Projects/alias-walk}
while [ $# -gt 0 ]; do
  case $1 in
    --lane) LANE=$2; shift 2 ;;
    --mac-guard) GUARD=$2; shift 2 ;;
    --work) WORK=$2; shift 2 ;;
    --repo) AW=$2; shift 2 ;;
    --boxes) export CC_CROSS_PEERS_BOXES=$2; shift 2 ;;
    *) echo "REFRESH FAIL usage: unknown option $1"; exit 4 ;;
  esac
done
say() { echo "[$(date '+%Y-%m-%d %H:%M:%S MST')] $*"; }
fail() { say "REFRESH FAIL $1: $2"; exit "$3"; }
[ -n "$TIP" ] && [ -n "$LANE" ] && [ -n "$GUARD" ] || fail usage "<tip-sha>, a lane and the Mac guard are required" 4
TIP=$(git -C "$AW" rev-parse --verify -q "$TIP^{commit}") || fail EROL-PC "$1 is not a commit in alias-walk" 4
T10=${TIP:0:10}
WORK=${WORK:-D:/mx/$LANE/refresh}
mkdir -p "$WORK" || fail EROL-PC "cannot create $WORK" 4
PY="$WORK/refresh_helper.py"
cat > "$PY" <<'PYEOF'
"""cross_refresh_boxes.sh helper: hash manifests, the ship plan, receipts and the dry preflight."""
import datetime as dt
import hashlib
import json
import os
import shlex
import sys
from pathlib import Path, PurePosixPath


def digest(path):
    h = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''): h.update(block)
    return h.hexdigest()


def manifest(tree, tools_list, out):
    # Top-level executables and DLLs, every Data file under the harness's content rules, and the tip's tracked tools files.
    tree = Path(tree)
    result = dict(top={}, data={}, tools={})
    for path in sorted(tree.iterdir()):
        if path.is_file() and path.suffix.lower() in ('.exe', '.dll'): result['top'][path.name] = digest(path)
    root = tree / 'Data'
    for directory, names, files in os.walk(root, followlinks=False):
        names[:] = sorted(n for n in names if not Path(directory, n).is_symlink())
        for name in sorted(files):
            path = Path(directory, name)
            if path.is_symlink() or path.suffix.lower() in ('.pyc', '.ds_store') or name == '.DS_Store': continue
            result['data']['Data/' + path.relative_to(root).as_posix()] = digest(path)
    for rel in Path(tools_list).read_text(encoding='utf-8').splitlines():
        path = tree / rel
        result['tools'][rel] = digest(path) if path.is_file() else None
    Path(out).write_text(json.dumps(result), encoding='utf-8')
    present = sum(v is not None for v in result['tools'].values())
    print(f'manifest {tree}: top={len(result["top"])} data={len(result["data"])} tools={present}/{len(result["tools"])}')


def plan(local, remote, out):
    a = json.loads(Path(local).read_text(encoding='utf-8'))
    b = json.loads(Path(remote).read_text(encoding='utf-8'))
    ship = {kind: sorted(p for p, h in a[kind].items() if h is not None and b[kind].get(p) != h) for kind in ('top', 'data', 'tools')}
    extra = sorted(p for p in b['data'] if p not in a['data'])
    absent = sorted(p for p, h in a['tools'].items() if h is None)
    Path(out).write_text(''.join(p + '\n' for kind in ship for p in ship[kind]), encoding='utf-8')
    print(f'plan: top={len(ship["top"])} {ship["top"]} data={len(ship["data"])} tools={len(ship["tools"])} data_only_on_box={len(extra)}')
    for p in ship['data'][:20] + ship['tools'][:40]: print('  ship', p)
    for p in extra[:20]: print('  EXTRA (on the box, not at the tip)', p)
    if absent: print('  tracked but absent in the local tree:', absent[:10]); return 6
    return 5 if extra else 0


def receipt(out, tip, sha, compiler, build_log, source):
    value = dict(commit=tip, executable_sha256=sha, compiler=compiler, build_log=build_log, source=source,
                 stamp=dt.datetime.now(dt.timezone(dt.timedelta(hours=-7))).strftime('%Y-%m-%d %H:%M:%S MST'))
    Path(out).write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')
    print(f'receipt {out} commit={tip[:10]} exe={sha[:16]}')


def load(tools_dir, lane, guard):
    sys.path.insert(0, str(Path(tools_dir).resolve()))
    import cross_peers as c
    c.set_lane(lane)
    c.MAC_GUARD = guard or None
    import inspect
    path = Path(os.environ.get('CC_CROSS_PEERS_BOXES', str(Path(tools_dir) / 'cross_peers/boxes.json')))
    boxes = c.load_boxes(path, 'four-way') if 'roster' in inspect.signature(c.load_boxes).parameters else c.load_boxes(path)
    return c, boxes


def resolve(tools_dir, lane, guard):
    c, manifest_ = load(tools_dir, lane, guard)
    keys = {'windows-local': 'EROL', 'windows-task': 'EDITH'}
    kinds = [box['kind'] for box in manifest_['boxes']]
    posix = {box['name'] for box in manifest_['boxes'] if box['kind'] == 'posix-ssh'}
    if kinds.count('windows-local') != 1 or kinds.count('windows-task') != 1 or posix not in ({'Mac'}, {'Mac', 'Linux'}):
        raise SystemExit(f'expected EROL-PC, EDITH, Mac, and optional Linux, got {kinds}/{sorted(posix)}')
    print(f'BOX_COUNT={len(manifest_["boxes"])}')
    for box in manifest_['boxes']:
        key = keys.get(box['kind'], box['name'].upper())
        for field in ('name', 'ssh', 'tree', 'executable', 'python', 'scratch', 'runner'):
            print(f'{key}_{field.upper()}={shlex.quote(str(box.get(field, "")))}')


def preflight(tools_dir, lane, guard, work):
    # The harness's --preflight on each remote box, then the coordinator's checks (cross_peers.py run_plan) against EROL-PC's
    # tree computed here without the launch guards (no engine is launched).
    import platform
    c, manifest_ = load(tools_dir, lane, guard)
    work = Path(work)
    boxes = manifest_['boxes']
    local = next(box for box in boxes if box['kind'] == 'windows-local')
    tree = Path(local['tree'])
    identity = c.command(['pwsh', '-NoProfile', '-Command', '(Get-CimInstance Win32_ComputerSystemProduct).UUID']).strip()
    content = c.content_manifest(tree)
    ref = dict(head=c.command(['git', '-C', tree, 'rev-parse', 'HEAD']).strip(),
               build=json.loads((tree / 'tools/cross_peers/build.json').read_text(encoding='utf-8')),
               executable_sha256=c.digest_file(local['executable']), content=content,
               modules={p: h for p, h in content.items() if p.endswith('/Index.ini')},
               fixture={n: c.digest_file(tree / 'tools/feel' / n) for n in ('CrossCombat.lua', 'CrossCombat.ini')},
               machine_id=hashlib.sha256((platform.system() + ':' + identity).encode()).hexdigest())
    values, failed = {local['name']: ref}, []
    for box in boxes:
        if box['kind'] == 'windows-local': continue
        rdir = str(PurePosixPath(box['scratch']) / 'refresh-preflight')
        mkdir = (f'New-Item -ItemType Directory -Force -Path {c.quote_ps(rdir)} | Out-Null' if box['kind'] == 'windows-task'
                 else f'mkdir -p {shlex.quote(rdir)}')
        try:
            c.command(['ssh', box['ssh'], mkdir])
            payload = work / f'preflight-payload-{box["name"]}.json'
            c.write_json(payload, dict(box=box, specs=[], pin='pending'))
            c.stage_remote(box, payload, rdir + '/payload.json')
            print(c.command(c.remote_command(box, [box['python'], box['tree'] + '/tools/cross_peers.py', '--preflight',
                                                    rdir + '/payload.json']), timeout=180).strip())
            got = work / f'preflight-{box["name"]}.json'
            c.command(['scp', '-q', f'{box["ssh"]}:{rdir}/preflight.json', str(got)], timeout=120)
            values[box['name']] = json.loads(got.read_text(encoding='utf-8'))
        except Exception as error:
            print(f'PREFLIGHT FAIL {box["name"]}: {error}')
            failed.append(box['name'])
    if not failed and len({v['machine_id'] for v in values.values()}) != len(boxes):
        print('PREFLIGHT FAIL machines: each declared box must be a distinct real machine')
        failed.append('machines')
    for box in boxes:
        value = values.get(box['name'])
        if value is None: continue
        reasons = []
        for key in ('content', 'modules', 'fixture'):
            if value[key] != ref[key]:
                diff = sorted(p for p in set(value[key]) | set(ref[key]) if value[key].get(p) != ref[key].get(p))
                reasons.append(f'{key} differs ({len(diff)}: {diff[:5]})')
        if box['kind'].startswith('windows') and value['executable_sha256'] != ref['executable_sha256']:
            reasons.append('Windows executable hash differs')
        build = value['build']
        if build.get('commit') != ref['head'] or build.get('executable_sha256') != value['executable_sha256']:
            reasons.append(f'no build receipt tying this executable to {ref["head"]} (receipt {str(build.get("commit"))[:10]}'
                           f'/{str(build.get("executable_sha256"))[:16]}, executable {value["executable_sha256"][:16]})')
        if reasons:
            print(f'PREFLIGHT FAIL {box["name"]}: ' + '; '.join(reasons))
            failed.append(box['name'])
        else:
            print(f'preflight {box["name"]}: PASS executable={value["executable_sha256"][:16]} receipt={build["commit"][:10]} '
                  f'data_files={len(value["content"])}')
    return 3 if failed else 0


if __name__ == '__main__':
    name, *args = sys.argv[1:]
    sys.exit(dict(manifest=manifest, plan=plan, receipt=receipt, resolve=resolve, preflight=preflight)[name](*args) or 0)
PYEOF
RESOLVED=$(python "$PY" resolve "$AW/tools" "$LANE" "$GUARD" 2>&1) || fail usage "boxes.json: $RESOLVED" 4
eval "${RESOLVED//$'\r'/}"
pyout() { python "$@" | tr -d '\r'; }   # native Python writes CRLF into a pipe
say "refresh start tip=$TIP lane=$LANE work=$WORK"

# EROL-PC: the ladder has built alias-walk at the tip and written its receipt; everything shipped comes from this tree.
HEAD_AW=$(git -C "$AW" rev-parse HEAD)
[ "$HEAD_AW" = "$TIP" ] || fail EROL-PC "alias-walk HEAD ${HEAD_AW:0:10} is not the tip $T10" 4
git -C "$AW" diff --quiet HEAD -- tools Data || fail EROL-PC "alias-walk has local edits under tools/ or Data/" 4
AWSHA=$(sha256sum "$EROL_EXECUTABLE" | cut -d' ' -f1)
AWREC=$(python -c "import json,sys; r=json.load(open(sys.argv[1],encoding='utf-8')); print(r.get('commit',''), r.get('executable_sha256',''))" "$AW/tools/cross_peers/build.json" 2>/dev/null)
[ "$AWREC" = "$TIP $AWSHA" ] || fail EROL-PC "alias-walk receipt '$AWREC' does not tie the executable ${AWSHA:0:16} to the tip" 4
AWCOMP=$(python -c "import json,sys; print(json.load(open(sys.argv[1],encoding='utf-8')).get('compiler',''))" "$AW/tools/cross_peers/build.json")
AWLOG=$(python -c "import json,sys; print(json.load(open(sys.argv[1],encoding='utf-8')).get('build_log',''))" "$AW/tools/cross_peers/build.json")
say "EROL-PC alias-walk exe=$AWSHA receipt at $T10"

# The Mac half runs detached on the Mac (a ~10-minute ninja must not depend on this ssh session) while EDITH is refreshed.
MW="$MAC_SCRATCH/refresh"
cat > "$WORK/mac_refresh.zsh" <<'ZEOF'
#!/bin/zsh
# cross_refresh_boxes.sh's Mac half: the tip in the harness's clone, ninja, and the receipt from the binary's measured hash.
set -u
export PATH=/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:$PATH
TIP=$1 REPO=$2 EXE=$3 OUT=$4
T=${TIP[1,10]}
LOG=$OUT/steps-$T.log NLOG=$OUT/ninja-$T.log EXITF=$OUT/exit-$T.txt
BUILD=${EXE:h}
stamp() { TZ=America/Phoenix date '+%Y-%m-%d %H:%M:%S MST'; }
say() { echo "[$(stamp)] $*" >> $LOG; }
finish() { say "done exit=$1 $2"; echo "$1 $2" > $EXITF; exit $1; }
: > $LOG
say "start tip=$TIP repo=$REPO"
cd $REPO || finish 1 "no repo $REPO"
busy=x
for i in {1..40}; do
  busy=""
  for p in $(pgrep -x ninja); do
    cwd=$(lsof -a -p $p -d cwd -Fn 2>/dev/null | sed -n 's/^n//p')
    [[ $cwd == $REPO* ]] && busy="$busy ninja:$p"
  done
  for p in $(pgrep -x CortexCommand); do
    [[ $(ps -o command= -p $p) == $EXE* ]] && busy="$busy engine:$p"
  done
  [[ -z $busy ]] && break
  say "busy:$busy; waiting"; sleep 30
done
[[ -n $busy ]] && finish 1 "clone busy after 20 minutes:$busy"
if ! git cat-file -e "${TIP}^{commit}" 2>/dev/null; then
  REF=$(git ls-remote origin 2>>$LOG | awk -v t=$TIP '$1==t && $2 ~ /^refs\/heads\// {print $2; exit}')
  ok=0
  for i in 1 2 3 4 5 6; do
    if [[ -n $REF ]]; then git fetch -q origin "+${REF}:refs/remotes/origin/${REF#refs/heads/}" >> $LOG 2>&1 && ok=1
    else git fetch -q origin $TIP >> $LOG 2>&1 && ok=1; fi
    [[ $ok == 1 ]] && break
    say "fetch attempt $i failed"; sleep 20
  done
  [[ $ok == 1 ]] || finish 1 "fetch of $TIP failed (ref ${REF:-none})"
  say "fetched ${REF:-$TIP}"
fi
was=$(git rev-parse HEAD)
if [[ $was != $TIP ]]; then git checkout -q --detach $TIP >> $LOG 2>&1 || finish 1 "checkout $TIP failed"; fi
say "HEAD=$(git rev-parse HEAD) (was ${was[1,10]})"
ninja -C $BUILD > $NLOG 2>&1; rc=$?
nowork=$(grep -c 'no work to do' $NLOG)
say "ninja exit=$rc errors=$(grep -c ' error: ' $NLOG) last: $(tail -1 $NLOG | cut -c1-120)"
[[ $rc == 0 ]] || finish 2 "ninja exit=$rc (log $NLOG)"
[[ -x $EXE ]] || finish 2 "no binary $EXE"
SHA=$(shasum -a 256 $EXE | cut -d' ' -f1)
CXX=$(awk '$1=="command" && $3 ~ /g\+\+/ {print $3; exit}' $BUILD/build.ninja)
COMP=$($CXX --version 2>/dev/null | head -1)
REC=$REPO/tools/cross_peers/build.json
old=$(/opt/homebrew/bin/python3 -c 'import json,sys; r=json.load(open(sys.argv[1])); print(r.get("commit",""), r.get("executable_sha256",""))' $REC 2>/dev/null)
if [[ $old == "$TIP $SHA" ]]; then say "receipt already ties $SHA to the tip"
else /opt/homebrew/bin/python3 $OUT/refresh_helper.py receipt $REC $TIP $SHA "$COMP" $NLOG "ninja -C ${BUILD:t} in this clone at the tip" >> $LOG 2>&1 || finish 2 "receipt write failed"; fi
say "binary $SHA compiler $COMP"
noop=0; [[ $was == $TIP && $nowork -gt 0 && $old == "$TIP $SHA" ]] && noop=1
finish 0 "sha=$SHA noop=$noop"
ZEOF
MACRC=0; MACWHY=""
if ssh -o BatchMode=yes "$MAC_SSH" "mkdir -p '$MW' && rm -f '$MW/exit-$T10.txt'" \
   && (cd "$WORK" && scp -q mac_refresh.zsh refresh_helper.py "$MAC_SSH:$MW/") \
   && ssh -o BatchMode=yes "$MAC_SSH" "nohup /bin/zsh '$MW/mac_refresh.zsh' $TIP '$MAC_TREE' '$MAC_EXECUTABLE' '$MW' > '$MW/run-$T10.log' 2>&1 < /dev/null &"; then
  say "Mac: build launched in $MAC_TREE (log $MW/steps-$T10.log)"
else
  MACRC=2; MACWHY="could not stage or launch the Mac half over ssh $MAC_SSH"
fi

# Linux refresh uses the same measured receipt and guarded clone as the Mac.
LINUXRC=0; LINUXWHY=""; lsha=""
if [ -n "${LINUX_TREE:-}" ]; then
  LW="$LINUX_SCRATCH/refresh"
  cat > "$WORK/linux_refresh.sh" <<'LEOF'
#!/bin/bash
# The Linux receipt names the binary measured after its build.
set -u
export PATH=$HOME/.local/bin:/usr/local/bin:/usr/bin:/bin:$PATH
TIP=$1 REPO=$2 EXE=$3 OUT=$4
T=${TIP:0:10}
LOG=$OUT/steps-$T.log NLOG=$OUT/ninja-$T.log EXITF=$OUT/exit-$T.txt
BUILD=$(dirname "$EXE")
stamp() { TZ=America/Phoenix date '+%Y-%m-%d %H:%M:%S MST'; }
say() { echo "[$(stamp)] $*" >> $LOG; }
finish() { say "done exit=$1 $2"; echo "$1 $2" > $EXITF; exit $1; }
: > $LOG
say "start tip=$TIP repo=$REPO"
cd $REPO || finish 1 "no repo $REPO"
busy=x
for i in {1..40}; do
  busy=""
  for p in $(pgrep -x ninja); do
    cwd=$(readlink /proc/$p/cwd 2>/dev/null)
    [[ $cwd == $REPO* ]] && busy="$busy ninja:$p"
  done
  for p in $(pgrep -x CortexCommand); do
    [[ $(ps -o command= -p $p) == $EXE* ]] && busy="$busy engine:$p"
  done
  [[ -z $busy ]] && break
  say "busy:$busy; waiting"; sleep 30
done
[[ -n $busy ]] && finish 1 "clone busy after 20 minutes:$busy"
if ! git cat-file -e "${TIP}^{commit}" 2>/dev/null; then
  REF=$(git ls-remote origin 2>>$LOG | awk -v t=$TIP '$1==t && $2 ~ /^refs\/heads\// {print $2; exit}')
  ok=0
  for i in 1 2 3 4 5 6; do
    if [[ -n $REF ]]; then git fetch -q origin "+${REF}:refs/remotes/origin/${REF#refs/heads/}" >> $LOG 2>&1 && ok=1
    else git fetch -q origin $TIP >> $LOG 2>&1 && ok=1; fi
    [[ $ok == 1 ]] && break
    say "fetch attempt $i failed"; sleep 20
  done
  [[ $ok == 1 ]] || finish 1 "fetch of $TIP failed (ref ${REF:-none})"
  say "fetched ${REF:-$TIP}"
fi
was=$(git rev-parse HEAD)
if [[ $was != $TIP ]]; then git checkout -q --detach $TIP >> $LOG 2>&1 || finish 1 "checkout $TIP failed"; fi
say "HEAD=$(git rev-parse HEAD) (was ${was:0:10})"
ninja -C $BUILD -j8 > $NLOG 2>&1; rc=$?
nowork=$(grep -c 'no work to do' $NLOG)
say "ninja exit=$rc errors=$(grep -c ' error: ' $NLOG) last: $(tail -1 $NLOG | cut -c1-120)"
[[ $rc == 0 ]] || finish 2 "ninja exit=$rc (log $NLOG)"
[[ -x $EXE ]] || finish 2 "no binary $EXE"
SHA=$(shasum -a 256 $EXE | cut -d' ' -f1)
COMP=$(g++ --version | head -1)
REC=$REPO/tools/cross_peers/build.json
old=$(/usr/bin/python3 -c 'import json,sys; r=json.load(open(sys.argv[1])); print(r.get("commit",""), r.get("executable_sha256",""))' $REC 2>/dev/null)
if [[ $old == "$TIP $SHA" ]]; then say "receipt already ties $SHA to the tip"
else /usr/bin/python3 $OUT/refresh_helper.py receipt $REC $TIP $SHA "$COMP" $NLOG "ninja -C ${BUILD##*/} in this clone at the tip" >> $LOG 2>&1 || finish 2 "receipt write failed"; fi
say "binary $SHA compiler $COMP"
noop=0; [[ $was == $TIP && $nowork -gt 0 && $old == "$TIP $SHA" ]] && noop=1
finish 0 "sha=$SHA noop=$noop"
LEOF
  if ssh -o BatchMode=yes "$LINUX_SSH" "mkdir -p '$LW' && rm -f '$LW/exit-$T10.txt'" \
     && (cd "$WORK" && scp -q linux_refresh.sh refresh_helper.py "$LINUX_SSH:$LW/") \
     && ssh -o BatchMode=yes "$LINUX_SSH" "nohup /bin/bash '$LW/linux_refresh.sh' $TIP '$LINUX_TREE' '$LINUX_EXECUTABLE' '$LW' > '$LW/run-$T10.log' 2>&1 < /dev/null &"; then
    say "Linux refresh started on $LINUX_SSH"
  else LINUXRC=5; LINUXWHY="could not start the Linux refresh"; fi
fi

# EDITH: wait for an idle box, compare hashes, ship the differences, verify, write the receipt.
edith() { ssh -o BatchMode=yes "$EDITH_SSH" "$@" 2>&1 | tr -d '\r' | grep -v '^\*\* \|^$'; }   # drops OpenSSH's key-exchange notice
refresh_edith() {
  local ew="$EDITH_SCRATCH/refresh" rec="$EDITH_TREE/tools/cross_peers/build.json" st i info rcommit rsha base
  for i in $(seq 1 21); do
    st=$(edith "\$p=@(Get-Process 'Cortex Command*' -ErrorAction SilentlyContinue); \$t=(Get-ScheduledTask -TaskName '$EDITH_RUNNER').State; if (\$p.Count -gt 0 -or ('' + \$t) -eq 'Running') { 'busy engines=' + \$p.Count + ' task=' + \$t } else { 'idle' }")
    [ "$st" = "idle" ] && break
    case $st in busy*) ;; *) EDWHY="EDITH unreachable over ssh $EDITH_SSH ('$st')"; return 1 ;; esac
    [ "$i" = 21 ] && { EDWHY="EDITH still busy after 20 minutes ($st)"; return 1; }
    say "EDITH $st; waiting"; sleep 60
  done
  info=$(edith "if (Test-Path -LiteralPath '$EDITH_EXECUTABLE') { (Get-FileHash -LiteralPath '$EDITH_EXECUTABLE').Hash.ToLower() } else { 'missing' }; \$r='$rec'; if (Test-Path -LiteralPath \$r) { \$j=Get-Content -Raw -LiteralPath \$r | ConvertFrom-Json; 'commit=' + \$j.commit; 'sha=' + \$j.executable_sha256 } else { 'commit=none'; 'sha=none' }")
  esha=$(echo "$info" | sed -n 1p); rcommit=$(echo "$info" | sed -n 's/^commit=//p'); rsha=$(echo "$info" | sed -n 's/^sha=//p')
  [ -n "$esha" ] || { EDWHY="EDITH status query returned nothing"; return 1; }
  say "EDITH before: exe=${esha:0:16} receipt=${rcommit:0:10}/${rsha:0:16}"
  if [ "$esha" = "$AWSHA" ] && [ "$rcommit" = "$TIP" ] && [ "$rsha" = "$esha" ]; then
    say "EDITH no-op: executable and receipt already at the tip"; return 0
  fi
  base=${PREV:-$rcommit}
  if git -C "$AW" cat-file -e "$base^{commit}" 2>/dev/null; then
    say "EDITH git diff ${base:0:10}..$T10 under tools/ Data/: $(git -C "$AW" diff --name-only --diff-filter=ACMR "$base" "$TIP" -- tools Data | wc -l) changed, $(git -C "$AW" diff --name-only --diff-filter=D "$base" "$TIP" -- tools Data | wc -l) deleted (the ship list is by hash)"
  fi
  git -C "$AW" ls-files -- tools | grep -v '/__pycache__/' > "$WORK/tools-list.txt"
  edith "New-Item -ItemType Directory -Force -Path '$ew' | Out-Null" > /dev/null
  (cd "$WORK" && scp -q refresh_helper.py tools-list.txt "$EDITH_SSH:$ew/") || { EDWHY="scp of the helper to EDITH failed"; return 1; }
  python "$PY" manifest "$AW" "$WORK/tools-list.txt" "$WORK/erol-manifest.json" || { EDWHY="local manifest failed"; return 1; }
  local pass
  for pass in ship verify; do
    edith "& '$EDITH_PYTHON' '$ew/refresh_helper.py' manifest '$EDITH_TREE' '$ew/tools-list.txt' '$ew/edith-manifest.json'"
    (cd "$WORK" && rm -f edith-manifest.json && scp -q "$EDITH_SSH:$ew/edith-manifest.json" edith-manifest.json) || { EDWHY="EDITH manifest ($pass) not fetched"; return 1; }
    python "$PY" plan "$WORK/erol-manifest.json" "$WORK/edith-manifest.json" "$WORK/ship-$T10.list"
    local prc=$?
    [ "$prc" = 0 ] || { EDWHY="EDITH ship plan exit $prc (files on EDITH absent at the tip, or tracked tools files absent here; no file is deleted on EDITH)"; return 1; }
    if [ "$pass" = verify ]; then
      [ -s "$WORK/ship-$T10.list" ] && { EDWHY="files still differ on EDITH after the copy: $(head -3 "$WORK/ship-$T10.list" | paste -sd, -)"; return 1; }
      break
    fi
    if [ ! -s "$WORK/ship-$T10.list" ]; then say "EDITH: nothing differs"; break; fi
    (cd "$AW" && "C:/Windows/System32/tar.exe" -cf "$WORK/ship-$T10.tar" -T "$WORK/ship-$T10.list") || { EDWHY="tar of the ship list failed"; return 1; }
    say "EDITH ship: $(wc -l < "$WORK/ship-$T10.list") files, $(du -k "$WORK/ship-$T10.tar" | cut -f1) KB"
    (cd "$WORK" && scp -q "ship-$T10.tar" "$EDITH_SSH:$ew/") || { EDWHY="scp of the ship tar failed"; return 1; }
    rm -f "$WORK/ship-$T10.tar"
    st=$(edith "tar.exe -xf '$ew/ship-$T10.tar' -C '$EDITH_TREE'; 'tar_x rc=' + \$LASTEXITCODE; Remove-Item -LiteralPath '$ew/ship-$T10.tar'")
    say "EDITH $st"
    [ "$st" = "tar_x rc=0" ] || { EDWHY="extraction on EDITH: $st"; return 1; }
  done
  esha=$(edith "(Get-FileHash -LiteralPath '$EDITH_EXECUTABLE').Hash.ToLower()")
  say "EDITH exe measured on EDITH  : $esha"
  say "EROL-PC alias-walk exe       : $AWSHA"
  [ "$esha" = "$AWSHA" ] || { EDWHY="executable hash after the copy $esha != alias-walk $AWSHA"; return 1; }
  python "$PY" receipt "$WORK/edith-build.json" "$TIP" "$esha" "$AWCOMP" "$AWLOG" "EROL-PC $EROL_EXECUTABLE, copied to EDITH; hash measured on EDITH" > /dev/null
  (cd "$WORK" && scp -q edith-build.json "$EDITH_SSH:$rec.incoming") || { EDWHY="scp of the receipt failed"; return 1; }
  info=$(edith "Move-Item -LiteralPath '$rec.incoming' -Destination '$rec' -Force; \$j=Get-Content -Raw -LiteralPath '$rec' | ConvertFrom-Json; 'commit=' + \$j.commit; 'sha=' + \$j.executable_sha256")
  rcommit=$(echo "$info" | sed -n 's/^commit=//p'); rsha=$(echo "$info" | sed -n 's/^sha=//p')
  say "EDITH receipt read back: commit=$rcommit exe=$rsha"
  [ "$rcommit" = "$TIP" ] && [ "$rsha" = "$esha" ] || { EDWHY="receipt read back '$rcommit/$rsha'"; return 1; }
  return 0
}
EDWHY=""; esha=""; msha=""; refresh_edith; EDRC=$?

# The Mac: wait for its half (the ~10-minute incremental build), then re-measure the binary and read the receipt from here.
if [ "$MACRC" = 0 ]; then
  r=""
  for i in $(seq 1 135); do
    r=$(ssh -o BatchMode=yes "$MAC_SSH" "cat '$MW/exit-$T10.txt' 2>/dev/null")
    [ -n "$r" ] && break
    sleep 20
  done
  ssh -o BatchMode=yes "$MAC_SSH" "cat '$MW/steps-$T10.log'" | sed 's/^/  mac: /'
  if [ -z "$r" ]; then MACRC=2; MACWHY="the Mac half did not finish in 45 minutes"
  elif [ "${r%% *}" != 0 ]; then MACRC=2; MACWHY="Mac half: $r"
  else
    msha=$(ssh -o BatchMode=yes "$MAC_SSH" "shasum -a 256 '$MAC_EXECUTABLE'" | cut -d' ' -f1)
    mrec=$(ssh -o BatchMode=yes "$MAC_SSH" "/opt/homebrew/bin/python3 -c 'import json,sys; r=json.load(open(sys.argv[1])); print(r.get(\"commit\",\"\"), r.get(\"executable_sha256\",\"\"))' '$MAC_TREE/tools/cross_peers/build.json'")
    say "Mac binary measured on the Mac: $msha"
    say "Mac receipt                  : $mrec"
    [ "$mrec" = "$TIP $msha" ] || { MACRC=2; MACWHY="receipt '$mrec' does not tie the binary $msha to the tip"; }
  fi
fi
if [ -n "${LINUX_TREE:-}" ] && [ "$LINUXRC" = 0 ]; then
  r=""
  for i in $(seq 1 135); do
    r=$(ssh -o BatchMode=yes "$LINUX_SSH" "cat '$LW/exit-$T10.txt' 2>/dev/null")
    [ -n "$r" ] && break
    sleep 20
  done
  ssh -o BatchMode=yes "$LINUX_SSH" "cat '$LW/steps-$T10.log'" | sed 's/^/  linux: /'
  if [ -z "$r" ]; then LINUXRC=5; LINUXWHY="Linux refresh did not finish in 45 minutes"
  elif [ "${r%% *}" != 0 ]; then LINUXRC=5; LINUXWHY="Linux refresh: $r"
  else
    lsha=$(ssh -o BatchMode=yes "$LINUX_SSH" "sha256sum '$LINUX_EXECUTABLE'" | cut -d' ' -f1)
    lrec=$(ssh -o BatchMode=yes "$LINUX_SSH" "/usr/bin/python3 -c 'import json,sys; r=json.load(open(sys.argv[1])); print(r.get(\"commit\",\"\"), r.get(\"executable_sha256\",\"\"))' '$LINUX_TREE/tools/cross_peers/build.json'")
    say "Linux binary measured on Linux: $lsha"
    say "Linux receipt: $lrec"
    [ "$lrec" = "$TIP $lsha" ] || { LINUXRC=5; LINUXWHY="receipt '$lrec' does not tie the binary $lsha to the tip"; }
  fi
fi
[ "$LINUXRC" = 0 ] || { say "REFRESH FAIL Linux: $LINUXWHY"; exit 5; }

[ "$EDRC" = 0 ] || say "REFRESH FAIL EDITH: $EDWHY"
[ "$MACRC" = 0 ] || say "REFRESH FAIL Mac: $MACWHY"
[ "$EDRC" = 0 ] || exit 1
[ "$MACRC" = 0 ] || exit 2

# Parity: the harness's preflight on EDITH and the Mac, the coordinator's checks against EROL-PC's tree.
python "$PY" preflight "$AW/tools" "$LANE" "$GUARD" "$WORK" > "$WORK/preflight-$T10.raw" 2>&1; frc=$?
tr -d '\r' < "$WORK/preflight-$T10.raw" > "$WORK/preflight-$T10.txt"; cat "$WORK/preflight-$T10.txt"
if [ "$frc" != 0 ] || [ "$(grep -c ': PASS executable=' "$WORK/preflight-$T10.txt")" != "$BOX_COUNT" ]; then
  boxes=$(grep -o '^PREFLIGHT FAIL [A-Za-z0-9-]*' "$WORK/preflight-$T10.txt" | awk '{print $3}' | sort -u | paste -sd, -)
  fail "${boxes:-preflight}" "the dry preflight is red (exit $frc; the PREFLIGHT FAIL lines above)" 3
fi
say "REFRESH PASS tip=$T10 EROL-PC=${AWSHA:0:16} EDITH=${esha:0:16} Mac=${msha:0:16} Linux=${lsha:0:16}"
exit 0
