"""Run the fixed unattended feel matrix on private desktops, retaining every raw record."""
from __future__ import annotations

import argparse
import concurrent.futures
from datetime import datetime, timedelta, timezone
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
from feel.report import EarlyDecision, TICKS, file_record, pin, record_path, reduce_peer, item9a_gates, apply_tps_call, write_json
from feel.retained_resume import PER_PEER_SUBSYSTEMS, compare_live_hashes
from feel.records import compress_case_records, record_path
from run_sim_test import make_run, engine_executable
from run_selftests import SELFTESTS
from compare_sim_traces import compare_fullstate, strict_compare

REPO = Path(__file__).resolve().parents[1]
MST = timezone(timedelta(hours=-7))
HELPERS = REPO / 'tools/feel'
SP_CONTROL = Path('D:/mx/opus-f24-20260913/sp-control')
SP_COMPARATOR = Path('D:/Projects/reviews/takeover-20260909/grok-workers/opus-f24-first-update-20260913/scripts/compare_sp.py')
BYTE_LIMIT = 5_000_000_000
MATRIX_BYTE_LIMIT = 10_000_000_000
# Every N committed ticks each match peer hashes its whole capture (-net-fullstate-hash-every); 0 is off. Set by --fullstate-every.
FULLSTATE_EVERY = 0
LAG_ARMS = tuple(f'{lag}ms-{cap}' for lag in (100, 200) for cap in ('60hz', 'uncapped'))


def stamp():
    return datetime.now(MST).strftime('%Y-%m-%d %H:%M MST')


def scratch_bytes(root, limit=BYTE_LIMIT):
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
    if total >= limit:
        raise RuntimeError(f'scratch footprint {total} bytes reaches the {limit} byte limit; no cleanup performed')
    return total


def private_settings(run, cap):
    path = Path(run.cwd) / 'Userdata/Settings.ini'
    text = path.read_text(encoding='utf-8-sig')
    values = {'EnableVSync': '0', 'LocalPrediction': '1', 'LocalPredictionMaxTicks': '20',
              'NetworkHostDelayPolicy': 'Auto', 'NetworkInputDelayFrames': '0',
              'NetworkSlowPlayerBoundTicks': '3', 'NetworkSlowPlayerPolicy': 'Substitute', 'NetworkShowDiagnostics': '1'}
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


def stage_baseline(run, window_ticks=TICKS, humans=2):
    module = Path(run.cwd) / 'Userdata/UserScenes.rte'
    module.mkdir(exist_ok=True)
    script, rewritten = re.subn(r'(?m)^local WINDOW_TICKS = \d+;$', f'local WINDOW_TICKS = {window_ticks};',
                                (HELPERS / 'FeelBaseline.lua').read_text(encoding='utf-8'))
    if rewritten != 1:
        raise RuntimeError('the feel fixture does not carry exactly one WINDOW_TICKS line')
    (module / 'FeelBaseline.lua').write_text(script, encoding='utf-8')
    (module / 'Index.ini').write_text(
        'DataModule\n\tModuleName = User Scenes\n\tScanFolderContents = 1\n\tIgnoreMissingItems = 1\n'
        '\tAddActivity = GAScripted\n\t\tCopyOf = P4 Alpha Duel\n'
        '\t\tPresetName = Determinism FeelBaseline\n\t\tScriptPath = UserScenes.rte/FeelBaseline.lua\n'
        '\t\tLuaClassName = FeelBaseline\n\t\tIsTestActivity = 1\n'
        '\t\tTeamOfPlayer1 = 0\n\t\tPlayer1IsHuman = 1\n'
        '\t\tTeamOfPlayer2 = 1\n\t\tPlayer2IsHuman = 1\n' +
        ('\t\tTeamOfPlayer3 = 2\n\t\tPlayer3IsHuman = 1\n' if humans == 3 else ''), encoding='utf-8')


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


TIMING_CASES = (
    ('100ms-loss5', 100, 5, None),
    ('200ms-loss5', 200, 5, None),
    ('100ms-silent600', 100, 0, 600),
    ('200ms-silent600', 200, 0, 600),
    ('100ms-loss5-silent600', 100, 5, 600),
    ('200ms-loss5-silent600', 200, 5, 600),
)

AUTOSAVE_CASES = (
    ('autosave-100ms', 100, 1),
    ('autosave-200ms', 200, 1),
)


