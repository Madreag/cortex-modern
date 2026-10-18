"""Measure local input presentation beside the same single-player script."""
from __future__ import annotations

import argparse
from collections import Counter
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
from zoneinfo import ZoneInfo

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'tools'))
from feel import report
from feel.records import compress_case_records, record_path
from feel_measure import input_pattern, private_settings, stage_baseline, compare_pair, scratch_bytes
from run_sim_test import engine_executable, make_run
from run_selftests import BUILD_PROCESSES

TICKS = report.TICKS
ARMS = {'sp': None, '30ms': 30, '200ms': 200}


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, allow_nan=False) + '\n', encoding='utf-8')


def stamp():
    return datetime.now(ZoneInfo('America/Phoenix')).strftime('%Y-%m-%d %I:%M:%S %p MST')


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def contenders():
    found = []
    listing = subprocess.run(['ps', '-axo', 'pid=,comm='], capture_output=True,
                             text=True, timeout=60, check=True).stdout
    for line in listing.splitlines():
        pid, separator, command = line.strip().partition(' ')
        if not separator or not pid.isdecimal():
            continue
        command = command.strip()
        name = Path(command).name
        if name in ('CortexCommand', 'CortexCommand.exe') or name.lower() in BUILD_PROCESSES:
            found.append(dict(pid=int(pid), exe=command))
    return found


def wait_quiet():
    while live := contenders():
        print(stamp(), 'waiting for a quiet measurement slot', json.dumps(live), flush=True)
        time.sleep(10)


def watch_results(path):
    values = []
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        if line.startswith('[text-watch] summary '):
            values.append(json.loads(line[len('[text-watch] summary '):]))
    return values


def preview_obligations(frames, network):
    """A stopped activity owes no preview; missing state cannot excuse a live frame."""
    state = ('activity_running', 'lockstep_active', 'lockstep_paused')
    missing = [r['_line'] for r in frames if any(k not in r for k in state)]
    owed = [r for r in frames if r.get('activity_running') and r['actors'] and
            (not network or r.get('lockstep_active') and not r.get('lockstep_paused'))]
    mismatches = [dict(line=r['_line'], tick=r['tick'], frame=r['frame'], delay=r['delay'], depth=r['preview_depth'])
                  for r in owed if r['preview_depth'] != r['delay']]
    return owed, mismatches, missing


def applied_delays(text):
    return [tuple(map(int, values)) for values in re.findall(
        r'\[net-input-delay-applied\] local=\d+ peer=(\d+) frame=(\d+) ticks=(\d+)', text)]


