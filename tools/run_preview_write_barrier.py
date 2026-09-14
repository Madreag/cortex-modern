"""Prepare or execute the preview barrier RED, compatibility and cost matrix."""
from __future__ import annotations

import argparse
from collections import Counter
import ctypes as C
from ctypes import wintypes as W
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

sys.dont_write_bytecode = True

REPO = Path('D:/Projects/fencing-warm')
LANE = Path('D:/mx/astra-f44-write-barrier-20260914')
ROOT = LANE / 'phase-b'
BASE = '6e8a59e117'
RED = '0aad92fe55e7622f8d5dc64625fea893df443edd'
REFERENCE = Path('D:/mx/opus-f44-20260914/binaries/final-bdf7e0bb.exe')
REFERENCE_SHA = 'bdf7e0bbe9d95170b1464938224a12efccc65e513f60147794e17301339c6653'
LOCKS = [Path('D:/mx') / name for name in ('LEAD_FAMILY.lock', 'LEAD_EXCLUSIVE.lock', 'LEAD_BATTERY.lock')]
FIXTURES = Path('D:/Projects/stage2_p4/fixtures')
LEGACY_FIXTURES = ('Data/Tests.rte', 'tools/pie_lockstep/fixtures', 'tools/pie_writes/fixtures')
PORT_LO, PORT_HI = 48250, 48259
LIMIT_MS = 0.5
STATS = re.compile(r'\[localpred\] previews=(\d+) actor_ticks=\d+ ms_total=([\d.]+) avg_ms=([\d.]+)')
NATIVE = re.compile(r'\[preview-write-barrier\] (.*)')
FAIL = re.compile(r'^\[script-graph-selftest\] FAIL(?: (.*))?$', re.M)
OBSERVE = re.compile(r'^\[(?:pie-observe|pie-write-observe|pie-write|preview-module-fixture|preview-compat)[^\]]*\].*$', re.M)
DRIVER_SHA = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()


def git(*args):
    return subprocess.check_output(['git', '-C', str(REPO), *args])


def stamp():
    return subprocess.check_output(['date', '+%Y-%m-%d %H:%M:%S MST'],
                                  text=True, env=dict(os.environ, TZ='MST7')).strip()


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')


def inventory():
    class Entry(C.Structure):
        _fields_ = [('size', W.DWORD), ('usage', W.DWORD), ('pid', W.DWORD), ('heap', C.c_size_t),
                    ('module', W.DWORD), ('threads', W.DWORD), ('parent', W.DWORD), ('priority', W.LONG),
                    ('flags', W.DWORD), ('name', W.WCHAR * 260)]
    kernel = C.WinDLL('kernel32', use_last_error=True)
    kernel.CreateToolhelp32Snapshot.argtypes = [W.DWORD, W.DWORD]
    kernel.CreateToolhelp32Snapshot.restype = W.HANDLE
    kernel.Process32FirstW.argtypes = kernel.Process32NextW.argtypes = [W.HANDLE, C.POINTER(Entry)]
    kernel.Process32FirstW.restype = kernel.Process32NextW.restype = W.BOOL
    kernel.CloseHandle.argtypes = [W.HANDLE]
    handle = kernel.CreateToolhelp32Snapshot(2, 0)
    if handle == C.c_void_p(-1).value:
        raise C.WinError(C.get_last_error())
    rows, entry = [], Entry()
    entry.size = C.sizeof(entry)
    try:
        more = kernel.Process32FirstW(handle, C.byref(entry))
        while more:
            if entry.name.lower() in ('cl.exe', 'link.exe') or entry.name.lower().startswith('cortex command'):
                rows.append({'pid': int(entry.pid), 'name': entry.name})
            more = kernel.Process32NextW(handle, C.byref(entry))
    finally:
        kernel.CloseHandle(handle)
    return rows