def engine_placements(peers):
    """Three engines on one box each get their own cores and the host a third of them at above-normal priority: a
    real match runs one engine per machine, so a host starved by its neighbours is the harness's limit, not the round's.
    Whole SMT pairs, the host's third rounded up."""
    if len(peers) != 3 or sys.platform != 'win32':
        return {}
    pairs = (os.cpu_count() or 0) // 2
    if pairs < 3:
        return {}
    host_pairs = -(-pairs // 3)
    client_pairs = -(-(pairs - host_pairs) // 2)
    spans = dict(host=(0, 2 * host_pairs), client=(2 * host_pairs, 2 * (host_pairs + client_pairs)),
                 survivor=(2 * (host_pairs + client_pairs), 2 * pairs))
    return {peer: dict(mask=sum(1 << cpu for cpu in range(*spans[peer])), logical=f'{spans[peer][0]}-{spans[peer][1] - 1}',
                       priority='above_normal' if peer == 'host' else 'normal') for peer in peers}


def place_engine(run, placement):
    """Applies a placement to a started engine and records what the process reports back in its launch.json."""
    import ctypes
    from ctypes import wintypes
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.SetProcessAffinityMask.argtypes = [wintypes.HANDLE, ctypes.c_size_t]
    kernel.SetProcessAffinityMask.restype = wintypes.BOOL
    kernel.GetProcessAffinityMask.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t)]
    kernel.GetProcessAffinityMask.restype = wintypes.BOOL
    kernel.SetPriorityClass.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.SetPriorityClass.restype = wintypes.BOOL
    kernel.GetPriorityClass.argtypes = [wintypes.HANDLE]
    kernel.GetPriorityClass.restype = wintypes.DWORD
    handle = run.process
    if not kernel.SetProcessAffinityMask(handle, placement['mask']):
        raise OSError(f'SetProcessAffinityMask failed: {ctypes.get_last_error()}')
    if not kernel.SetPriorityClass(handle, 0x8000 if placement['priority'] == 'above_normal' else 0x20):
        raise OSError(f'SetPriorityClass failed: {ctypes.get_last_error()}')
    process_mask, system_mask = ctypes.c_size_t(), ctypes.c_size_t()
    if not kernel.GetProcessAffinityMask(handle, ctypes.byref(process_mask), ctypes.byref(system_mask)):
        raise OSError(f'GetProcessAffinityMask failed: {ctypes.get_last_error()}')
    applied = dict(requested_mask=hex(placement['mask']), logical=placement['logical'], priority=placement['priority'],
                   process_mask=hex(process_mask.value), system_mask=hex(system_mask.value), priority_class=hex(kernel.GetPriorityClass(handle)))
    if process_mask.value != placement['mask']:
        raise OSError(f'the engine reports affinity {applied["process_mask"]}, not {applied["requested_mask"]}')
    run.record['cpu_placement'] = applied
    run._save()
    return applied


def peer_pre_match_history(peer, host_pre_match_history, client_pre_match_history):
    # What a runtime spent before the match: the joining client takes its own, every other peer the host's.
    return client_pre_match_history if peer == 'client' else host_pre_match_history


def case_peers(sp=False, silent_tick=None):
    """The engines an arm launches: one single-player run, or host and client plus a survivor when the client goes silent."""
    return ['sp'] if sp else (['host', 'client', 'survivor'] if silent_tick else ['host', 'client'])


# Set by --dry-run: launch_case records each arm here instead of launching it.
DRY_RUN_PLAN = None


def launch_case(root, name, lag, cap, record, port, script, exe_hash, timeout, sp=False, loss_percent=0, silent_tick=None, live_stalls=None, window_ticks=None, sp_humans=2, autosave_seconds=None, host_lua_states=4, client_lua_states=4, host_pre_match_history=0, client_pre_match_history=0):
    if DRY_RUN_PLAN is not None:
        DRY_RUN_PLAN.append(dict(arm=name, port=None if sp else port, lag_ms=lag, loss_percent=loss_percent, silent_tick=silent_tick,
                                 autosave_seconds=autosave_seconds, peers=case_peers(sp, silent_tick)))
        return None
    out = root / name
    out.mkdir(exist_ok=False)
    final_tick = window_ticks if window_ticks is not None else 2 * TICKS if silent_tick else TICKS
    # The engine's Lua state count is a build constant; the flags are kept, accepted and ignored.
    lua_states = {'host': host_lua_states, 'client': client_lua_states}
    pre_match_history = {'host': host_pre_match_history, 'client': client_pre_match_history}
    manifest = dict(started=stamp(), mode='local single-player P4 Alpha Duel' if sp else ('autosave service e2e' if autosave_seconds else ('three-peer service e2e, private rejoin' if silent_tick else 'two-peer service e2e, normal render loop')),
                    ticks=final_tick, lag_ms=lag, cap_hz=cap, instrumentation=record, port=None if sp else port,
                    loss_percent=loss_percent, loss_scope='GNS client send and receive packet loss, each direction', silent_tick=silent_tick,
                    live_stalls=live_stalls, autosave_seconds=autosave_seconds, baseline_humans=sp_humans if sp else None,
                    auto_input_delay=not sp, input_script=file_record(script), input_schedule=file_record(script.with_name('input-schedule.json')),
                    exe=file_record(engine_executable(REPO)), lua_states=lua_states, pre_match_history=pre_match_history,
                    lua_states_note='retired: the engine fixes the count at build time')
    write_json(out / 'manifest.json', manifest)
    peers = case_peers(sp, silent_tick)
    manifest['per_peer_lag_ms'] = {peer: (2 * lag if peer == 'client' else 0) if loss_percent or silent_tick else lag for peer in peers}
    placements = engine_placements(peers)
    manifest['engine_placement'] = {}
    write_json(out / 'manifest.json', manifest)
    runs, records = {}, {}
    try:
        for peer in peers:
            run_out = out / peer
            trace = out / f'{peer}_trace.json'
            flags = ['-seed', '42', '-max-ticks', str(final_tick), '-tick-hashes',
                     '-net-live-tick-hashes', str(out / f'{peer}-live.jsonl'),
                     '-out', str(trace), '-input-script', str(script),
                     '-controller-debug-dump', str(out / f'{peer}_controller.jsonl'),
                     '-controller-debug-ticks', f'1-{final_tick}',
                     '-feel-render-settings', str(run_out / 'runtime/Userdata/FeelRender.ini')]
            if record:
                flags += ['-feel-measure', str(run_out / 'feel')]
            if peer_pre_match_history(peer, host_pre_match_history, client_pre_match_history):
                flags += ['-selftest-prematch-history', str(peer_pre_match_history(peer, host_pre_match_history, client_pre_match_history))]
            if sp:
                flags += ['-scenario', 'FeelBaseline', '-controller-log-out', str(out / 'controllers.json')]
            else:
                flags += ['-net-match-service-e2e', '-net-port', str(port), '-net-match-ticks', str(final_tick),
                          '-net-match-humans', str(len(peers)), '-net-match-peers', str(len(peers)), '-net-match-cpu-slots', '0',
                          '-net-match-service-preset', 'Determinism FeelBaseline',
                          '-net-match-service-module', 'UserScenes.rte',
                          '-net-match-auto-delay', '-net-fake-lag', str(manifest['per_peer_lag_ms'][peer]), '-net-local-prediction', 'on',
                          '-net-reconnect-ticket', str(out / f'{peer}.ticket'),
                          '-net-match-report', str(out / f'{peer}_report.json')]
                if autosave_seconds is not None and peer == 'host':
                    flags += ['-net-autosave-seconds', str(autosave_seconds)]
                if FULLSTATE_EVERY:
                    flags += ['-net-fullstate-hash-every', str(FULLSTATE_EVERY)]
                flags += ['-net-host', '-net-replay-out', str(out / 'match.ccreplay')] if peer == 'host' else ['-net-join', '127.0.0.1']
            environment = dict(CCCP_HEADLESS='1', CC_TRACE_PREVIEW_EVENT='1', CC_SIM_DUMP=f'1:{final_tick}', PYTHONDONTWRITEBYTECODE='1')
            if peer == 'client' and loss_percent:
                environment['CC_TEST_GNS_LOSS_PERCENT'] = str(loss_percent)
            # A live stall holds the client's seat in any form; the three-peer form without one freezes a frame instead.
            if peer == 'client' and live_stalls:
                for tick, duration in live_stalls:
                    flags += ['-net-test-live-stall', f'{tick}:{duration}']
            elif peer == 'client' and silent_tick:
                flags += ['-selftest-frame-stall', f'{silent_tick}:1500']
            run = make_run(REPO, flags, run_out, timeout=timeout, env=environment,
                           expected=[trace, Path(str(trace) + '.simdump.txt'), out / f'{peer}_controller.jsonl'])
            runs[peer] = run
            private_settings(run, cap)
            if record:
                (run_out / 'feel').mkdir()
            stage_baseline(run, final_tick, sp_humans if sp else 2)
            run.start()
            if peer in placements:
                manifest['engine_placement'][peer] = place_engine(run, placements[peer])
                write_json(out / 'manifest.json', manifest)
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
    compress_case_records(out)
    manifest.update(finished=stamp(), scratch_bytes=scratch_bytes(out),
                    launches_complete=all(row.get('exit_code') == 0 and row.get('evidence_complete') and not row.get('timed_out') for row in records.values()))
    write_json(out / 'manifest.json', manifest)
    print(f'{manifest["finished"]} {name}: launches_complete={manifest["launches_complete"]}', flush=True)
    if not sp and record and (out / 'match.ccreplay').is_file():
        inspect = make_run(REPO, ['-net-replay-verify', str(out / 'match.ccreplay'), '-net-replay-dump', f'1:{final_tick}',
                                 '-out', str(out / 'replay-report.json')], out / 'replay-inspect', timeout=timeout,
                           env={'CCCP_HEADLESS': '1'}, expected=[out / 'replay-report.json'])
        try:
            inspect.start().finish()
        finally:
            inspect.close()
        # The recorder's off-wire proof on one committed timeline: the recording played back with no recorder.
        playback = make_run(REPO, ['-net-replay', str(out / 'match.ccreplay'), '-tick-hashes', '-out', str(out / 'replay_trace.json'),
                                   '-max-ticks', str(final_tick), '-seed', '42'], out / 'replay-off', timeout=timeout,
                            env={'CCCP_HEADLESS': '1'}, expected=[out / 'replay_trace.json'])
        stage_baseline(playback, final_tick, 2)
        try:
            playback.start().finish()
        finally:
            playback.close()
    return out


