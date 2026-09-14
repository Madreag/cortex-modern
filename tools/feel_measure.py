"""Run the fixed unattended feel matrix on private desktops, retaining every raw record."""
from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import time

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from feel.report import TICKS, file_record, reduce_peer, write_json
from run_sim_test import make_run
from compare_sim_traces import strict_compare

REPO = Path(__file__).resolve().parents[1]
SCRATCH = Path('D:/mx/astra-feel-measure-20260913')
HELPERS = REPO / 'tools/feel'
SP_CONTROL = Path('D:/mx/opus-f24-20260913/sp-control')
SP_COMPARATOR = Path('D:/Projects/reviews/takeover-20260909/grok-workers/opus-f24-first-update-20260913/scripts/compare_sp.py')
BYTE_LIMIT = 5_000_000_000


def stamp():
    environment = dict(os.environ, TZ='America/Phoenix')
    return subprocess.check_output(['date', '+%Y-%m-%d %H:%M MST'], env=environment, text=True).strip()


def scratch_bytes(root):
    total, pending = 0, [Path(root)]
    while pending:
        directory = pending.pop()
        if not directory.exists():
            continue
        with os.scandir(directory) as entries:
            for entry in entries:
                info = entry.stat(follow_symlinks=False)
                if getattr(info, 'st_file_attributes', 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
                    continue
                if stat.S_ISDIR(info.st_mode):
                    pending.append(Path(entry.path))
                elif stat.S_ISREG(info.st_mode):
                    total += info.st_size
    if total >= BYTE_LIMIT:
        raise RuntimeError(f'scratch footprint {total} bytes reaches the {BYTE_LIMIT} byte limit; no cleanup performed')
    return total


def private_settings(run, cap):
    path = Path(run.cwd) / 'Userdata/Settings.ini'
    text = path.read_text(encoding='utf-8-sig')
    values = {'EnableVSync': '0', 'LocalPrediction': '1', 'LocalPredictionMaxTicks': '20'}
    for name, value in values.items():
        text, count = re.subn(rf'(?m)^(\s*{name}\s*=\s*)[^\r\n]*', lambda match: match[1] + value, text)
        if count == 0:
            text += f'\n\t{name} = {value}\n'
    path.write_text(text, encoding='utf-8')
    render_path = Path(run.cwd) / 'Userdata/FeelRender.ini'
    render_path.write_text(f'RenderCapHz = {cap}\n', encoding='utf-8')
    manifest = json.loads((Path(run.out) / 'runtime.json').read_text(encoding='utf-8'))
    manifest['settings_overrides'].update(values)
    manifest['settings_sha256'] = file_record(path)['sha256']
    manifest['feel_render_settings'] = file_record(render_path)
    write_json(Path(run.out) / 'runtime.json', manifest)


def stage_baseline(run):
    module = Path(run.cwd) / 'Userdata/UserScenes.rte'
    module.mkdir(exist_ok=True)
    (module / 'FeelBaseline.lua').write_bytes((HELPERS / 'FeelBaseline.lua').read_bytes())
    (module / 'Index.ini').write_text(
        'DataModule\n\tModuleName = User Scenes\n\tScanFolderContents = 1\n\tIgnoreMissingItems = 1\n'
        '\tAddActivity = GAScripted\n\t\tCopyOf = P4 Alpha Duel\n'
        '\t\tPresetName = Determinism FeelBaseline\n\t\tScriptPath = UserScenes.rte/FeelBaseline.lua\n'
        '\t\tLuaClassName = FeelBaseline\n\t\tIsTestActivity = 1\n'
        '\t\tTeamOfPlayer1 = 1\n\t\tPlayer1IsHuman = 1\n'
        '\t\tTeamOfPlayer2 = 0\n\t\tPlayer2IsHuman = 0\n', encoding='utf-8')


def input_pattern(path):
    rows, probes = [], []
    aims = ['0.8,0.6'] * (TICKS + 1)
    for tick in range(180, 1100, 120):
        rows.append(f'{tick} {tick + 2} FIRE')
        for at in range(tick, tick + 3):
            aims[at] = '0.6,0.8'
        direction = 'L_LEFT' if (tick // 120) % 2 else 'L_RIGHT'
        rows.append(f'{tick + 30} {tick + 59} {direction}')
        probes += [dict(tick=tick, action='FIRE', held=True), dict(tick=tick + 3, action='FIRE', held=False),
                   dict(tick=tick, action='AIM_VECTOR', held=True), dict(tick=tick + 3, action='AIM_VECTOR', held=True),
                   dict(tick=tick + 30, action=direction, held=True), dict(tick=tick + 60, action=direction, held=False)]
    first = 1
    for tick in range(2, TICKS + 2):
        if tick > TICKS or aims[tick] != aims[first]:
            rows.append(f'{first} {tick - 1} AIM={aims[first]}')
            first = tick
    path.write_text('\n'.join(rows) + '\n', encoding='utf-8')
    write_json(path.with_name('input-schedule.json'), dict(probes=probes, initial_aim='0.8,0.6', fire_presses=8))


def launch_case(root, name, lag, cap, record, port, script, exe_hash, timeout, sp=False):
    out = root / name
    out.mkdir(exist_ok=False)
    manifest = dict(started=stamp(), mode='local single-player P4 Alpha Duel' if sp else 'two-peer service e2e, normal render loop',
                    ticks=TICKS, lag_ms=lag, cap_hz=cap, instrumentation=record, port=None if sp else port,
                    auto_input_delay=not sp, input_script=file_record(script), input_schedule=file_record(script.with_name('input-schedule.json')),
                    exe=file_record(REPO / 'Cortex Command.exe'))
    write_json(out / 'manifest.json', manifest)
    peers = ['sp'] if sp else ['host', 'client']
    runs, records = {}, {}
    try:
        for peer in peers:
            run_out = out / peer
            trace = out / f'{peer}_trace.json'
            flags = ['-seed', '42', '-max-ticks', str(TICKS), '-tick-hashes', '-num-lua-states', '4',
                     '-out', str(trace), '-input-script', str(script),
                     '-controller-debug-dump', str(out / f'{peer}_controller.jsonl'),
                     '-controller-debug-ticks', f'1-{TICKS}',
                     '-feel-render-settings', str(run_out / 'runtime/Userdata/FeelRender.ini')]
            if record:
                flags += ['-feel-measure', str(run_out / 'feel')]
            if sp:
                flags += ['-scenario', 'FeelBaseline', '-controller-log-out', str(out / 'controllers.json')]
            else:
                flags += ['-net-match-service-e2e', '-net-port', str(port), '-net-match-ticks', str(TICKS),
                          '-net-match-auto-delay', '-net-fake-lag', str(lag), '-net-local-prediction', 'on',
                          '-net-match-report', str(out / f'{peer}_report.json')]
                flags += ['-net-host', '-net-replay-out', str(out / 'match.ccreplay')] if peer == 'host' else ['-net-join', '127.0.0.1']
            environment = dict(CCCP_HEADLESS='1', CC_TRACE_PREVIEW_EVENT='1', CC_SIM_DUMP=f'1:{TICKS}', PYTHONDONTWRITEBYTECODE='1')
            run = make_run(REPO, flags, run_out, timeout=timeout, env=environment,
                           expected=[trace, Path(str(trace) + '.simdump.txt'), out / f'{peer}_controller.jsonl'])
            runs[peer] = run
            private_settings(run, cap)
            if record:
                (run_out / 'feel').mkdir()
            if sp:
                stage_baseline(run)
            run.start()
            if not sp and peer == 'host':
                time.sleep(.75)
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(runs)) as executor:
            jobs = {executor.submit(run.finish): peer for peer, run in runs.items()}
            for job in concurrent.futures.as_completed(jobs):
                records[jobs[job]] = job.result()
    finally:
        for run in runs.values():
            run.close()
        for peer, run in runs.items():
            records[peer] = run.record
        write_json(out / 'run-result.json', records)
    if any(record.get('exe_sha256') != exe_hash for record in records.values()):
        raise RuntimeError('the executable changed during the matrix')
    manifest.update(finished=stamp(), scratch_bytes=scratch_bytes(SCRATCH),
                    launches_complete=all(row.get('exit_code') == 0 and row.get('evidence_complete') and not row.get('timed_out') for row in records.values()))
    write_json(out / 'manifest.json', manifest)
    print(f'{manifest["finished"]} {name}: launches_complete={manifest["launches_complete"]}', flush=True)
    if not sp and record and (out / 'match.ccreplay').is_file():
        inspect = make_run(REPO, ['-net-replay-verify', str(out / 'match.ccreplay'), '-net-replay-dump', f'1:{TICKS}',
                                 '-out', str(out / 'replay-report.json')], out / 'replay-inspect', timeout=timeout,
                           env={'CCCP_HEADLESS': '1'}, expected=[out / 'replay-report.json'])
        try:
            inspect.start().finish()
        finally:
            inspect.close()
    return out


def compare_pair(first, second):
    result = dict(first=str(first), second=str(second))
    ok, existing = strict_compare(first, second, expected_ticks=TICKS)
    result.update(sim_gated_pass=ok, existing_comparator=existing)
    try:
        left = json.loads(first.read_text(encoding='utf-8-sig'))['runs'][0]['tick_hashes']
        right = json.loads(second.read_text(encoding='utf-8-sig'))['runs'][0]['tick_hashes']
        exact_coverage = [row['tick'] for row in left] == [row['tick'] for row in right] == list(range(1, TICKS + 1))
        result['all_tick_hashes_identical'] = exact_coverage and left == right
        result['first_full_row_difference'] = next((a['tick'] for a, b in zip(left, right) if a != b), None)
    except (OSError, ValueError, KeyError, IndexError) as error:
        result.update(all_tick_hashes_identical=False, error=str(error))
    result['pass'] = bool(ok and result['all_tick_hashes_identical'])
    return result


def value_text(value):
    if isinstance(value, float):
        return f'{value:.3f}'
    return json.dumps(value, separators=(',', ':'), allow_nan=False)


def summarize_case(report, out):
    lines = [f'Measured {stamp()}', '', report['mode'], '',
             '| Pinned number | Host value / result | Client value / result |', '|---|---|---|']
    peers = report['peers']
    for key in peers['client']['pins']:
        cells = []
        for peer in ('host', 'client'):
            row = peers[peer]['pins'][key]
            value = row['value']
            if isinstance(value, dict) and 'max' in value:
                value = value['max']
            cells.append(f'{value_text(value)} / {row["status"]}')
        lines.append(f'| {key} | {cells[0]} | {cells[1]} |')
    lines += ['', f'Measurement complete: {report["measurement_complete"]}. Off-wire proof: {report["off_wire_pass"]}.',
              '', 'Raw files and their hashes are in feel-report.json. Per-edge matches, every kinematic residual,',
              'all commit comparisons and firing matches are under each peer/analysis directory.', '',
              'The presentation boundary is UploadFrame return on the private desktop. Audio times are conservative',
              'preview-step bounds for logged voice starts; sound output stays muted. Same-frame remote commands',
              'are candidates, and a correction with unproven cause remains a MISS.']
    (out / 'summary.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')


def analyze(root):
    baselines = {}
    for cap_name in ('60hz', 'uncapped'):
        run = root / f'baseline-{cap_name}'
        result = reduce_peer(run, 'sp')
        baselines[cap_name] = dict(result['metrics'], raw_path=result['raw_path'])
        write_json(run / 'feel-report.json', result)
    results = []
    for lag in (100, 200):
        for cap_name in ('60hz', 'uncapped'):
            name = f'{lag}ms-{cap_name}'
            on, off = root / (name + '-on'), root / (name + '-off')
            manifest = json.loads((on / 'manifest.json').read_text(encoding='utf-8'))
            peers = {peer: reduce_peer(on, peer, baselines[cap_name]) for peer in ('host', 'client')}
            proof = {'peers_on': compare_pair(on / 'host_trace.json', on / 'client_trace.json'),
                     'peers_off': compare_pair(off / 'host_trace.json', off / 'client_trace.json'),
                     **{peer + '_on_off': compare_pair(on / f'{peer}_trace.json', off / f'{peer}_trace.json') for peer in ('host', 'client')}}
            write_json(on / 'hash-proof.json', proof)
            raw_paths = [on / 'manifest.json', on / 'run-result.json', on / 'match.ccreplay', on / 'replay-report.json',
                         on / 'replay-inspect/stdout.log', off / 'manifest.json', off / 'run-result.json',
                         Path(baselines[cap_name]['raw_path']), Path(manifest['input_script']['path']), Path(manifest['input_schedule']['path'])]
            for peer in ('host', 'client'):
                raw_paths += [on / peer / 'feel/raw.jsonl', on / f'{peer}_controller.jsonl',
                              on / f'{peer}_trace.json', on / f'{peer}_trace.json.simdump.txt', on / f'{peer}_report.json', on / peer / 'stdout.log',
                              on / peer / 'launch.json', on / peer / 'runtime.json',
                              off / f'{peer}_trace.json', off / peer / 'launch.json']
                raw_paths += sorted((on / peer / 'feel').glob('*.png'))
            report = dict(name=name, mode=manifest['mode'], measured=stamp(), executable=manifest['exe'],
                          reducer=file_record(HELPERS / 'report.py'), driver=file_record(Path(__file__)),
                          peers=peers, proof=proof, off_wire_pass=all(row['pass'] for row in proof.values()),
                          measurement_complete=manifest['launches_complete'] and all(row['measurement_complete'] for row in peers.values()),
                          raw_files=[file_record(path) for path in raw_paths if path.is_file()],
                          missing_raw_files=[str(path) for path in raw_paths if not path.is_file()])
            report['measurement_complete'] &= not report['missing_raw_files']
            write_json(on / 'feel-report.json', report)
            summarize_case(report, on)
            results.append(report)
    write_json(root / 'matrix-report.json', results)
    lines = [f'Measured {stamp()}', '', '| Configuration | Raw measurements complete | Off-wire proof | Findings |', '|---|---|---|---|']
    for report in results:
        misses = sum(row['status'] == 'MISS' for peer in report['peers'].values() for row in peer['pins'].values())
        lines.append(f'| {report["name"]} | {report["measurement_complete"]} | {report["off_wire_pass"]} | {misses} MISS; [{report["name"]}]({report["name"]}-on/summary.md) |')
    lines += ['', 'Every pinned threshold is unchanged. A numerical MISS is a finding; missing records and failed',
              'determinism proofs remain incomplete work. The full per-peer table and raw-file manifest are in each run.']
    (root / 'summary.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    return results


def gates(root, control, timeout):
    out = root / 'gates'
    out.mkdir(exist_ok=False)
    env = dict(os.environ, CCCP_HEADLESS='1', PYTHONDONTWRITEBYTECODE='1')
    command = [sys.executable, '-B', str(REPO / 'tools/run_selftests.py'), '--repo', str(REPO),
               '--out', str(out / 'selftests'), '--timeout', str(timeout)]
    with (out / 'selftests-driver.log').open('w', encoding='utf-8') as log:
        suite = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, env=env)
    graph = make_run(REPO, ['-script-graph-selftest', '-num-lua-states', '4'], out / 'script-graph', timeout=timeout, env=env)
    try:
        graph_record = graph.start().finish()
    finally:
        graph.close()
    control_launch = json.loads((control / 'launch.json').read_text(encoding='utf-8'))
    if control_launch.get('exit_code') != 0 or control_launch.get('timed_out') or not control_launch.get('evidence_complete'):
        raise ValueError('the SP control did not complete')
    argv = control_launch['argv'][1:]
    argv = [value for value in argv if value != '-headless']
    if argv[argv.index('-scenario') + 1] != 'PieSwitchSP' or any(value.startswith('-feel') or value.startswith('-net') for value in argv):
        raise ValueError('the SP control is not the uninstrumented pie-close fixture')
    sp = out / 'sp'
    argv[argv.index('-out') + 1] = str(sp / 'trace.json')
    run = make_run(REPO, argv, sp, timeout=timeout, env=dict(CCCP_HEADLESS='1', CC_SIM_DUMP='27:320'),
                   expected=[sp / 'trace.json', sp / 'trace.json.simdump.txt'])
    module = Path(run.cwd) / 'Userdata/UserScenes.rte'
    module.mkdir(exist_ok=True)
    for name in ('Index.ini', 'PieSwitchSP.lua'):
        (module / name).write_bytes((Path(control_launch['cwd']) / 'Userdata/UserScenes.rte' / name).read_bytes())
    try:
        record = run.start().finish()
    finally:
        run.close()
    write_json(sp / 'sp_summary.json', {key: record.get(key) for key in ('pid', 'exit_code', 'timed_out', 'evidence_complete', 'cwd', 'verdict_lines')})
    compare_command = [sys.executable, '-B', str(SP_COMPARATOR), str(control), str(sp), str(out / 'sp-comparison.json')]
    with (out / 'sp-comparison.log').open('w', encoding='utf-8') as log:
        compared = subprocess.run(compare_command, stdout=log, stderr=subprocess.STDOUT, env=env)
    suite_json = json.loads((out / 'selftests/result.json').read_text(encoding='utf-8'))
    graph_text = (out / 'script-graph/stdout.log').read_text(encoding='utf-8-sig', errors='replace')
    result = dict(measured=stamp(), selftests_command=command, selftests=suite_json,
                  selftests_pass=suite.returncode == 0 and suite_json.get('passed') == 13 and suite_json.get('total') == 13,
                  script_graph_pass=graph_record.get('exit_code') == 0 and not graph_record.get('timed_out')
                  and bool(re.search(r'\bPASS\b', graph_text)) and not re.search(r'\bFAIL\b', graph_text),
                  sp_comparator=file_record(SP_COMPARATOR), sp_compare_command=compare_command,
                  sp_control_dump=file_record(control / 'trace.json.simdump.txt'), sp_compare_pass=compared.returncode == 0)
    write_json(out / 'gates.json', result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=SCRATCH)
    parser.add_argument('--port', type=int, default=48231)
    parser.add_argument('--timeout', type=float, default=600)
    parser.add_argument('--analyze-only', action='store_true')
    parser.add_argument('--skip-gates', action='store_true', help='retain gates as unverified')
    parser.add_argument('--sp-control', type=Path, default=SP_CONTROL)
    args = parser.parse_args()
    root = args.out.resolve()
    if sys.platform != 'win32' or REPO.resolve() != Path('D:/Projects/value-observations').resolve():
        parser.error('this driver belongs to the assigned Windows worktree')
    if not root.is_relative_to(SCRATCH.resolve()):
        parser.error('output must stay under the assigned scratch root')
    if not 48231 <= args.port <= 48242:
        parser.error('the eight pair ports must stay within 48231..48249')
    if (Path('D:/mx/LEAD_FAMILY.lock')).exists():
        parser.error('Phase 1 lock is present; no driver or engine launch is permitted')
    branch = subprocess.check_output(['git', '-C', str(REPO), 'branch', '--show-current'], text=True).strip()
    if branch != 'stage2/feel-measurement':
        parser.error('unexpected branch: ' + branch)
    os.environ.update(CCCP_HEADLESS='1', PYTHONDONTWRITEBYTECODE='1')
    scratch_bytes(SCRATCH)
    if not args.analyze_only:
        root.mkdir(parents=True, exist_ok=True)
        if (root / 'matrix-plan.json').exists():
            parser.error('matrix-plan.json already exists; retain it and use a fresh child output directory')
        exe = file_record(REPO / 'Cortex Command.exe')
        plan = dict(started=stamp(), exe=exe, branch=branch,
                    commit=subprocess.check_output(['git', '-C', str(REPO), 'rev-parse', 'HEAD'], text=True).strip(),
                    source=file_record(Path(__file__)), reducer=file_record(HELPERS / 'report.py'),
                    ports=list(range(args.port, args.port + 8)), ticks=TICKS,
                    mode='service e2e without -free-run-sim; the normal loop presents every render iteration',
                    captures='own -feel-measure seam; frame-<requested tick>.png after UploadFrame')
        write_json(root / 'matrix-plan.json', plan)
        script = root / 'input.txt'
        input_pattern(script)
        for cap, cap_name in ((60, '60hz'), (0, 'uncapped')):
            launch_case(root, 'baseline-' + cap_name, 0, cap, True, 0, script, exe['sha256'], args.timeout, sp=True)
        port = args.port
        for lag in (100, 200):
            for cap, cap_name in ((60, '60hz'), (0, 'uncapped')):
                for enabled in (True, False):
                    name = f'{lag}ms-{cap_name}-' + ('on' if enabled else 'off')
                    launch_case(root, name, lag, cap, enabled, port, script, exe['sha256'], args.timeout)
                    port += 1
    results = analyze(root)
    gate_result = None if args.skip_gates else gates(root, args.sp_control, args.timeout)
    complete = all(row['measurement_complete'] and row['off_wire_pass'] for row in results)
    gate_pass = bool(gate_result and all(gate_result[key] for key in ('selftests_pass', 'script_graph_pass', 'sp_compare_pass')))
    completion = dict(finished=stamp(), measurement_complete=complete, gates_pass=gate_pass,
                      scratch_bytes=scratch_bytes(SCRATCH), gates_unverified=args.skip_gates)
    write_json(root / 'completion.json', completion)
    with (root / 'summary.md').open('a', encoding='utf-8') as stream:
        stream.write(f'\nGates passed: {gate_pass}. See gates/gates.json and completion.json.\n')
    print(json.dumps(completion, indent=2), flush=True)
    return 0 if complete and gate_pass else 1


if __name__ == '__main__':
    raise SystemExit(main())
