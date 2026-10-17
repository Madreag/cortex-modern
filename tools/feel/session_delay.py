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
    for proc in Path('/proc').iterdir():
        if not proc.name.isdecimal():
            continue
        try:
            exe = (proc / 'exe').resolve(strict=True)
            if exe.name in ('ninja', 'CortexCommand', 'CortexCommand.exe'):
                found.append({'pid': int(proc.name), 'exe': str(exe)})
        except (OSError, ValueError):
            pass
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


def reduce_peer(folder, peer, rtt, plan):
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
    preview_cost = preview.get('ms_total', 0) / preview['actor_ticks'] if preview.get('actor_ticks') else None
    watches = watch_results(folder / peer / 'stdout.log')
    delay_watches = [w for w in watches if w['watch'] == 'b4-delay']
    delay_values = [r['delay'] for r in frames if r['actors']]
    limit = 6 if rtt == 30 else 16 if rtt == 200 else 0
    coverage = bool(frames) and not missing and len(latencies) == expected_responses and not latency_missing
    result = dict(peer=peer, requested_rtt_ms=rtt, raw=str(raw), raw_sha256=sha(raw),
                  input_coverage=dict(expected_responses=expected_responses, observed_responses=len(latencies),
                                      missing_script_edges=missing, responses_not_drawn=len(latency_missing)),
                  latency_ms=report.distribution(measured),
                  latency_frames=report.distribution([r['frames'] for r in latencies if r['frames'] is not None]),
                  input_edges=latencies, delay_min=min(delay_values, default=None), delay_max=max(delay_values, default=None),
                  delay_depth_counts=[dict(delay=d, depth=p, frames=n) for (d, p), n in sorted(depths.items())],
                  depth_mismatches=mismatches, missing_activity_state=missing_state, preview=preview, preview_ms_per_actor_tick=preview_cost,
                  rtt_samples=[dict(tick=r['tick'], peer=p['peer'], ping_ms=p['ping_ms'])
                               for r in frames if r['tick'] % 60 == 0 for p in r.get('rtt', []) if not p['local']],
                  captures=[r for r in rows if r['type'] == 'capture'], delay_watches=delay_watches,
                  b1_delay_pass=bool(delay_values) and max(delay_values) <= limit,
                  b2_depth_pass=bool(owed) and not mismatches and not missing_state and (rtt is None or preview.get('enabled') and preview.get('previews', 0) > 0),
                  b3_coverage_pass=coverage,
                  b4_overlay_pass=rtt is None or bool(delay_watches) and all(w['active_frames'] > 0 and w['violations'] == 0 for w in delay_watches))
    write_json(folder / f'{peer}-measurement.json', result)
    return result