def held_client_away(log):
    """The ticks a held client never simulated: from the tick its seat was held at through the image it rejoined on."""
    ranges = []
    for stop, image in re.findall(r'\[net-match\] recovery requested tick=(\d+) [^\n]*PeerHeld:[^\n]*\n(?:[^\n]*\n)*?\[net-match\] bootstrap checkpoint=(\d+) ', log):
        if int(image) >= int(stop):
            ranges.append((int(stop), int(image)))
    return tuple(ranges)


def held_client_rewinds(log):
    """The images a held client replayed from that were older than the tick it stopped at: it simulates the ticks
    between them twice, and both readings are compared."""
    images = []
    for stop, image in re.findall(r'\[net-match\] recovery requested tick=(\d+) [^\n]*PeerHeld:[^\n]*\n(?:[^\n]*\n)*?\[net-match\] bootstrap checkpoint=(\d+) ', log):
        if int(image) < int(stop):
            images.append(int(image))
    return tuple(images)


def fullstate_proof(run, pairs):
    """The full-state oracle's verdict per pair of match peers: every sampled tick's shared sections must match."""
    return {f'{left}/{right}': compare_fullstate(Path(run) / left / 'stdout.log', Path(run) / right / 'stdout.log') for left, right in pairs}


def committed_timeline(run, final_tick=TICKS):
    """What a match committed before any tick ran: the lobby's seat delays, the agreed first frame and every live delay change."""
    run = Path(run)
    report = json.loads((run / 'host_report.json').read_text(encoding='utf-8-sig')) if (run / 'host_report.json').is_file() else {}
    delays = report.get('service', {}).get('runner', {}).get('lobby', {}).get('match_config', {}).get('peer_input_delays')
    log = (run / 'host/stdout.log').read_text(encoding='utf-8-sig', errors='replace') if (run / 'host/stdout.log').is_file() else ''
    first = re.findall(r'\[net-match\] agreed first frame=(\d+)', log)
    changes = re.findall(r'\[net-match\] delay change peer=(\d+) frame=(\d+) delay=(\d+)', log)
    return dict(peer_input_delays=delays, agreed_first_frame=int(first[0]) if first else None,
                delay_changes=[[int(peer), int(frame), int(delay)] for peer, frame, delay in changes if int(frame) <= final_tick])