def live_resize_proof(logs, peers, step):
    changes = {peer: applied_delays(logs[peer]) for peer in peers}
    faults = {peer: [tuple(map(int, values)) for values in re.findall(
        r'\[feel-lag-step\] requested_frame=(\d+) observed_target_frame=(\d+) per_engine_lag_ms=(\d+) '
        r'applied=(\d+) readback=(\d+) send_lag_ms=(\d+) receive_lag_ms=(\d+)', logs[peer])] for peer in peers}
    faults_match = all(len(rows) == 1 and rows[0][0] == step[0] and rows[0][1] >= step[0] and
                       rows[0][2:] == (step[1], 1, 1, step[1] // 2, step[1] - step[1] // 2)
                       for rows in faults.values())
    same = len(peers) == 2 and bool(changes[peers[0]]) and sorted(changes[peers[0]]) == sorted(changes[peers[1]])
    rises = same and any(frame > step[0] and delay >= 13 for owner, frame, delay in changes[peers[0]])
    return dict(changes=changes, faults=faults, same_applied_frames=same, rises_after_fault=bool(rises),
                pass_check=faults_match and bool(rises))


def reduce_peer(folder, peer, rtt, plan, lag_step=None):
    raw = record_path(folder / peer / 'feel/raw.jsonl')
    rows = list(report.read_jsonl(raw))
    frames = [r for r in rows if r['type'] == 'frame' and r['active'] and 1 <= r['tick'] <= TICKS]
    inputs = [r for r in rows if r['type'] == 'input' and 180 <= r['tick'] <= 1100]
    latencies = report.input_latencies(inputs, frames) if frames else []
    drawn = {r['frame']: r for r in frames}
    for edge in latencies:
        if frame := drawn.get(edge['frame']):
            edge.update(drawn_tick=frame['tick'], drawn_preview_depth=frame['preview_depth'],
                        drawn_horizon_tick=frame['tick'] + frame['preview_depth'])
    required = Counter((p['tick'], p['action'], p['held']) for p in plan['probes'])
    observed = Counter((r['tick'], c['action'], c['held']) for r in inputs for c in r['changes'])
    missing = list((required - observed).elements())
    latency_missing = [r for r in latencies if r['right_censored']]
    expected_responses = sum(p['action'] in ('AIM_VECTOR', 'L_LEFT', 'L_RIGHT') for p in plan['probes'])
    measured = [r['ms'] for r in latencies if r['ms'] is not None]
    owed, mismatches, missing_state = preview_obligations(frames, rtt is not None)
    depths = Counter((r['delay'], r['preview_depth']) for r in owed)
    native_path = folder / f'{peer}_report.json'
    native = json.loads(native_path.read_text()) if native_path.is_file() else {}
    preview = native.get('local_prediction', {})
    lockstep = native.get('service', {}).get('runner', {}).get('lockstep', {})
    preview_cost = preview.get('ms_total', 0) / preview['actor_ticks'] if preview.get('actor_ticks') else None
    preview_report_pass = rtt is None or bool(
        preview.get('enabled') and preview.get('previews', 0) > 0 and preview.get('actor_ticks', 0) > 0 and
        preview.get('last_request_completed') is True and preview.get('last_completed_tick', 0) > 0 and
        preview.get('last_completed_tick') == preview.get('last_requested_tick') and
        preview.get('input_delay_ticks', 0) > 0 and preview.get('last_depth') == preview['input_delay_ticks'] and
        preview.get('configured_max_ticks', -1) >= 0 and
        preview.get('effective_max_ticks') == max(preview['input_delay_ticks'], preview['configured_max_ticks']))
    watches = watch_results(folder / peer / 'stdout.log')
    delay_watches = [w for w in watches if w['watch'] == 'b4-delay']
    duplicate_watches = [w for w in watches if w['watch'] == 'screen-duplicates']
    own_hold_watches = [w for w in watches if w['watch'] == 'c-own-hold']
    delay_values = [r['delay'] for r in frames if r['actors']]
    limit = 16 if lag_step else 6 if rtt == 30 else 16 if rtt == 200 else 0
    low_delay = [r['delay'] for r in frames if r['actors'] and lag_step and r['tick'] < lag_step[0]]
    coverage = bool(frames) and not missing and len(latencies) == expected_responses and not latency_missing
    result = dict(peer=peer, requested_rtt_ms=rtt, raw=str(raw), raw_sha256=sha(raw),
                  input_coverage=dict(expected_responses=expected_responses, observed_responses=len(latencies),
                                      missing_script_edges=missing, responses_not_drawn=len(latency_missing)),
                  latency_ms=report.distribution(measured),
                  latency_frames=report.distribution([r['frames'] for r in latencies if r['frames'] is not None]),
                  input_edges=latencies, delay_min=min(delay_values, default=None), delay_max=max(delay_values, default=None),
                  delay_depth_counts=[dict(delay=d, depth=p, frames=n) for (d, p), n in sorted(depths.items())],
                  depth_mismatches=mismatches, missing_activity_state=missing_state, preview=preview, preview_ms_per_actor_tick=preview_cost,
                  wait_stats={key: lockstep.get(key) for key in ('missing_frame_stalls', 'blocking_frame_waits', 'longest_stall_ms')},
                  rtt_samples=[dict(tick=r['tick'], peer=p['peer'], ping_ms=p['ping_ms'])
                               for r in frames if r['tick'] % 60 == 0 for p in r.get('rtt', []) if not p['local']],
                  captures=[r for r in rows if r['type'] == 'capture'], delay_watches=delay_watches,
                  duplicate_watches=duplicate_watches, own_hold_watches=own_hold_watches,
                  c_duplicates_pass=rtt is None or bool(duplicate_watches) and all(w['active_frames'] > 0 and w['violations'] == 0 for w in duplicate_watches),
                  c_own_hold_pass=bool(own_hold_watches) and all(w['violations'] == 0 for w in own_hold_watches),
                  b1_delay_pass=bool(delay_values) and max(delay_values) <= limit and
                                (not lag_step or bool(low_delay) and max(low_delay) <= 6),
                  b2_report_pass=preview_report_pass,
                  b2_depth_pass=bool(owed) and not mismatches and not missing_state and preview_report_pass,
                  b3_coverage_pass=coverage,
                  b4_overlay_pass=rtt is None or bool(delay_watches) and all(w['active_frames'] > 0 and w['violations'] == 0 for w in delay_watches))
    write_json(folder / f'{peer}-measurement.json', result)
    return result


def launch_case(root, arm, repeat, port, exe_sha, size, lag_step=None, preview_cap=20, host_stall=None, dump_window=None):
    wait_quiet()
    scratch_bytes(root.parent)
    out = root / f'{repeat:02d}-{arm}'
    out.mkdir()
    script = out / 'input.txt'
    input_pattern(script)
    plan = json.loads(script.with_name('input-schedule.json').read_text())
    watches = out / 'screen-watches.txt'
    watches.write_text('b4-delay input_delay running\nc-own-hold sentence local_held LabelNetMatchStatus The AI is playing for you until you are back\nscreen-duplicates duplicates always\n', encoding='utf-8')
    rtt = ARMS[arm]
    peers = ['sp'] if rtt is None else ['host', 'client']
    runs, receipts = {}, {}
    manifest = dict(started=stamp(), arm=arm, repeat=repeat, requested_rtt_ms=rtt, ticks=TICKS,
                    per_engine_lag_ms=0 if rtt is None else rtt // 2, headed=True, display=':0', size=size,
                    lag_step=lag_step, local_prediction_max_ticks_setting=preview_cap, host_stall=host_stall, dump_window=dump_window,
                    binary=str(engine_executable(REPO)), binary_sha256=exe_sha, quiet_samples=[])
    write_json(out / 'manifest.json', manifest)
    try:
        for peer in peers:
            run_out = out / peer
            trace = out / f'{peer}_trace.json'
            flags = ['-seed', '42', '-max-ticks', str(TICKS), '-tick-hashes', '-out', str(trace),
                     '-input-script', str(script), '-feel-render-settings', str(run_out / 'runtime/Userdata/FeelRender.ini'),
                     '-feel-measure', str(run_out / 'feel')]
            if dump_window:
                flags += ['-controller-debug-dump', str(out / f'{peer}_controller.jsonl'),
                          '-controller-debug-ticks', '-'.join(map(str, dump_window))]
            if rtt is None:
                flags += ['-scenario', 'FeelBaseline']
            else:
                flags += ['-net-match-service-e2e', '-net-port', str(port), '-net-match-ticks', str(TICKS),
                          '-net-match-humans', '2', '-net-match-peers', '2', '-net-match-cpu-slots', '0',
                          '-net-match-service-preset', 'Determinism FeelBaseline', '-net-match-service-module', 'UserScenes.rte',
                          '-net-match-auto-delay', '-net-fake-lag', str(rtt // 2),
                          '-net-reconnect-ticket', str(out / f'{peer}.ticket'), '-net-match-report', str(out / f'{peer}_report.json')]
                if peer == 'host' and host_stall:
                    flags += ['-net-test-live-stall', ':'.join(map(str, host_stall))]
                flags += ['-net-host'] if peer == 'host' else ['-net-join', '127.0.0.1']
            environment = dict(CC_RUNNER_HEADED='1', DISPLAY=':0', CC_TEST_FEEL_HEADED='1', CCCP_TEST_SINGLE_SCREEN='1',
                               CCCP_TEST_SCREEN_WATCHES=str(watches), CC_TEST_VSYNC_OFF='1', PYTHONDONTWRITEBYTECODE='1')
            if rtt is not None:
                environment['CC_TEST_NET_MATCH_E2E_WAIT_PEERS'] = '1'
                environment['CC_TEST_DELAY_APPLY_DETAILS'] = '1'
                if lag_step:
                    environment['CC_TEST_FEEL_LAG_STEP'] = ':'.join(map(str, lag_step))
            if dump_window:
                environment['CC_SIM_DUMP'] = ':'.join(map(str, dump_window))
            expected = [trace, run_out / 'feel/raw.jsonl'] + ([out / f'{peer}_report.json'] if rtt is not None else [])
            run = make_run(REPO, flags, run_out, timeout=900, env=environment, expected=expected)
            runs[peer] = run
            private_settings(run, 60)
            settings = Path(run.cwd) / 'Userdata/Settings.ini'
            text = settings.read_text(encoding='utf-8')
            for key, value in zip(('ResolutionX', 'ResolutionY'), size):
                text = re.sub(rf'(?m)^([ \t]*{key}[ \t]*=[ \t]*)[^\r\n]*', lambda match: match[1] + str(value), text)
            text, cap_lines = re.subn(r'(?m)^([ \t]*LocalPredictionMaxTicks[ \t]*=[ \t]*)[^\r\n]*',
                                     lambda match: match[1] + str(preview_cap), text)
            if cap_lines != 1:
                raise RuntimeError('private settings must contain exactly one LocalPredictionMaxTicks')
            settings.write_text(text, encoding='utf-8')
            runtime_path = run_out / 'runtime.json'
            runtime = json.loads(runtime_path.read_text())
            runtime['settings_overrides'].update(dict(zip(('ResolutionX', 'ResolutionY'), map(str, size))))
            runtime['settings_overrides']['LocalPredictionMaxTicks'] = str(preview_cap)
            runtime['settings_sha256'] = sha(settings)
            write_json(runtime_path, runtime)
            stage_baseline(run, TICKS, 2)
            (run_out / 'feel').mkdir()
            run.start()
            if peer == 'host':
                time.sleep(.75)
        while any(run.poll() is None for run in runs.values()):
            live = contenders()
            own = {run.engine_pid for run in runs.values()}
            manifest['quiet_samples'].append(dict(time=stamp(), foreign=[p for p in live if p['pid'] not in own], loadavg=os.getloadavg()))
            failed = {peer: run.poll() for peer, run in runs.items() if run.poll() not in (None, 0)}
            if failed:
                manifest['peer_failure_abort'] = failed
                # A failed peer can leave its partner trying to recover forever. Preserve the failure and
                # close only these owned runner jobs; an aborted partner's missing evidence stays RED.
                for run in runs.values():
                    if run.poll() is None:
                        run.close()
                break
            time.sleep(1)
        for peer, run in runs.items():
            receipts[peer] = run.finish()
    finally:
        for run in runs.values():
            run.close()
        receipts.update({peer: run.record for peer, run in runs.items()})
        write_json(out / 'run-result.json', receipts)
    manifest['quiet'] = not any(r['foreign'] for r in manifest['quiet_samples'])
    manifest['binary_unchanged'] = sha(engine_executable(REPO)) == exe_sha
    manifest['complete'] = all(r.get('exit_code') == 0 and not r.get('timed_out') and r.get('evidence_complete') for r in receipts.values())
    manifest['finished'] = stamp()
    write_json(out / 'manifest.json', manifest)
    compress_case_records(out)
    results = [reduce_peer(out, peer, rtt, plan, lag_step) for peer in peers] if manifest['complete'] else []
    if rtt is not None and manifest['complete']:
        manifest['peer_hashes'] = compare_pair(out / 'host_trace.json', out / 'client_trace.json', cross_peer=True)
        manifest['complete'] = manifest['peer_hashes']['pass']
    if lag_step:
        manifest['live_resize'] = live_resize_proof({peer: (out / peer / 'stdout.log').read_text(errors='replace') for peer in peers}, peers, lag_step)
        manifest['live_resize']['measured_rtt_reaches_200ms'] = all(any(r['ping_ms'] >= 200 for r in p['rtt_samples']) for p in results) and len(results) == 2
        manifest['live_resize']['pass_check'] &= manifest['live_resize']['measured_rtt_reaches_200ms']
    write_json(out / 'manifest.json', manifest)
    print(stamp(), arm, repeat, json.dumps({p['peer']: p['latency_ms'] for p in results}), flush=True)
    return dict(arm=arm, repeat=repeat, path=str(out), manifest=manifest, peers=results)


def summarize(cases, repeats, selected):
    baselines = [p for case in cases if case['arm'] == 'sp' for p in case['peers']]
    coverage = len(baselines) >= 3 and all(p['b3_coverage_pass'] for p in baselines)
    noise = None
    if coverage:
        medians = [p['latency_ms']['p50'] for p in baselines]
        noise = dict(p50_min_ms=min(medians), p50_max_ms=max(medians), p50_spread_ms=max(medians) - min(medians),
                     frame_p50_min=min(p['latency_frames']['p50'] for p in baselines),
                     frame_p50_max=max(p['latency_frames']['p50'] for p in baselines))
    judged = []
    for case in cases:
        for p in case['peers']:
            feel_pass = p['b3_coverage_pass'] and (case['arm'] == 'sp' or coverage and
                noise['p50_min_ms'] <= p['latency_ms']['p50'] <= noise['p50_max_ms'] and
                noise['frame_p50_min'] <= p['latency_frames']['p50'] <= noise['frame_p50_max'])
            judged.append(dict(arm=case['arm'], repeat=case['repeat'], peer=p['peer'], latency_ms=p['latency_ms'],
                               latency_frames=p['latency_frames'], b3_pass=feel_pass,
                               b1_delay_pass=p['b1_delay_pass'], b2_depth_pass=p['b2_depth_pass'], b4_overlay_pass=p['b4_overlay_pass'],
                               c_duplicates_pass=p['c_duplicates_pass'], c_own_hold_pass=p['c_own_hold_pass'],
                               preview_ms_per_actor_tick=p['preview_ms_per_actor_tick'], delay_min=p['delay_min'], delay_max=p['delay_max']))
    complete = repeats >= 3 and set(selected) == set(ARMS) and len(cases) == repeats * len(ARMS) and all(
        c['manifest']['complete'] and c['manifest']['quiet'] and c['manifest']['binary_unchanged'] for c in cases)
    rise_checks = [case['manifest']['live_resize'] for case in cases if case['manifest'].get('lag_step')]
    return dict(complete=complete, single_player_repeat_noise=noise, rows=judged, live_resize_checks=rise_checks,
                pass_check=complete and coverage and all(r['pass_check'] for r in rise_checks) and
                           all(all(p[k] for k in ('b1_delay_pass', 'b2_depth_pass', 'b3_pass', 'b4_overlay_pass', 'c_duplicates_pass', 'c_own_hold_pass')) for p in judged),
                definition='Existing feel.report.input_latencies: first completed presentation whose local actor changes aim/velocity in the requested direction and carries that input. Every missing response remains RED. Each network p50 must lie inside the measured single-player repeat p50 range, in milliseconds and presented frames; no added tolerance.',
                scope='Headed POSIX local two-engine fake-RTT measurement, not a spread/relay proof or physical display scanout measurement.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--only', nargs='+', choices=tuple(ARMS), default=list(ARMS))
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--size', type=int, nargs=2, default=[960, 540], metavar=('WIDTH', 'HEIGHT'))
    parser.add_argument('--lag-step', type=int, nargs=2, metavar=('FRAME', 'PER_ENGINE_LAG_MS'),
                        help='raise the 30 ms pair to 200 ms RTT at a live outgoing frame (use 600 100)')
    parser.add_argument('--preview-cap', type=int, default=20,
                        help='private legacy preview cap setting; bounded matches must still cover their whole delay')
    parser.add_argument('--host-stall', type=int, nargs=2, metavar=('TICK', 'MILLISECONDS'),
                        help='exercise the standard own-hold text watch through a live host stall')
    parser.add_argument('--dump-window', type=int, nargs=2, metavar=('FROM_TICK', 'THROUGH_TICK'),
                        help='retain per-object simulation and controller dumps for a failing handoff')
    args = parser.parse_args()
    if sys.platform not in ('linux', 'darwin') or args.repeats < 1 or not 1024 <= args.port <= 49151:
        parser.error('requires Linux or macOS, positive repeats, and a lane-owned port below 49152')
    if not 640 <= args.size[0] <= 1920 or not 480 <= args.size[1] <= 1080:
        parser.error('size must be within 640x480 and 1920x1080')
    if args.lag_step and (args.only != ['30ms'] or not 180 <= args.lag_step[0] <= 900 or args.lag_step[1] != 100):
        parser.error('the live rise requires --only 30ms and a frame in 180..900 with per-engine lag 100')
    if not 0 <= args.preview_cap <= 65535:
        parser.error('preview cap must be within 0..65535')
    if args.host_stall and (args.only != ['30ms'] or not 180 <= args.host_stall[0] <= 900 or not 1 <= args.host_stall[1] <= 20000):
        parser.error('the host-stall control requires --only 30ms, a tick in 180..900 and duration in 1..20000ms')
    if args.dump_window and not 1 <= args.dump_window[0] <= args.dump_window[1] <= TICKS:
        parser.error('dump window must be an ordered range inside the1200-tick run')
    os.environ.pop('CCCP_HEADLESS', None)
    os.environ.update(CC_RUNNER_HEADED='1', DISPLAY=':0', PYTHONDONTWRITEBYTECODE='1')
    root = args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    binary_sha = sha(engine_executable(REPO))
    write_json(root / 'plan.json', dict(binary_sha256=binary_sha, source_head=subprocess.check_output(
        ['git', 'rev-parse', 'HEAD'], cwd=REPO, text=True).strip(), repeats=args.repeats, arms=args.only,
        lag_step=args.lag_step, preview_cap=args.preview_cap, host_stall=args.host_stall, dump_window=args.dump_window,
        single_player_repeat_noise='Range of repeated SP p50 values; fixed before looking at the network cases.',
        draw_boundary='Completed UploadFrame, including the software swap; no physical scanout claim.'))
    cases = []
    for repeat in range(1, args.repeats + 1):
        for arm in args.only:
            case = launch_case(root, arm, repeat, args.port, binary_sha, args.size, args.lag_step, args.preview_cap, args.host_stall, args.dump_window)
            cases.append(case)
            write_json(root / 'results.json', summarize(cases, args.repeats, args.only))
            if not case['manifest']['complete']:
                return 1
    return 0 if summarize(cases, args.repeats, args.only)['pass_check'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