def launch_case(root, arm, repeat, port, exe_sha, size):
    wait_quiet()
    scratch_bytes(root.parent)
    out = root / f'{repeat:02d}-{arm}'
    out.mkdir()
    script = out / 'input.txt'
    input_pattern(script)
    plan = json.loads(script.with_name('input-schedule.json').read_text())
    watches = out / 'screen-watches.txt'
    watches.write_text('b4-delay input_delay running\n', encoding='utf-8')
    rtt = ARMS[arm]
    peers = ['sp'] if rtt is None else ['host', 'client']
    runs, receipts = {}, {}
    manifest = dict(started=stamp(), arm=arm, repeat=repeat, requested_rtt_ms=rtt, ticks=TICKS,
                    per_engine_lag_ms=0 if rtt is None else rtt // 2, headed=True, display=':0', size=size,
                    binary=str(engine_executable(REPO)), binary_sha256=exe_sha, quiet_samples=[])
    write_json(out / 'manifest.json', manifest)
    try:
        for peer in peers:
            run_out = out / peer
            trace = out / f'{peer}_trace.json'
            flags = ['-seed', '42', '-max-ticks', str(TICKS), '-tick-hashes', '-out', str(trace),
                     '-input-script', str(script), '-feel-render-settings', str(run_out / 'runtime/Userdata/FeelRender.ini'),
                     '-feel-measure', str(run_out / 'feel')]
            if rtt is None:
                flags += ['-scenario', 'FeelBaseline']
            else:
                flags += ['-net-match-service-e2e', '-net-port', str(port), '-net-match-ticks', str(TICKS),
                          '-net-match-humans', '2', '-net-match-peers', '2', '-net-match-cpu-slots', '0',
                          '-net-match-service-preset', 'Determinism FeelBaseline', '-net-match-service-module', 'UserScenes.rte',
                          '-net-match-auto-delay', '-net-fake-lag', str(rtt // 2),
                          '-net-reconnect-ticket', str(out / f'{peer}.ticket'), '-net-match-report', str(out / f'{peer}_report.json')]
                flags += ['-net-host'] if peer == 'host' else ['-net-join', '127.0.0.1']
            environment = dict(CC_RUNNER_HEADED='1', DISPLAY=':0', CC_TEST_FEEL_HEADED='1', CCCP_TEST_SINGLE_SCREEN='1',
                               CCCP_TEST_SCREEN_WATCHES=str(watches), CC_TEST_VSYNC_OFF='1', PYTHONDONTWRITEBYTECODE='1')
            if rtt is not None:
                environment['CC_TEST_NET_MATCH_E2E_WAIT_PEERS'] = '1'
            expected = [trace, run_out / 'feel/raw.jsonl'] + ([out / f'{peer}_report.json'] if rtt is not None else [])
            run = make_run(REPO, flags, run_out, timeout=900, env=environment, expected=expected)
            runs[peer] = run
            private_settings(run, 60)
            settings = Path(run.cwd) / 'Userdata/Settings.ini'
            text = settings.read_text(encoding='utf-8')
            for key, value in zip(('ResolutionX', 'ResolutionY'), size):
                text = re.sub(rf'(?m)^([ \t]*{key}[ \t]*=[ \t]*)[^\r\n]*', lambda match: match[1] + str(value), text)
            settings.write_text(text, encoding='utf-8')
            runtime_path = run_out / 'runtime.json'
            runtime = json.loads(runtime_path.read_text())
            runtime['settings_overrides'].update(dict(zip(('ResolutionX', 'ResolutionY'), map(str, size))))
            runtime['settings_sha256'] = sha(settings)
            write_json(runtime_path, runtime)
            stage_baseline(run, TICKS, 2)
            (run_out / 'feel').mkdir()
            run.start()
            if peer == 'host':
                time.sleep(.75)
        while any(run.poll() is None for run in runs.values()):
            own = {run.record.get('pid') for run in runs.values()}
            live = contenders()
            manifest['quiet_samples'].append(dict(time=stamp(), foreign=[p for p in live if p['pid'] not in own], loadavg=os.getloadavg()))
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
    results = [reduce_peer(out, peer, rtt, plan) for peer in peers] if manifest['complete'] else []
    if rtt is not None and manifest['complete']:
        manifest['peer_hashes'] = compare_pair(out / 'host_trace.json', out / 'client_trace.json', cross_peer=True)
        manifest['complete'] = manifest['peer_hashes']['pass']
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
                               preview_ms_per_actor_tick=p['preview_ms_per_actor_tick'], delay_min=p['delay_min'], delay_max=p['delay_max']))
    complete = repeats >= 3 and set(selected) == set(ARMS) and len(cases) == repeats * len(ARMS) and all(
        c['manifest']['complete'] and c['manifest']['quiet'] and c['manifest']['binary_unchanged'] for c in cases)
    return dict(complete=complete, single_player_repeat_noise=noise, rows=judged,
                pass_check=complete and coverage and all(all(p[k] for k in ('b1_delay_pass', 'b2_depth_pass', 'b3_pass', 'b4_overlay_pass')) for p in judged),
                definition='Existing feel.report.input_latencies: first completed presentation whose local actor changes aim/velocity in the requested direction and carries that input. Every missing response remains RED. Each network p50 must lie inside the measured single-player repeat p50 range, in milliseconds and presented frames; no added tolerance.',
                scope='Headed Linux local two-engine fake-RTT measurement, not a spread/relay proof or physical display scanout measurement.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--only', nargs='+', choices=tuple(ARMS), default=list(ARMS))
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--size', type=int, nargs=2, default=[960, 540], metavar=('WIDTH', 'HEIGHT'))
    args = parser.parse_args()
    if sys.platform != 'linux' or args.repeats < 1 or not 1024 <= args.port <= 49151:
        parser.error('requires Linux, positive repeats, and a lane-owned port below 49152')
    if not 640 <= args.size[0] <= 1920 or not 480 <= args.size[1] <= 1080:
        parser.error('size must be within 640x480 and 1920x1080')
    os.environ.pop('CCCP_HEADLESS', None)
    os.environ.update(CC_RUNNER_HEADED='1', DISPLAY=':0', PYTHONDONTWRITEBYTECODE='1')
    root = args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    binary_sha = sha(engine_executable(REPO))
    write_json(root / 'plan.json', dict(binary_sha256=binary_sha, source_head=subprocess.check_output(
        ['git', 'rev-parse', 'HEAD'], cwd=REPO, text=True).strip(), repeats=args.repeats, arms=args.only,
        single_player_repeat_noise='Range of repeated SP p50 values; fixed before looking at the network cases.',
        draw_boundary='Completed UploadFrame, including the software swap; no physical scanout claim.'))
    cases = []
    for repeat in range(1, args.repeats + 1):
        for arm in args.only:
            case = launch_case(root, arm, repeat, args.port, binary_sha, args.size)
            cases.append(case)
            write_json(root / 'results.json', summarize(cases, args.repeats, args.only))
            if not case['manifest']['complete']:
                return 1
    return 0 if summarize(cases, args.repeats, args.only)['pass_check'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