def compare_pair(first, second, expected_ticks=TICKS, cross_peer=False, client_away=(), window_only=False, client_rewinds=()):
    """Two peers' traces skip only the per-machine routing subsystem (and the total that folds it in); two runs of one
    peer compare every field."""
    result = dict(first=str(first), second=str(second), cross_peer=cross_peer)
    per_peer = PER_PEER_SUBSYSTEMS if cross_peer else frozenset()
    ok, existing = strict_compare(first, second, expected_ticks=expected_ticks, per_peer=per_peer, client_away=client_away, prefix=window_only,
                                  client_rewinds=client_rewinds)
    result.update(sim_gated_pass=ok, existing_comparator=existing)

    def shared(row):
        if not cross_peer:
            return row
        return {**{key: value for key, value in row.items() if key != 'total'},
                'subsystems': {name: value for name, value in row['subsystems'].items() if name not in per_peer}}
    try:
        left = [shared(row) for row in json.loads(first.read_text(encoding='utf-8-sig'))['runs'][0]['tick_hashes']]
        right = [shared(row) for row in json.loads(second.read_text(encoding='utf-8-sig'))['runs'][0]['tick_hashes']]
        exact_coverage = [row['tick'] for row in left] == [row['tick'] for row in right] == list(range(1, expected_ticks + 1))
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
    for key in dict.fromkeys(key for value in peers.values() for key in value['pins']):
        cells = []
        for peer in ('host', 'client'):
            row = peers[peer]['pins'].get(key)
            if row is None:
                cells.append('n/a')
                continue
            value = row['value']
            if isinstance(value, dict) and 'max' in value:
                value = value['max']
            cells.append(f'{value_text(value)} / {row["status"]}')
        lines.append(f'| {key} | {cells[0]} | {cells[1]} |')
    lines += ['', f'Measurement complete: {report["measurement_complete"]}. Off-wire proof: {report["off_wire_pass"]}.',
              f'RTT and D selection: {json.dumps(peers.get("client", {}).get("metrics", {}).get("auto_picks", []), separators=(",", ":"))}',
              '', 'Raw files and their hashes are in feel-report.json. Per-edge matches, every kinematic residual,',
              'all commit comparisons and firing matches are under each peer/analysis directory.', '',
              'The presentation boundary is UploadFrame return on the private desktop. Audio times are conservative',
              'preview-step bounds for logged voice starts; sound output stays muted. Same-frame remote commands',
              'are candidates, and a correction with unproven cause remains a MISS.']
    (out / 'summary.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')


def single_case_peer(root):
    root = Path(root)
    if not (root / 'manifest.json').is_file():
        return None
    if (root / 'sp').is_dir():
        return 'sp'
    if (root / 'host').is_dir():
        return 'host'
    return None


def early_peer_report(run, peer, error):
    """A peer that stopped before the window is a failed pin on its own arm.

    Its numbers are whatever its clock did reach; the arm is marked incomplete and carries the tick
    and the cause, and every other arm is still measured and reported.
    """
    run = Path(run)
    report = item9a_gates(run, peer)
    report['pins']['item9a_measurement_window'] = pin(
        error.tick, f'the peer must reach tick {TICKS}', False, [error.path] if error.path else [], error.fail_line)
    report['pass_check'] = False
    report['measurement_complete'] = False
    report['early_decision'] = dict(tick=error.tick, evidence=str(error.path), failure=error.fail_line)
    report.setdefault('raw_path', str(record_path(run / peer / 'feel/raw.jsonl')))
    return report


def unreduced_peer_report(run, peer, reason, evidence=None):
    """An arm the reducer could not read at all is a failed pin naming why, never a dead matrix."""
    run = Path(run)
    try:
        report = item9a_gates(run, peer)
    except Exception as error:  # the arm has no clock either: pin that too
        report = dict(peer=peer, pins={}, metrics={}, pass_check=False, measurement_complete=False,
                      clock_unreadable=f'{type(error).__name__}: {error}')
    report['pins']['item9a_reduction'] = pin(
        None, 'the arm must reduce to measured frames', False, [evidence] if evidence else [], reason)
    report['pass_check'] = False
    report['measurement_complete'] = False
    report['reduction_failure'] = reason
    report.setdefault('raw_path', str(record_path(run / peer / 'feel/raw.jsonl')))
    return report


def reduce_or_fail(run, peer, baseline=None):
    try:
        return reduce_peer(run, peer, baseline)
    except EarlyDecision as error:
        print(f'{Path(run).name}/{peer}: {error.fail_line}', flush=True)
        return early_peer_report(run, peer, error)
    except (ValueError, KeyError, IndexError, OSError) as error:
        reason = f'{type(error).__name__}: {error}'
        print(f'{Path(run).name}/{peer}: {reason}', flush=True)
        return unreduced_peer_report(run, peer, reason, record_path(Path(run) / peer / 'feel/raw.jsonl'))


def timing_peer(run, peer):
    try:
        return item9a_gates(run, peer)
    except (ValueError, KeyError, IndexError, OSError) as error:
        reason = f'{type(error).__name__}: {error}'
        print(f'{Path(run).name}/{peer}: {reason}', flush=True)
        return unreduced_peer_report(run, peer, reason)