def footprint():
    total = 0
    for parent, dirs, files in os.walk(LANE, followlinks=False):
        dirs[:] = [d for d in dirs if not (Path(parent) / d).is_junction() and not (Path(parent) / d).is_symlink()]
        total += sum((Path(parent) / f).stat().st_size for f in files)
    if total >= 2_000_000_000:
        raise RuntimeError(f'lane scratch cap reached: {total} bytes')
    return total


def locks_clear():
    locked = [str(path) for path in LOCKS if path.exists()]
    if locked:
        raise RuntimeError('machine remains reserved: ' + ', '.join(locked))


def gate():
    deadline = time.monotonic() + 1800
    while True:
        locks_clear()
        busy = inventory()
        if not busy:
            footprint()
            return
        if time.monotonic() >= deadline:
            raise RuntimeError('timeout waiting for build/engine processes: ' + json.dumps(busy))
        print(stamp(), 'waiting for', busy, flush=True)
        time.sleep(20)


def fixture_hashes():
    files = [path for folder in LEGACY_FIXTURES for path in sorted((REPO / folder).rglob('*')) if path.is_file()]
    files += [FIXTURES / (name + suffix) for name in ('pickup_fire', 'ak47_fire') for suffix in ('.txt', '.ccreplay')]
    files += [REPO / 'tools/fixtures' / name for name in ('preview_write_barrier.lua', 'preview_barrier_240.lua', 'preview_barrier_240.txt')]
    return {str(path).replace('\\', '/'): sha(path) for path in files}


def ledger(action, **fields):
    ROOT.mkdir(parents=True, exist_ok=True)
    entry = dict(fields)
    entry.update(date=stamp(), action=action, driver_sha256=DRIVER_SHA)
    with (ROOT / 'exe-ledger.jsonl').open('a', encoding='utf-8') as stream:
        stream.write(json.dumps(entry) + '\n')


def restore_build_products():
    changed = git('diff', '--name-only').decode().splitlines()
    unexpected = [p for p in changed if not (p.startswith('external/') and p.endswith('.lib'))]
    if unexpected:
        raise RuntimeError('build changed source files: ' + repr(unexpected))
    for name in changed:
        before = sha(REPO / name)
        (REPO / name).write_bytes(git('show', 'HEAD:' + name))
        ledger('restore tracked build product', path=name, built_sha256=before, restored_sha256=sha(REPO / name))


def build(label, revision):
    gate()
    if git('status', '--porcelain').strip():
        raise RuntimeError('build requires a clean committed tree')
    subprocess.run(['git', '-C', str(REPO), 'checkout', '--detach', revision], check=True)
    vswhere = Path('C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe')
    msbuild = subprocess.check_output([str(vswhere), '-latest', '-requires', 'Microsoft.Component.MSBuild',
                                      '-find', 'MSBuild/**/Bin/MSBuild.exe'], text=True).splitlines()[0]
    command = [msbuild, str(REPO / 'RTEA.sln'), '/t:RTEA:Rebuild', '/p:Configuration=Final',
               '/p:Platform=x64', '/p:PreferredToolArchitecture=x64', '/m:1', '/nologo', '/v:minimal']
    env = dict(os.environ, CL='/MP6', CCCP_HEADLESS='1',
               GNS_ROOT='D:/Projects/stage2_p2/gns_spike/install-win-vcpkg-release',
               GNS_DEP_ROOT='D:/Projects/stage2_p2/gns_spike/build-win-vcpkg-release/vcpkg_installed/x64-windows')
    folder = ROOT / 'build' / label
    folder.mkdir(parents=True, exist_ok=False)
    record = dict(date=stamp(), revision=git('rev-parse', 'HEAD').decode().strip(), command=command, CL='/MP6')
    write(folder / 'command.json', record)
    with (folder / 'build.log').open('w', encoding='utf-8') as log:
        result = subprocess.run(command, cwd=REPO, env=env, stdout=log, stderr=subprocess.STDOUT)
    record['exit_code'] = result.returncode
    restore_build_products()
    if result.returncode == 0:
        for name in ('Cortex Command.exe', 'Cortex Command.pdb'):
            shutil.copyfile(REPO / name, folder / name)
            record[name + '_sha256'] = sha(folder / name)
    write(folder / 'result.json', record)
    ledger('build', **record)
    if result.returncode:
        raise RuntimeError(f'{label} build failed: {folder / "build.log"}')
    return folder / 'Cortex Command.exe'


