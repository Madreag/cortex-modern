"""Keep periodic autosaves on their deadlines on a busy machine (one machine, two engines, proof:false).

A hosted two-peer match saves every --autosave-seconds while both engines share this machine's cores, so neither leaves
the saver much idle time; --host-sim-cost-us makes the host slower than its peer, so its own seat is held and its saves go
through the held host's agreed base. A saver that only gets the leftovers of a machine whose cores are all busy takes far
longer than an interval to write one save, and every later deadline then meets a busy writer. The run passes only when
every gap between two periodic captures stays within an interval and a half, nothing is refused, and both peers exit.
The host's sim-thread cost per capture (capture_ms) is reported against the 2 ms budget.
Each peer's frame pacing is reported for the [frame-pace] windows where no save of that peer is overdue (the frame-cost
detector's classification): the median frame rate, the 95th and 99th percentiles of the per-window worst frame interval
and the peer's holds of its own seat there. --base names a base build's run of this detector (its folder or result.json);
each peer then keeps at least 95 % of the base's frame rate and at most 125 % of its worst frame interval there, and holds
its own seat no more often than the base did.
"""
import argparse
from datetime import datetime, timezone
import json
import platform
from pathlib import Path
import re
import statistics
import subprocess
import sys
import time


def parse_fields(line):
    return {key: int(value) for key, value in re.findall(r'(\w+)=(-?\d+)\b', line)}