def reduce_timing_case(run, reference=None):
    manifest = json.loads((run / 'manifest.json').read_text(encoding='utf-8'))
    silent = bool(manifest.get('silent_tick'))
    peers = {peer: timing_peer(run, peer) for peer in (('host', 'survivor') if silent else ('host',))}
    if reference is not None:
        for value in peers.values():
            apply_tps_call(value, reference)
    proof = compare_pair(run / 'host_trace.json', run / ('survivor_trace.json' if silent else 'client_trace.json'), manifest.get('ticks', TICKS), cross_peer=True)
    if silent:
        # The held client's own ticks are compared too: before its hold and from the image it rejoined on.
        client_log = (run / 'client/stdout.log').read_text(encoding='utf-8-sig', errors='replace') if (run / 'client/stdout.log').is_file() else ''
        # A rejoined client runs its own cap from its image, so only the planned window is compared.
        proof['held_client'] = compare_pair(run / 'host_trace.json', run / 'client_trace.json', manifest.get('ticks', TICKS), cross_peer=True,
                                            client_away=held_client_away(client_log), window_only=True,
                                            client_rewinds=held_client_rewinds(client_log))
    pairs = [('host', 'survivor'), ('host', 'client'), ('survivor', 'client')] if silent else [('host', 'client')]
    live = {f'{left}/{right}': compare_live_hashes(run / f'{left}-live.jsonl', run / f'{right}-live.jsonl', 1)
            for left, right in pairs}
    live_pass = all(passes and all(row['compared_ticks'] >= 30 and row['mismatched_ticks'] == 0 for row in passes)
                    for passes in live.values())
    proof.update(live_passes=live, live_pass=live_pass)
    proof['pass'] &= live_pass
    if FULLSTATE_EVERY:
        proof['fullstate'] = fullstate_proof(run, pairs)
        proof['pass'] &= all(row['passed'] for row in proof['fullstate'].values())
    write_json(run / 'hash-proof.json', proof)
    return dict(name=run.name, peers=peers, measurement_complete=manifest['launches_complete'] and all(value['measurement_complete'] for value in peers.values()),
                launches_complete=manifest['launches_complete'], engine_placement=manifest.get('engine_placement') or {},
                proof=proof, off_wire_pass=proof['pass'], item9a_pass=all(value['pass_check'] for value in peers.values()))


def item9a_evidence_complete(report):
    """Require network pins for every measured peer while retaining presentation findings separately."""
    peers = report.get('peers') or {}
    if not peers:
        return False
    for peer in peers.values():
        pins = peer.get('pins', {})
        names = [name for name in pins if name.startswith('item9a_')]
        if not names or any(pin.get('value') is None for name, pin in pins.items() if name.startswith('item9a_')):
            return False
    return True