def select_binary(label, binary):
    gate()
    wanted = sha(binary)
    shutil.copyfile(binary, REPO / 'Cortex Command.exe')
    symbols = binary.with_suffix('.pdb')
    if symbols.exists():
        shutil.copyfile(symbols, REPO / 'Cortex Command.pdb')
    actual = sha(REPO / 'Cortex Command.exe')
    ledger('select', label=label, source=str(binary), wanted=wanted, actual=actual)
    if actual != wanted:
        raise RuntimeError('selected executable hash differs')


def install(run, files, activity=None):
    folder = Path(run.cwd) / 'Userdata/UserScenes.rte'
    folder.mkdir(exist_ok=True)
    for name, path in files.items():
        (folder / name).write_bytes(Path(path).read_bytes())
    index = 'DataModule\n\tModuleName = User Scenes\n\tScanFolderContents = 1\n\tIgnoreMissingItems = 1\n'
    if activity:
        index += ('\tAddActivity = GAScripted\n\t\tPresetName = Determinism ' + activity + '\n'
                  '\t\tSceneName = Grasslands\n\t\tScriptPath = UserScenes.rte/preview_barrier_240.lua\n'
                  '\t\tLuaClassName = ' + activity + '\n\t\tMinTeamsRequired = 2\n'
                  '\t\tIsTestActivity = 1\n\t\tDefaultRequireClearPathToOrbit = 0\n'
                  '\t\tDefaultFogOfWar = 0\n\t\tDefaultDeployUnits = 0\n')
    (folder / 'Index.ini').write_text(index, encoding='utf-8')


def run_case(name, flags, files=None, activity=None, quiet=False):
    gate()
    from run_sim_test import make_run
    out = ROOT / name
    identity = sha(REPO / 'Cortex Command.exe')
    args = [*map(str, flags), '-out', str(out / 'trace.json')]
    env = {'CCCP_HEADLESS': '1', 'CC_PREVIEW_BARRIER_STATS': '1'}
    run = make_run(REPO, args, out, timeout=900, env=env)
    if files:
        install(run, files, activity)
    interference = []
    try:
        run.start()
        if quiet:
            while run.poll() is None:
                locks_clear()
                others = [p for p in inventory() if p['pid'] != run.record['pid']]
                if others:
                    interference.append(dict(date=stamp(), processes=others))
                time.sleep(0.25)
        record = run.finish()
    finally:
        run.close()
    text = (out / 'stdout.log').read_text(encoding='utf-8-sig', errors='replace')
    console = out / 'runtime/LogConsole.txt'
    observations = OBSERVE.findall(console.read_text(encoding='utf-8-sig', errors='replace') if console.exists() else text)
    stats = STATS.findall(text)
    native = [{k: float(v) for k, v in re.findall(r'(\w+)=([\d.eE+-]+)', row)} for row in NATIVE.findall(text)]
    same = sha(REPO / 'Cortex Command.exe') == identity
    row = dict(date=stamp(), name=name, identity=identity, flags=args, driver_sha256=DRIVER_SHA,
               exit_code=record.get('exit_code'), complete=record.get('evidence_complete'),
               timed_out=record.get('timed_out'), binary_unchanged=same,
               desktop_unchanged=record.get('input_desktop_before') == record.get('input_desktop_after'),
               preview_ms=float(stats[-1][2]) if stats else None,
               previews=int(stats[-1][0]) if stats else 0, native=native, interference=interference,
               graph_failures=FAIL.findall(text), observations=observations,
               verdicts=[line for line in text.splitlines() if any(tag in line for tag in ('[lpinv]', '[script-graph-selftest]', '[preview-event-selftest]'))])
    row['transport_ok'] = row['complete'] is True and row['timed_out'] is not True and same and row['desktop_unchanged']
    write(out / 'row.json', row)
    ledger('run', name=name, identity=identity, binary_unchanged=same, exit_code=row['exit_code'])
    print(name, 'exit', row['exit_code'], 'preview_ms', row['preview_ms'], flush=True)
    return row