def frame_pacing(run_dir, warmup):
    """Each peer's frames in the windows where no save of that peer is overdue."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import test_autosave_frame_cost as fc
    def text(role):
        path = run_dir / role / 'stdout.log'
        return path.read_text(errors='replace') if path.exists() else ''
    # The client's own deadlines follow the same agreed schedule as the host's.
    dues = fc.timeline(text('host'), True)[2]
    out = {}
    for local, role in ((1, 'host'), (2, 'client')):
        windows, saves, _, bases, holds = fc.timeline(text(role), role == 'host')
        kinds, _ = fc.classify(windows, saves, dues, bases, warmup)
        chosen = [w for w, k in zip(windows, kinds) if k in ('idle', 'save', 'edge')]
        if not chosen:
            out[role] = {'windows': 0}
            continue
        worst = [w['worst_ms'] for w in chosen]
        out[role] = {'windows': len(chosen), 'overdue_windows': kinds.count('overdue'),
                     'fps_median': round(statistics.median(w['fps'] for w in chosen), 2),
                     'worst_ms_p95': round(fc.percentile(worst, 0.95), 2), 'worst_ms_p99': round(fc.percentile(worst, 0.99), 2),
                     'own_seat_holds': sum(1 for h in holds if h['local'] == local and h['peer'] == local and
                                           any(w['first'] <= h['frame'] <= w['last'] for w in chosen))}
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repo', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--autosave-seconds', type=int, default=10)
    p.add_argument('--periodic-captures', type=int, default=8)
    p.add_argument('--host-sim-cost-us', type=int, default=0)
    p.add_argument('--warmup-ticks', type=int, default=600)
    p.add_argument('--limit-seconds', type=int, default=900)
    p.add_argument('--port', type=int, default=49484)
    p.add_argument('--base', type=Path, help="a base build's run of this detector (its folder or result.json)")
    a = p.parse_args()
    base_dir = None
    if a.base:
        found = [a.base.resolve()] if a.base.is_file() else sorted(a.base.resolve().rglob('result.json'))
        if not found or not (found[0].parent / 'host' / 'stdout.log').exists():
            raise SystemExit(f'no base run at {a.base}')
        base_dir = found[0].parent
    repo = a.repo.resolve()
    sys.path.insert(0, str(repo / 'tools'))
    from run_sim_test import make_run, seed_settings, engine_executable, file_sha256
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out / 'driver-as-run.py').write_bytes(Path(__file__).read_bytes())
    quit_path = out / 'quit.json'
    menu = out / 'menu.txt'
    menu.write_text(f'in_play\nwait_file {quit_path.as_posix()} {a.limit_seconds + 120}\nexit\n')
    ticks = str(60 * (a.limit_seconds + 120))
    common = ['-net-port', str(a.port), '-net-match-peers', '2', '-net-match-input-delay', '1',
              '-net-autosave-seconds', str(a.autosave_seconds), '-net-match-ticks', ticks, '-max-ticks', ticks,
              '-menu-script', str(menu)]
    argv = {'host': ['-net-match-service-e2e', '-net-host'] + common,
            'client': ['-net-match-service-e2e', '-net-join', '127.0.0.1'] + common}
    env = {'CCCP_HEADLESS': '1', 'CC_RUNNER_IGNORE_FULLSCREEN': '1', 'CC_TEST_MEMORY_CENSUS_TICKS': '60',
           'CC_TEST_NET_MATCH_E2E_WAIT_PEERS': '1'}
    settings = {'AutosaveSeconds': '0', 'NetworkHostDelayPolicy': 'Fixed', 'NetworkIceEnable': '0',
                'NetworkPortMapEnable': '0', 'NetworkHostVisibility': 'Unlisted', 'EnableLuaDebugging': '0',
                'ResolutionX': '960', 'ResolutionY': '540', 'EnableVSync': '0'}
    runs, records, started = {}, {}, []
    for role in ('host', 'client'):
        args = argv[role] + ['-net-match-report', str(out / f'{role}-report.json'),
                             '-input-script', str(repo / 'tools/e2e/play-input.txt'),
                             '-net-match-service-preset', 'Determinism FeelBaseline', '-net-match-service-module', 'UserScenes.rte']
        role_env = dict(env)
        if role == 'host' and a.host_sim_cost_us:
            role_env['CCCP_TEST_SIM_COST_US'] = str(a.host_sim_cost_us)
        # Each engine leaves when the driver has seen its captures.
        probe_dir = out / f'{role}-probe'
        probe_dir.mkdir()
        probe_path = probe_dir / 'probe.json'
        probe_path.write_text(json.dumps({'schema': 1, 'timeout_ms': (a.limit_seconds + 300) * 1000,
                                          'steps': [{'op': 'wait_file', 'path': str(quit_path)}, {'op': 'finish', 'quit': True}]}, indent=2))
        role_env['CC_TEST_NET_UI_SCRIPT'] = str(probe_path)
        runs[role] = make_run(repo, args, out / role, a.limit_seconds + 600, env=role_env)
        seed_settings(runs[role], settings)
        fixture = Path(runs[role].cwd) / 'Userdata/UserScenes.rte'
        fixture.mkdir()
        source = (repo / 'tools/feel/FeelBaseline.lua').read_text()
        source, count = re.subn(r'^local WINDOW_TICKS = \d+;$', f'local WINDOW_TICKS = {ticks};', source, flags=re.M)
        assert count == 1
        (fixture / 'FeelBaseline.lua').write_text(source)
        (fixture / 'Index.ini').write_text('DataModule\n\tModuleName = User Scenes\n\tScanFolderContents = 1\n\tIgnoreMissingItems = 1\n'
            '\tAddActivity = GAScripted\n\t\tCopyOf = P4 Alpha Duel\n\t\tPresetName = Determinism FeelBaseline\n'
            '\t\tScriptPath = UserScenes.rte/FeelBaseline.lua\n\t\tLuaClassName = FeelBaseline\n'
            '\t\tTeamOfPlayer1 = 0\n\t\tPlayer1IsHuman = 1\n\t\tTeamOfPlayer2 = 1\n\t\tPlayer2IsHuman = 1\n')
    interval_ticks = 60 * a.autosave_seconds
    config = {'topology': 'single-box', 'proof': False, 'box': platform.node(), 'autosave_seconds': a.autosave_seconds,
              'host_sim_cost_us': a.host_sim_cost_us, 'input_delay_frames': 1, 'settings': settings, 'environment': env,
              'source_head': subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip(),
              'exe_sha256': file_sha256(engine_executable(repo)),
              'started_utc': datetime.now(timezone.utc).isoformat(), 'base_run': str(base_dir) if base_dir else None}
    (out / 'configuration.json').write_text(json.dumps(config, indent=2))

    def log(role):
        path = out / role / 'stdout.log'
        return path.read_text(errors='replace') if path.exists() else ''

    def periodic(role):
        # A periodic capture is named on the stream ("named tick= at="); a held host's comes from its agreed base request.
        named = [int(t) for t in re.findall(r'^\[autosave\] named tick=(\d+) at=\d+ writers=\d+', log(role), re.M)]
        taken = [int(t) for t in re.findall(r'^\[autosave\] tick=(\d+) capture_ms=', log(role), re.M)]
        return named if named else taken

    failure = None
    began = time.monotonic()
    try:
        for role in ('host', 'client'):
            runs[role].start()
            started.append(role)
        until = time.monotonic() + a.limit_seconds
        while len(periodic('host')) < a.periodic_captures + 1:
            if time.monotonic() > until:
                raise RuntimeError(f'only {len(periodic("host"))} periodic captures in {a.limit_seconds} s')
            if any(runs[role].poll() is not None for role in started):
                raise RuntimeError('an engine ended before the captures')
            time.sleep(0.5)
    except Exception as error:
        failure = repr(error)
    finally:
        quit_path.write_text(json.dumps({'requested_utc': datetime.now(timezone.utc).isoformat()}))
        for role in started:
            records[role] = runs[role].finish()
        for run in runs.values():
            run.close()
    host = log('host')
    captures = periodic('host')
    gaps = [later - earlier for earlier, later in zip(captures, captures[1:])]
    lateness = [parse_fields(line) for line in re.findall(r'^\[autosave\] periodic due named .*$', host, re.M)]
    capture_ms = [float(ms) for ms in re.findall(r'^\[autosave\] tick=\d+ capture_ms=([0-9.]+)', host, re.M)]
    walks = [int(us) for us in re.findall(r'^\[autosave\] tick=\d+ graph_walk_us=(\d+)', host, re.M)]
    refused = {role: re.findall(r'^\[autosave\] (?:named tick=\d+ not taken: .*|failed .*)$', log(role), re.M) for role in started}
    checks = {'no_driver_failure': failure is None,
              'all_exit_zero': len(records) == 2 and all(r.get('exit_code') == 0 and not r.get('timed_out') for r in records.values()),
              'periodic_captures_taken': len(captures) >= a.periodic_captures + 1,
              'every_gap_within_an_interval_and_a_half': bool(gaps) and max(gaps) <= interval_ticks * 3 // 2,
              'no_capture_refused': all(not refused[role] for role in started)}
    pacing = frame_pacing(out, a.warmup_ticks)
    base_pacing = frame_pacing(base_dir, a.warmup_ticks) if base_dir else None
    if base_pacing:
        for role in ('host', 'client'):
            mine, theirs = pacing[role], base_pacing[role]
            enough = mine['windows'] >= 10 and theirs['windows'] >= 10
            checks[f'{role}_frames_equal_to_base'] = enough and mine['fps_median'] >= 0.95 * theirs['fps_median']
            checks[f'{role}_worst_frame_equal_to_base'] = enough and mine['worst_ms_p95'] <= 1.25 * theirs['worst_ms_p95']
            checks[f'{role}_own_seat_holds_equal_to_base'] = enough and mine['own_seat_holds'] <= theirs['own_seat_holds']
    result = {**config, 'pass': all(checks.values()), 'checks': checks, 'failure': failure, 'wall_seconds': round(time.monotonic() - began, 1),
              'frame_pacing': pacing, 'base_frame_pacing': base_pacing,
              'interval_ticks': interval_ticks, 'captures': captures, 'gaps': gaps, 'max_gap': max(gaps) if gaps else None,
              'late_ticks': [row.get('late_ticks') for row in lateness], 'waited_ticks': [row.get('waited_ticks') for row in lateness],
              'held': [row.get('held') for row in lateness],
              'capture_ms_median': statistics.median(capture_ms) if capture_ms else None, 'capture_ms_max': max(capture_ms) if capture_ms else None,
              'capture_ms': capture_ms, 'graph_walk_ms': [round(us / 1000, 1) for us in walks], 'refused': refused,
              'exits': {role: r.get('exit_code') for role, r in records.items()}}
    (out / 'result.json').write_text(json.dumps(result, indent=2))
    print(json.dumps({k: result[k] for k in ('pass', 'checks', 'failure', 'max_gap', 'interval_ticks', 'late_ticks', 'capture_ms_median', 'capture_ms_max',
                                              'frame_pacing', 'base_frame_pacing')}))
    return 0 if result['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