def analyze(root, stock=None):
    root = Path(root)
    peer = single_case_peer(root)
    if peer:
        manifest = json.loads((root / 'manifest.json').read_text(encoding='utf-8'))
        result = reduce_timing_case(root) if manifest.get('loss_percent') or manifest.get('silent_tick') else reduce_or_fail(root, peer)
        write_json(root / 'feel-report.json', result)
        return [result]
    baselines, plain_baselines = {}, {}
    subset = json.loads((root / 'matrix-plan.json').read_text(encoding='utf-8')).get('lag_arms') if (root / 'matrix-plan.json').is_file() else None
    for cap_name in ('60hz', 'uncapped'):
        run = root / f'baseline-{cap_name}'
        if subset and not run.is_dir():
            continue
        result = reduce_or_fail(run, 'sp')
        baselines[cap_name] = dict(result['metrics'], raw_path=result['raw_path'])
        timing = item9a_gates(run, 'sp')
        baselines[cap_name]['steady_wall_tps'] = timing['metrics']['steady_wall_tps']
        result['metrics']['steady_wall_tps'] = timing['metrics']['steady_wall_tps']
        write_json(run / 'feel-report.json', result)
        plain = item9a_gates(root / f'baseline-{cap_name}-off', 'sp')
        plain_baselines[cap_name] = dict(steady_wall_tps=plain['metrics']['steady_wall_tps'],
            evidence=plain['metrics']['clock_path'], method='same build, scene, input script, hashes and render cap; recorder off')
        write_json(root / f'baseline-{cap_name}-off' / 'feel-report.json', plain)
    three_reference = None
    if not subset:
        three = item9a_gates(root / 'baseline-three-60hz', 'sp')
        write_json(root / 'baseline-three-60hz' / 'feel-report.json', three)
        three_reference = dict(steady_wall_tps=three['metrics']['steady_wall_tps'], evidence=three['metrics']['clock_path'],
            method='same build, six actors, three human seats, recorder and 60 Hz cap; single process with local seat views')
    results = []
    for lag in (100, 200):
        for cap_name in ('60hz', 'uncapped'):
            name = f'{lag}ms-{cap_name}'
            if subset and name not in subset:
                continue
            on, off = root / (name + '-on'), root / (name + '-off')
            manifest = json.loads((on / 'manifest.json').read_text(encoding='utf-8'))
            peers = {peer: reduce_or_fail(on, peer, baselines[cap_name]) for peer in ('host', 'client')}
            for value in peers.values():
                apply_tps_call(value, dict(steady_wall_tps=baselines[cap_name]['steady_wall_tps'],
                    evidence=baselines[cap_name]['raw_path'], method='same build, scene, actor count, recorder and render cap'))
            for peer in ('host', 'client'):
                timing = item9a_gates(off, peer)
                reference = plain_baselines[cap_name]
                if stock and cap_name in stock:
                    timing['stock_reference'] = stock[cap_name]
                    if stock[cap_name]['steady_wall_tps'] < 59.5 or reference['steady_wall_tps'] >= 59.5:
                        reference = stock[cap_name]
                apply_tps_call(timing, reference)
                peers[peer + '_off'] = timing
            proof = {'peers_on': compare_pair(on / 'host_trace.json', on / 'client_trace.json', cross_peer=True),
                     'peers_off': compare_pair(off / 'host_trace.json', off / 'client_trace.json', cross_peer=True),
                     **{peer + '_on_replay': compare_pair(on / f'{peer}_trace.json', on / 'replay_trace.json', cross_peer=True) for peer in ('host', 'client')},
                     **{peer + '_on_off': compare_pair(on / f'{peer}_trace.json', off / f'{peer}_trace.json') for peer in ('host', 'client')}}
            # Two separate matches are one sim only when they committed one timeline; otherwise the replay rows carry the proof.
            timelines = {state: committed_timeline(run) for state, run in (('on', on), ('off', off))}
            for peer in ('host', 'client'):
                proof[peer + '_on_off']['committed_timelines'] = timelines
                proof[peer + '_on_off']['required'] = timelines['on'] == timelines['off']
            if FULLSTATE_EVERY:
                for state, state_run in (('on', on), ('off', off)):
                    verdict = compare_fullstate(state_run / 'host' / 'stdout.log', state_run / 'client' / 'stdout.log')
                    proof[f'fullstate_{state}'] = dict(verdict, **{'pass': verdict['passed']})
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
                          peers=peers, proof=proof, off_wire_pass=all(row['pass'] for row in proof.values() if row.get('required', True)),
                          measurement_complete=manifest['launches_complete'] and all(row['measurement_complete'] for row in peers.values()),
                          raw_files=[file_record(path) for path in raw_paths if record_path(path).is_file()],
                          missing_raw_files=[str(path) for path in raw_paths if not record_path(path).is_file()])
            report['measurement_complete'] &= not report['missing_raw_files']
            write_json(on / 'feel-report.json', report)
            summarize_case(report, on)
            results.append(report)
    for name, _, _, _ in () if subset else TIMING_CASES:
        run = root / name
        if not run.is_dir():
            results.append(dict(name=name, peers={}, measurement_complete=False, off_wire_pass=False, item9a_pass=False, reason='case not measured'))
            continue
        reference = three_reference if 'silent' in name else dict(steady_wall_tps=baselines['60hz']['steady_wall_tps'],
            evidence=baselines['60hz']['raw_path'], method='same build, four actors, recorder and 60 Hz cap')
        report = reduce_timing_case(run, reference)
        write_json(run / 'feel-report.json', report)
        results.append(report)
    for name, _, _ in () if subset else AUTOSAVE_CASES:
        run = root / name
        if not run.is_dir():
            results.append(dict(name=name, peers={}, measurement_complete=False, off_wire_pass=False, item9a_pass=False, reason='case not measured'))
            continue
        report = reduce_timing_case(run, three_reference)
        write_json(run / 'feel-report.json', report)
        results.append(report)
    write_json(root / 'matrix-report.json', results)
    lines = [f'Measured {stamp()}', '', '| Configuration | Raw measurements complete | Off-wire proof | Findings |', '|---|---|---|---|']
    for report in results:
        misses = sum(row['status'] == 'MISS' for peer in report['peers'].values() for row in peer['pins'].values())
        link = f'{report["name"]}/feel-report.json' if report['name'] in {case[0] for case in TIMING_CASES + tuple((name, lag, 0, None) for name, lag, _ in AUTOSAVE_CASES)} else f'{report["name"]}-on/summary.md'
        lines.append(f'| {report["name"]} | {report["measurement_complete"]} | {report["off_wire_pass"]} | {misses} MISS; [{report["name"]}]({link}) |')
    lines += ['', 'Item 9a retains the 50 ms and one-percent wait gates. TPS uses the same-machine',
              'single-player reference with a five-percent maximum gap when that reference is below 59.5.',
              'Nominal 60 Hz horizon drift is retained as a diagnostic in that case. Missing records and failed',
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
    graph = make_run(REPO, ['-script-graph-selftest'], out / 'script-graph', timeout=timeout, env=env)
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
                  selftests_pass=suite.returncode == 0 and suite_json.get('passed') == len(SELFTESTS) and suite_json.get('total') == len(SELFTESTS),
                  script_graph_pass=graph_record.get('exit_code') == 0 and not graph_record.get('timed_out')
                  and bool(re.search(r'\bPASS\b', graph_text)) and not re.search(r'\bFAIL\b', graph_text),
                  sp_comparator=file_record(SP_COMPARATOR), sp_compare_command=compare_command,
                  sp_control_dump=file_record(control / 'trace.json.simdump.txt'), sp_compare_pass=compared.returncode == 0)
    write_json(out / 'gates.json', result)
    return result


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--port', type=int, default=48231)
    parser.add_argument('--timeout', type=float, default=600)
    parser.add_argument('--analyze-only', action='store_true')
    parser.add_argument('--skip-gates', action='store_true', help='retain gates as unverified')
    parser.add_argument('--sp-control', type=Path, default=SP_CONTROL)
    parser.add_argument('--stock-baseline', type=Path, help='same-machine stock measurements, keyed by 60hz and uncapped')
    parser.add_argument('--host-lua-states', type=int, default=4, help='retired: the build fixes the Lua state count')
    parser.add_argument('--client-lua-states', type=int, default=4, help='retired: the build fixes the Lua state count')
    parser.add_argument('--host-pre-match-history', type=int, default=0, help='objects the host runtime spends before the match')
    parser.add_argument('--client-pre-match-history', type=int, default=0, help='objects the joining client spends before the match')
    parser.add_argument('--cases', nargs='+', choices=[name for name, *_ in AUTOSAVE_CASES] + [name for name, *_ in TIMING_CASES],
                        help='run only the selected autosave or timing arms, without baselines or the full matrix')
    parser.add_argument('--lag-arms', nargs='+', choices=LAG_ARMS,
                        help='run only these lag arms (each on and off) and the single-player baselines of their caps')
    parser.add_argument('--dry-run', action='store_true',
                        help='print every arm this command would launch with its port and peers; launch nothing, write nothing')
    parser.add_argument('--fullstate-every', type=int, default=0,
                        help='every N committed ticks each match peer hashes its whole capture (-net-fullstate-hash-every); 0 is off')
    return parser, parser.parse_args(argv)