def replay_flags(fixture='pickup_fire', spec='153:1,4,7,12:1,3', modes=None):
    flags = ['-net-replay', FIXTURES / (fixture + '.ccreplay'), '-input-script', FIXTURES / (fixture + '.txt'),
             '-tick-hashes', '-max-ticks', 400 if fixture == 'ak47_fire' else 221,
             '-num-lua-states', 4, '-local-prediction-depth', 7, '-local-prediction-invariance', spec]
    if modes:
        flags += ['-lpinv-overlay-links', modes]
    return flags


def import_driver(relative):
    path = REPO / relative
    spec = importlib.util.spec_from_file_location(path.stem, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def call_main(module, args):
    saved = sys.argv
    try:
        sys.argv = [module.__file__, *map(str, args)]
        return module.main()
    finally:
        sys.argv = saved


def compatibility(label):
    for scenario in ('LuaBaseline', 'LuaPairsStress', 'LuaRandomStress', 'LuaOsStubTest'):
        run_case(f'{label}/scenario-{scenario}', ['-scenario', scenario, '-seed', 42,
                 '-max-ticks', 600, '-num-lua-states', 4, '-tick-hashes'])
    run_case(f'{label}/module', [*replay_flags(), '-test-script', 'UserScenes.rte/PreviewModuleCompat.lua'],
             {'PreviewModuleCompat.lua': REPO / 'Data/Tests.rte/PreviewModuleCompat.lua'})
    pie = import_driver('tools/pie_lockstep/run_arm.py')
    pie.LANE_PORTS = ((PORT_LO, PORT_HI),)
    for case in ('next', 'prev', 'goto', 'actor_cancel', 'delivery_cancel'):
        gate()
        identity = sha(REPO / 'Cortex Command.exe')
        status = call_main(pie, [case, ROOT / label / ('pie-' + case), '--sp', '--observe', '--port', PORT_LO])
        ledger('fixture', name=label + '/pie-' + case, identity=identity, binary_unchanged=sha(REPO / 'Cortex Command.exe') == identity, exit_code=status)
    buy = import_driver('tools/pie_writes/run_write_arm.py')
    buy.PORT_LO, buy.PORT_HI = PORT_LO, PORT_HI
    for index, case in enumerate(buy.CASES):
        gate()
        out = ROOT / label / ('buy-' + case)
        identity = sha(REPO / 'Cortex Command.exe')
        status = call_main(buy, [case, out, '--port', PORT_LO + 1 + index])
        ledger('fixture', name=label + '/buy-' + case, identity=identity, binary_unchanged=sha(REPO / 'Cortex Command.exe') == identity, exit_code=status)
        checker = import_driver('tools/pie_writes/verify_peer_pie.py')
        checker.PORT_LO, checker.PORT_HI = PORT_LO, PORT_HI
        call_main(checker, [case, out, '--out', out / 'check.json'])


def read_row(name):
    return json.loads((ROOT / name / 'row.json').read_text())


def trace_values(folder):
    run = json.loads((folder / 'trace.json').read_text())['runs'][0]
    return dict(passed=run['passed'], strings=run['strings'], ticks=run['ticks'],
                numeric={key: value for key, value in run['numeric'].items() if key != '__wall_seconds'})


def fixture_output(folder, peer):
    path = folder / peer / 'runtime/LogConsole.txt'
    return OBSERVE.findall(path.read_text(encoding='utf-8-sig', errors='replace'))


def assess_cost(red, green, scene, red_census, census):
    native = green.get('native', [])
    valid = bool(native) and all(all(key in row and math.isfinite(row[key]) and row[key] >= 0
                                   for key in ('capture_ms', 'write_ms', 'restore_ms', 'max_ms'))
                                 and row.get('windows', 0) > 0 and row['capture_ms'] > 0 and row['restore_ms'] > 0 for row in native)
    maximum = sum(row['max_ms'] for row in native) if valid else None
    delta = green['preview_ms'] - red['preview_ms'] if all(r.get('preview_ms') is not None for r in (red, green)) else None
    ok = all(r['transport_ok'] and not r['interference'] and r['previews'] > 0 and
             r['preview_ms'] is not None and math.isfinite(r['preview_ms']) for r in (red, green))
    ok = ok and green['exit_code'] == 0 and maximum is not None and maximum < LIMIT_MS
    ok = ok and delta is not None and delta < LIMIT_MS
    ok = ok and (census == red_census == 240 if scene == '240' else red['exit_code'] == 0)
    return dict(scene=scene, red_ms=red['preview_ms'], green_ms=green['preview_ms'], delta_ms=delta,
                native_max_ms_sum=maximum, actors=census, red_actors=red_census, pass_check=ok)


def assess_selftests(summary, identity):
    from run_selftests import SELFTESTS
    rows = summary.get('results', {})
    return (summary.get('passed') == summary.get('total') == len(SELFTESTS) == 13 and
            summary.get('exe_sha256') == identity and set(rows) == set(SELFTESTS) and
            all(row.get('pass') is True and row.get('binary') == identity for row in rows.values()))


def score():
    checks = {}
    identities = {'reference': REFERENCE_SHA}
    for label in ('red', 'green'):
        identities[label] = json.loads((ROOT / 'build' / label / 'result.json').read_text())['Cortex Command.exe_sha256']
    for path in ROOT.rglob('row.json'):
        if 'runtime' in path.parts:
            continue
        row = json.loads(path.read_text())
        label = next((part for part in path.relative_to(ROOT).parts if part in identities), None)
        if label is None:
            label = path.parent.name.split('-')[0]
        checks['identity_' + row['name']] = row['identity'] == identities[label] and row['transport_ok']
    red, green = read_row('red/graph'), read_row('green/graph')
    checks['fresh_red'] = red['transport_ok'] and red['exit_code'] == 1 and 'preview_deep_global_writes_undone' in red['graph_failures']
    checks['green_graph'] = green['transport_ok'] and green['exit_code'] == 0 and not green['graph_failures']
    checks['barrier_rounds'] = sum('PASS preview_barrier_exact_rollback ' in line for line in green['verdicts']) == 10
    suites = json.loads((ROOT / 'green/selftests/result.json').read_text())
    checks['selftests_13'] = assess_selftests(suites, identities['green'])
    for name in ('f39', 'letters', 'links-r', 'links-t', 'event-d7', 'lpinv-100'):
        a, b = read_row('red/' + name), read_row('green/' + name)
        def verdicts(row):
            result = Counter()
            for line in row['verdicts']:
                if (m := re.match(r'\[lpinv\] (PASS|FAIL|ok) (.*)', line)):
                    parts = m[2].split(':')
                    result[m[1], ':'.join(parts[:2] if m[2].startswith('overlay-links ') else parts[:1])] += 1
            return result
        checks['unchanged_' + name] = a['transport_ok'] and b['transport_ok'] and a['exit_code'] == b['exit_code'] and verdicts(a) == verdicts(b)
        if name == 'event-d7':
            checks['event_d7_reached'] = all(r['exit_code'] == 0 and any('[preview-event-selftest] PASS' in line for line in r['verdicts']) for r in (a, b))
        else:
            checks['reached_' + name] = bool(verdicts(a)) and bool(verdicts(b))
    for scenario in ('LuaBaseline', 'LuaPairsStress', 'LuaRandomStress', 'LuaOsStubTest'):
        rows = [trace_values(ROOT / label / ('scenario-' + scenario)) for label in ('reference', 'red', 'green')]
        runs_ok = all(read_row(label + '/scenario-' + scenario)['exit_code'] == 0 for label in ('reference', 'red', 'green'))
        checks['fixture_' + scenario] = runs_ok and rows[0]['passed'] and rows[0] == rows[1] == rows[2]
    rows = [read_row(label + '/module') for label in ('reference', 'red', 'green')]
    checks['fixture_module'] = all(r['transport_ok'] and r['exit_code'] == 0 for r in rows) and bool(rows[0]['observations']) and rows[0]['observations'] == rows[1]['observations'] == rows[2]['observations']
    sys.path.insert(0, str(REPO / 'tools/pie_lockstep'))
    pie_compare = import_driver('tools/pie_lockstep/compare_sp.py')
    for case in ('next', 'prev', 'goto', 'actor_cancel', 'delivery_cancel'):
        for label in ('red', 'green'):
            result = pie_compare.compare(case, ROOT / 'reference' / ('pie-' + case), ROOT / label / ('pie-' + case))
            write(ROOT / 'comparisons' / f'{label}-pie-{case}.json', result)
            checks[f'fixture_{label}_pie_{case}'] = result['pass_check']
    for case in ('buy_menu', 'form_squad', 'full_inventory'):
        for peer in ('host', 'client'):
            outputs = [fixture_output(ROOT / label / ('buy-' + case), peer) for label in ('reference', 'red', 'green')]
            checks[f'fixture_buy_{case}_{peer}'] = bool(outputs[0]) and outputs[0] == outputs[1] == outputs[2]
        checks['buy_reached_' + case] = all(json.loads((ROOT / label / ('buy-' + case) / 'check.json').read_text())['pass_check'] for label in ('reference', 'red', 'green'))
    costs = []
    for repeat in (1, 2, 3):
        for scene in ('retained', '240'):
            a, b = (read_row(f'cost-{scene}/{label}-{repeat}') for label in ('red', 'green'))
            before = ROOT / f'cost-{scene}/green-{repeat}/trace.json.lpinv_before.simstate.txt'
            census = len(re.findall(r'^\d+ actor ', before.read_text(), re.M))
            red_before = ROOT / f'cost-{scene}/red-{repeat}/trace.json.lpinv_before.simstate.txt'
            red_census = len(re.findall(r'^\d+ actor ', red_before.read_text(), re.M))
            cost = assess_cost(a, b, scene, red_census, census)
            cost['repeat'] = repeat
            costs.append(cost)
            checks[f'cost_{scene}_{repeat}'] = cost['pass_check']
    checks['fixtures_unchanged'] = fixture_hashes() == json.loads((ROOT / 'fixtures.json').read_text())
    result = dict(date=stamp(), pass_check=all(checks.values()), checks=checks, costs=costs)
    write(ROOT / 'result.json', result)
    return 0 if result['pass_check'] else 1


def phase_b(authorized):
    if not authorized:
        raise RuntimeError('requires the explicit FAMILY ENDED — BUILD resume')
    gate()
    if ROOT.exists():
        raise RuntimeError('use a new --out directory; retained evidence is never overwritten')
    if git('status', '--porcelain').strip():
        raise RuntimeError('Phase B requires a clean committed tree')
    if sha(REFERENCE) != REFERENCE_SHA:
        raise RuntimeError('retained reference hash differs')
    if git('diff', BASE, '--', *LEGACY_FIXTURES).strip():
        raise RuntimeError('legacy fixtures differ from the wave base')
    if git('diff', RED, '--', 'Source/Managers/PreviewScriptSelfTest.cpp', 'Source/Managers/PreviewScriptSelfTest.h').strip():
        raise RuntimeError('RED and GREEN regression fixtures differ')
    branch = git('branch', '--show-current').decode().strip()
    tip = git('rev-parse', 'HEAD').decode().strip()
    write(ROOT / 'fixtures.json', fixture_hashes())
    write(ROOT / 'authorization.json', dict(date=stamp(), message='FAMILY ENDED — BUILD', branch=branch, tip=tip, red=RED))
    green_binary = None
    try:
        red_binary = build('red', RED)
        run_case('red/graph', ['-script-graph-selftest', '-num-lua-states', 4])
        green_binary = build('green', tip)
        subprocess.run(['git', '-C', str(REPO), 'checkout', branch], check=True)
        for label, binary in (('reference', REFERENCE), ('red', red_binary), ('green', green_binary)):
            select_binary(label, binary)
            if label == 'green':
                run_case('green/graph', ['-script-graph-selftest', '-num-lua-states', 4])
                gate()
                command = [sys.executable, str(REPO / 'tools/run_selftests.py'), '--repo', str(REPO),
                           '--out', str(ROOT / 'green/selftests'), '--timeout', '300']
                subprocess.run(command, check=False, env=dict(os.environ, CCCP_HEADLESS='1'))
            if label != 'reference':
                for name, flags in (
                    ('f39', replay_flags('ak47_fire', '176:1,4,7,12:1')),
                    ('letters', replay_flags(spec='200:1:1', modes='brcitmqxonlap')),
                    ('links-r', replay_flags(spec='135:1:1', modes='r')),
                    ('links-t', replay_flags(spec='135:1:1', modes='t')),
                    ('lpinv-100', replay_flags(spec='100:1,4,7,12:1,3')),
                    ('event-d7', ['-net-replay', FIXTURES / 'pickup_fire.ccreplay', '-input-script', FIXTURES / 'pickup_fire.txt',
                                  '-max-ticks', 221, '-num-lua-states', 4, '-tick-hashes', '-local-prediction-depth', 7,
                                  '-local-prediction-event-ledger', 153])):
                    run_case(label + '/' + name, flags)
            compatibility(label)
        for repeat in (1, 2, 3):
            for label, binary in (('red', red_binary), ('green', green_binary)):
                select_binary(label, binary)
                run_case(f'cost-retained/{label}-{repeat}', replay_flags(modes='p'), quiet=True)
                run_case(f'cost-240/{label}-{repeat}', ['-scenario', 'PreviewBarrier240', '-seed', 42, '-max-ticks', 221,
                         '-num-lua-states', 4, '-tick-hashes', '-input-script', REPO / 'tools/fixtures/preview_barrier_240.txt',
                         '-local-prediction-depth', 7, '-local-prediction-invariance', '153:1,4,7,12:1,3'],
                         {name: REPO / 'tools/fixtures' / name for name in ('preview_barrier_240.lua', 'preview_write_barrier.lua')},
                         activity='PreviewBarrier240', quiet=True)
        return score()
    finally:
        restore_build_products()
        subprocess.run(['git', '-C', str(REPO), 'checkout', branch], check=True)
        if green_binary:
            select_binary('green-final', green_binary)


def main():
    global ROOT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['plan', 'phase-b', 'score'])
    parser.add_argument('--family-ended-build', action='store_true')
    parser.add_argument('--out', type=Path, default=ROOT)
    args = parser.parse_args()
    ROOT = args.out.resolve()
    if not ROOT.is_relative_to(LANE.resolve()) or ROOT == LANE.resolve():
        parser.error('--out must be a child of the assigned lane scratch directory')
    sys.path.insert(0, str(REPO / 'tools'))
    if args.action == 'plan':
        plan = dict(date=stamp(), status='PHASE A unbuilt', red=RED, reference=str(REFERENCE), reference_sha256=sha(REFERENCE),
                    driver_sha256=DRIVER_SHA, family_locked=LOCKS[0].exists(), budget_ms=LIMIT_MS,
                    command=['python', str(REPO / 'tools/run_preview_write_barrier.py'), 'phase-b', '--family-ended-build', '--out', str(ROOT)],
                    fixture_sha256=fixture_hashes(), retained_cost_actor_count=4, new_cost_actor_count_required=240)
        write(LANE / 'phase-b-plan.json', plan)
        print(json.dumps({k: v for k, v in plan.items() if k != 'fixture_sha256'}, indent=2))
        return 0
    return phase_b(args.family_ended_build) if args.action == 'phase-b' else score()


if __name__ == '__main__':
    raise SystemExit(main())
