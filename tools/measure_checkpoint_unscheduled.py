"""Measure unscheduled capture arms with autosaves off on one machine (proof:false).

Run through the platform's existing headless session launcher. Both cases warm up
for 1800 ticks. A fresh late world join names a capture with the minimum two-tick
lead; a hosted-player manual save uses the minimum legal three-tick lead. The
manual case uses the checked-in FeelBaseline fixture so the activity stays alive.
World bootstrap retires before the late joiner starts: at most two engines run.

Every run preserves its source, settings, binary hash, native logs, arm costs and
completion checks. A completed capture does not make a failed world join pass.
"""
import argparse
from datetime import datetime, timezone
import json
import platform
from pathlib import Path
import re
import subprocess
import sys
import time


def parse_fields(line):
    return {key: int(value) for key, value in re.findall(r'(\w+)=(-?\d+)\b', line)}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repo', type=Path, required=True)
    p.add_argument('--checkout', type=Path)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--case', choices=('world-join', 'manual'), required=True)
    p.add_argument('--warmup-ticks', type=int, default=1800)
    p.add_argument('--port', type=int, default=49470)
    a = p.parse_args()
    repo = (a.checkout or a.repo).resolve()
    sys.path.insert(0, str(repo / 'tools'))
    from run_sim_test import engine_executable, file_sha256, make_run, seed_settings
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out / 'driver-as-run.py').write_bytes(Path(__file__).read_bytes())
    quit_path = out / 'quit.json'
    menu = out / 'menu.txt'
    menu.write_text(f'in_play\nwait_file {quit_path.as_posix()} 300\nexit\n')
    peers = '3' if a.case == 'world-join' else '2'
    # A seatless world may use zero. A hosted player match rejects zero for its client seats.
    delay = '0' if a.case == 'world-join' else '1'
    common = ['-net-port', str(a.port), '-net-match-peers', peers, '-net-match-input-delay', delay,
              '-net-autosave-seconds', '0', '-net-match-ticks', '12000', '-max-ticks', '12000',
              '-menu-script', str(menu)]
    host_args = (['-net-dedicated', '-net-persistent-world', '-net-world-fresh'] if a.case == 'world-join'
                 else ['-net-match-service-e2e', '-net-host'])
    argv = {'host': host_args + common,
            'client': ['-net-match-service-e2e', '-net-join', '127.0.0.1'] + common}
    env = {'CCCP_HEADLESS': '1', 'CC_RUNNER_IGNORE_FULLSCREEN': '1',
           'CCCP_CHECKPOINT_PHASES': '1', 'CC_TEST_MEMORY_CENSUS_TICKS': '60'}
    if a.case == 'manual':
        env['CC_TEST_NET_MATCH_E2E_WAIT_PEERS'] = '1'
    settings = {'AutosaveSeconds': '0', 'NetworkHostDelayPolicy': 'Fixed', 'NetworkIceEnable': '0',
                'NetworkPortMapEnable': '0', 'NetworkHostVisibility': 'Unlisted', 'EnableLuaDebugging': '0',
                'ResolutionX': '960', 'ResolutionY': '540', 'EnableVSync': '0'}
    runs, records, started, active = {}, {}, [], []
    for role in ('host', 'client'):
        args = argv[role] + ['-net-match-report', str(out / f'{role}-report.json')]
        if a.case == 'manual':
            args += ['-input-script', str(repo / 'tools/e2e/play-input.txt'),
                     '-net-match-service-preset', 'Determinism FeelBaseline', '-net-match-service-module', 'UserScenes.rte']
        role_env = dict(env)
        probe_dir = out / f'{role}-probe'
        probe_dir.mkdir()
        steps = []
        if role == 'host' and a.case == 'manual':
            steps = [
                {'op': 'wait', 'service': 'Running', 'sim_at_least': a.warmup_ticks},
                {'op': 'wait_file', 'path': str(out / 'request.json')},
                {'op': 'key_down', 'key': 'F5'}, {'op': 'key_up', 'key': 'F5'},
                {'op': 'signal', 'name': 'requested'}]
        steps += [{'op': 'wait_file', 'path': str(quit_path)}, {'op': 'finish', 'quit': True}]
        probe = {'schema': 1, 'timeout_ms': 240000, 'steps': steps}
        probe_path = probe_dir / 'probe.json'
        probe_path.write_text(json.dumps(probe, indent=2))
        role_env['CC_TEST_NET_UI_SCRIPT'] = str(probe_path)
        runs[role] = make_run(repo, args, out / role, 330, env=role_env)
        seed_settings(runs[role], settings)
        if a.case == 'manual':
            fixture = Path(runs[role].cwd) / 'Userdata/UserScenes.rte'
            fixture.mkdir()
            source = (repo / 'tools/feel/FeelBaseline.lua').read_text()
            source, count = re.subn(r'^local WINDOW_TICKS = \d+;$', 'local WINDOW_TICKS = 12000;', source, flags=re.M)
            assert count == 1
            (fixture / 'FeelBaseline.lua').write_text(source)
            (fixture / 'Index.ini').write_text('DataModule\n\tModuleName = User Scenes\n\tScanFolderContents = 1\n\tIgnoreMissingItems = 1\n'
                '\tAddActivity = GAScripted\n\t\tCopyOf = P4 Alpha Duel\n\t\tPresetName = Determinism FeelBaseline\n'
                '\t\tScriptPath = UserScenes.rte/FeelBaseline.lua\n\t\tLuaClassName = FeelBaseline\n'
                '\t\tTeamOfPlayer1 = 0\n\t\tPlayer1IsHuman = 1\n\t\tTeamOfPlayer2 = 1\n\t\tPlayer2IsHuman = 1\n')
    if a.case == 'world-join':
        # A fresh world needs its first client to open the round. Retire that process before starting the late joiner:
        # two client seats are authored, but only the host and one client ever run together.
        bootstrap_args = ['-net-match-service-e2e', '-net-join', '127.0.0.1', '-net-port', str(a.port),
                          '-net-match-peers', peers, '-net-match-input-delay', '0', '-net-autosave-seconds', '0',
                          '-net-match-ticks', '600', '-max-ticks', '600',
                          '-input-script', str(repo / 'tools/e2e/play-input.txt'),
                          '-net-match-report', str(out / 'bootstrap-report.json')]
        runs['bootstrap'] = make_run(repo, bootstrap_args, out / 'bootstrap', 180, env=env)
        seed_settings(runs['bootstrap'], settings)
    config = {'case': a.case, 'topology': 'single-box', 'proof': False, 'box': platform.node(),
              'autosave_seconds': 0, 'input_delay_frames': int(delay), 'delay_policy': 'Fixed',
              'warmup_ticks': a.warmup_ticks, 'maximum_concurrent_engines': 2,
              'authored_peer_capacity': int(peers), 'settings': settings, 'environment': env,
              'source_head': subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip(),
              'exe_sha256': file_sha256(engine_executable(repo)),
              'started_utc': datetime.now(timezone.utc).isoformat()}
    (out / 'configuration.json').write_text(json.dumps(config, indent=2))

    def log(role):
        path = out / role / 'stdout.log'
        return path.read_text(errors='replace') if path.exists() else ''

    def tick(role):
        rows = [parse_fields(line) for line in log(role).splitlines() if line.startswith('[mem-census]')]
        return rows[-1].get('tick', 0) if rows else 0

    def wait_for(check, description, limit=180):
        until = time.monotonic() + limit
        while time.monotonic() < until:
            if check():
                return
            if any(runs[role].poll() is not None for role in active):
                raise RuntimeError(f'engine ended before {description}')
            time.sleep(0.2)
        raise RuntimeError(f'timeout waiting for {description}')

    failure = None
    join_request_tick = None
    try:
        runs['host'].start()
        started.append('host')
        active.append('host')
        if a.case == 'world-join':
            runs['bootstrap'].start()
            started.append('bootstrap')
            # Its tick-limited normal exit leaves the world running with the departed seat held.
            records['bootstrap'] = runs['bootstrap'].finish()
            if records['bootstrap'].get('exit_code') != 0 or records['bootstrap'].get('timed_out'):
                raise RuntimeError('bootstrap client did not finish normally')
            wait_for(lambda: tick('host') >= a.warmup_ticks, 'host world warm-up')
            join_request_tick = tick('host')
            (out / 'join-request.json').write_text(json.dumps({'host_tick': join_request_tick, 'monotonic': time.monotonic()}))
        runs['client'].start()
        started.append('client')
        active.append('client')
        if a.case == 'manual':
            wait_for(lambda: all(tick(role) >= a.warmup_ticks for role in active), 'both peers warm-up')
            (out / 'request.json').write_text(json.dumps({'ticks': {role: tick(role) for role in started}}))
            wait_for(lambda: re.search(r'\[autosave\] manual save saved tick=\d+ writers=2/2', log('host')), 'both manual writers')
        else:
            wait_for(lambda: any(int(at) >= join_request_tick for at in re.findall(r'\[net-world\] activate peer=\d+ at=(\d+)', log('host')))
                     and '[net-world] catch-up complete peer=' in log('client'), 'late world join activation and catch-up')
            activated = tick('host')
            wait_for(lambda: tick('host') >= activated + 120, 'post-join live progress')
    except Exception as error:
        failure = repr(error)
    finally:
        quit_path.write_text(json.dumps({'requested_utc': datetime.now(timezone.utc).isoformat()}))
        for role in active:
            records[role] = runs[role].finish()
        for run in runs.values():
            run.close()
    arms = {role: [parse_fields(line) for line in log(role).splitlines() if line.startswith('[checkpoint-arm-split]')]
            for role in started}
    named_pattern = r'\[autosave\] ' + ('manual save ' if a.case == 'manual' else '') + r'named tick=(\d+) at=(\d+) writers=(\d+)'
    all_named = [{'tick': int(t), 'at': int(at), 'writers': int(w), 'lead_ticks': int(t) - int(at)}
             for t, at, w in re.findall(named_pattern, log('host'))]
    named = [row for row in all_named if a.case != 'world-join' or (join_request_tick is not None and row['at'] >= join_request_tick)]
    selected_role = 'client' if a.case == 'manual' else 'host'
    selected = [row for row in arms.get(selected_role, []) if any(row['tick'] == n['tick'] for n in named)]
    expected_runs = 3 if a.case == 'world-join' else 2
    checks = {'no_driver_failure': failure is None, 'all_exit_zero': len(records) == expected_runs and all(r.get('exit_code') == 0 and not r.get('timed_out') for r in records.values()),
              'one_named_capture': len(named) == 1, 'minimum_lead': len(named) == 1 and named[0]['lead_ticks'] == int(delay) + 2,
              'one_measured_arm': len(selected) == 1 and all(k in selected[0] for k in ('native_arm_us', 'lua_arm_us')),
              # A peer still replaying its catch-up is not live at a named tick and is not one of its writers; a live peer must take it.
              'no_capture_refusals': all('not taken: catch_up=false' not in log(role) and '[autosave] failed' not in log(role) for role in started)}
    result = {**config, 'pass': all(checks.values()), 'checks': checks, 'failure': failure,
              'named': named, 'all_named': all_named, 'join_request_tick': join_request_tick,
              'measured_role': selected_role, 'measured_arms': selected, 'all_arms': arms,
              'exits': {role: r.get('exit_code') for role, r in records.items()}}
    (out / 'cost-summary.json').write_text(json.dumps(result, indent=2))
    print(json.dumps(result))
    return 0 if result['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