def launch_timing_arm(root, index, case, port_base, script, exe_hash, timeout, counts):
    """One loss or silent-seat arm, launched the same way by the full matrix and by --cases."""
    name, lag, loss, silent = case
    return launch_case(root, name, lag, 60, True, port_base + 8 + index % 2, script, exe_hash, timeout,
                       loss_percent=loss, silent_tick=silent, **counts)


def launch_autosave_arm(root, index, case, port_base, script, exe_hash, timeout, counts):
    """One autosave arm, launched the same way by the full matrix and by --cases."""
    name, lag, seconds = case
    return launch_case(root, name, lag, 60, True, port_base + 8 + index % 2, script, exe_hash, timeout,
                       window_ticks=2 * TICKS, autosave_seconds=seconds, **counts)


def dry_run_plan(launch_all):
    """The arms the given launch sequence would start, read from launch_case itself."""
    global DRY_RUN_PLAN
    DRY_RUN_PLAN = []
    try:
        launch_all()
        return DRY_RUN_PLAN
    finally:
        DRY_RUN_PLAN = None


def main(argv=None):
    parser, args = parse_args(argv)
    root = args.out.resolve()
    if not 48231 <= args.port <= 48240:
        parser.error('the ten match ports must stay within 48231..48240')
    if args.host_lua_states < 1 or args.client_lua_states < 1:
        parser.error('--host-lua-states and --client-lua-states must be positive')
    if args.fullstate_every < 0:
        parser.error('--fullstate-every must be 0 or positive')
    global FULLSTATE_EVERY
    FULLSTATE_EVERY = args.fullstate_every
    if (Path('D:/mx/LEAD_FAMILY.lock')).exists():
        parser.error('Phase 1 lock is present; no driver or engine launch is permitted')
    branch = subprocess.check_output(['git', '-C', str(REPO), 'branch', '--show-current'], text=True).strip()
    os.environ.update(CCCP_HEADLESS='1', PYTHONDONTWRITEBYTECODE='1')
    scratch_bytes(root, MATRIX_BYTE_LIMIT)
    counts = dict(host_lua_states=args.host_lua_states, client_lua_states=args.client_lua_states,
                  host_pre_match_history=args.host_pre_match_history, client_pre_match_history=args.client_pre_match_history)
    if args.cases:
        selected = [(index, case) for index, case in enumerate(AUTOSAVE_CASES) if case[0] in args.cases] + \
                   [(index, case) for index, case in enumerate(TIMING_CASES) if case[0] in args.cases]
        launch_selected = lambda script, exe_hash: [(launch_timing_arm if len(case) == 4 else launch_autosave_arm)(root, index, case, args.port, script, exe_hash, args.timeout, counts)
                                                    for index, case in selected]
        if args.dry_run:
            print(json.dumps(dict(path='--cases', arms=dry_run_plan(lambda: launch_selected(root / 'input.txt', None))), indent=2), flush=True)
            return 0
        if not args.analyze_only:
            root.mkdir(parents=True, exist_ok=True)
            if (root / 'matrix-plan.json').exists():
                parser.error('matrix-plan.json already exists; use a fresh output directory')
            exe = file_record(engine_executable(REPO))
            write_json(root / 'matrix-plan.json', dict(started=stamp(), exe=exe, branch=branch,
                commit=subprocess.check_output(['git', '-C', str(REPO), 'rev-parse', 'HEAD'], text=True).strip(),
                source=file_record(Path(__file__)), reducer=file_record(HELPERS / 'report.py'),
                ports=[args.port + 8 + index % 2 for index, _ in selected], ticks=2 * TICKS,
                arms=[case[0] for _, case in selected]))
            script = root / 'input.txt'
            input_pattern(script)
            launch_selected(script, exe['sha256'])
        results = [reduce_timing_case(root / case[0]) for _, case in selected]
        for result in results:
            write_json(root / result['name'] / 'feel-report.json', result)
        write_json(root / 'matrix-report.json', results)
        complete = all(result['launches_complete'] for result in results)
        proof_pass = all(result['off_wire_pass'] for result in results)
        write_json(root / 'completion.json', dict(finished=stamp(), launches_complete=complete,
            case_launches={result['name']: result['launches_complete'] for result in results},
            off_wire_pass=proof_pass, gates_unverified=True, scratch_bytes=scratch_bytes(root, MATRIX_BYTE_LIMIT)))
        return 0 if complete and proof_pass else 1
    def launch_matrix(script, exe_hash):
        if args.lag_arms:
            for cap, cap_name in ((60, '60hz'), (0, 'uncapped')):
                if not any(arm.endswith(cap_name) for arm in args.lag_arms): continue
                launch_case(root, 'baseline-' + cap_name, 0, cap, True, 0, script, exe_hash, args.timeout, sp=True, **counts)
                launch_case(root, 'baseline-' + cap_name + '-off', 0, cap, False, 0, script, exe_hash, args.timeout, sp=True, **counts)
            # The full matrix's port for each arm, so a subset never reaches outside the ten.
            for index, arm in enumerate(LAG_ARMS):
                if arm not in args.lag_arms: continue
                lag, cap_name = int(arm.split('ms-')[0]), arm.split('ms-')[1]
                for offset, enabled in enumerate((True, False)):
                    launch_case(root, f'{arm}-' + ('on' if enabled else 'off'), lag, 60 if cap_name == '60hz' else 0, enabled, args.port + 2 * index + offset,
                                script, exe_hash, args.timeout, **counts)
            return
        for cap, cap_name in ((60, '60hz'), (0, 'uncapped')):
            launch_case(root, 'baseline-' + cap_name, 0, cap, True, 0, script, exe_hash, args.timeout, sp=True, **counts)
            launch_case(root, 'baseline-' + cap_name + '-off', 0, cap, False, 0, script, exe_hash, args.timeout, sp=True, **counts)
        launch_case(root, 'baseline-three-60hz', 0, 60, True, 0, script, exe_hash, args.timeout, sp=True,
                    window_ticks=2 * TICKS, sp_humans=3, **counts)
        port = args.port
        for lag in (100, 200):
            for cap, cap_name in ((60, '60hz'), (0, 'uncapped')):
                for enabled in (True, False):
                    name = f'{lag}ms-{cap_name}-' + ('on' if enabled else 'off')
                    launch_case(root, name, lag, cap, enabled, port, script, exe_hash, args.timeout, **counts)
                    port += 1
        for index, case in enumerate(TIMING_CASES):
            launch_timing_arm(root, index, case, args.port, script, exe_hash, args.timeout, counts)
        for index, case in enumerate(AUTOSAVE_CASES):
            launch_autosave_arm(root, index, case, args.port, script, exe_hash, args.timeout, counts)
    if args.dry_run:
        print(json.dumps(dict(path='full matrix', arms=dry_run_plan(lambda: launch_matrix(root / 'input.txt', None))), indent=2), flush=True)
        return 0
    if not args.analyze_only:
        root.mkdir(parents=True, exist_ok=True)
        if (root / 'matrix-plan.json').exists():
            parser.error('matrix-plan.json already exists; retain it and use a fresh child output directory')
        exe = file_record(engine_executable(REPO))
        plan = dict(started=stamp(), exe=exe, branch=branch,
                    commit=subprocess.check_output(['git', '-C', str(REPO), 'rev-parse', 'HEAD'], text=True).strip(),
                    source=file_record(Path(__file__)), reducer=file_record(HELPERS / 'report.py'),
                    ports=list(range(args.port, args.port + 10)), ticks=TICKS,
                    scratch_byte_limits=dict(case=BYTE_LIMIT, matrix=MATRIX_BYTE_LIMIT),
                    mode='service e2e without -free-run-sim; the normal loop presents every render iteration',
                    captures='own -feel-measure seam; frame-<requested tick>.png after UploadFrame',
                    lag_arms=args.lag_arms,
                    arms=[f'baseline-{cap}-on' for cap in ('60hz', 'uncapped')] +
                         [f'{lag}ms-{cap}-{state}' for lag in (100, 200) for cap in ('60hz', 'uncapped') for state in ('on', 'off')] +
                         [name for name, *_ in TIMING_CASES] + [name for name, *_ in AUTOSAVE_CASES],
                    lua_states={'host': args.host_lua_states, 'client': args.client_lua_states},
                    pre_match_history={'host': args.host_pre_match_history, 'client': args.client_pre_match_history})
        write_json(root / 'matrix-plan.json', plan)
        script = root / 'input.txt'
        input_pattern(script)
        launch_matrix(script, exe['sha256'])
    try:
        stock = json.loads(args.stock_baseline.read_text(encoding='utf-8')) if args.stock_baseline else None
        results = analyze(root, stock)
    except EarlyDecision as error:
        write_json(root / 'completion.json', dict(finished=stamp(), measurement_complete=False,
                   item9a_pass=False, gates_pass=False, gates_unverified=True, failure=error.fail_line,
                   evidence=str(error.path)))
        print(error.fail_line, flush=True)
        return 1
    except Exception as error:  # the matrix still owes its report: name the shape that broke it
        import traceback
        reason = f'{type(error).__name__}: {error}'
        write_json(root / 'completion.json', dict(finished=stamp(), measurement_complete=False,
                   item9a_pass=False, gates_pass=False, gates_unverified=True, failure=reason,
                   traceback=traceback.format_exc().splitlines()[-6:]))
        print(reason, flush=True)
        return 1
    skip_gates = args.skip_gates or args.analyze_only
    gate_result = None if skip_gates else gates(root, args.sp_control, args.timeout)
    case_launches = {path.parent.name: json.loads(path.read_text(encoding='utf-8'))['launches_complete']
                     for path in sorted(root.glob('*/manifest.json'))}
    complete = bool(case_launches) and all(case_launches.values()) and all(item9a_evidence_complete(row) for row in results)
    item9a_rows = [pin for row in results for peer in (row.get('peers') or ({'single': row} if 'pins' in row else {})).values()
                  for name, pin in peer['pins'].items() if name.startswith('item9a_') and pin.get('required', True)]
    item9a_pass = all(value['status'] == 'PASS' for value in item9a_rows) if item9a_rows else None
    gate_pass = bool(gate_result and all(gate_result[key] for key in ('selftests_pass', 'script_graph_pass', 'sp_compare_pass')))
    completion = dict(finished=stamp(), measurement_complete=complete, presentation_measurement_complete=all(row.get('measurement_complete', False) for row in results),
                      launches_complete=bool(case_launches) and all(case_launches.values()),
                      case_launches=case_launches, item9a_pass=item9a_pass, item9a_checks=len(item9a_rows), gates_pass=gate_pass,
                      scratch_bytes=scratch_bytes(root, MATRIX_BYTE_LIMIT), gates_unverified=skip_gates)
    write_json(root / 'completion.json', completion)
    with (root / 'summary.md').open('a', encoding='utf-8') as stream:
        stream.write(f'\nGates passed: {gate_pass}. See gates/gates.json and completion.json.\n')
    print(json.dumps(completion, indent=2), flush=True)
    return 0 if complete and item9a_pass is not False and (gate_pass or skip_gates) else 1


if __name__ == '__main__':
    raise SystemExit(main())
