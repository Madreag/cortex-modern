"""Name autosaves only when each writer is ready again (one machine, two engines, proof:false).

A hosted two-peer match saves every second while the test lever makes every finished capture take far longer to
retire than the capture lead. After the warm-up the host asks for a manual save. A writer that reopened its gate when
its last capture's future was ready, before that capture retired and the next plan was prepared, names a capture the
saver cannot take: its peers log 'not taken' and the manual ask is consumed. The run passes only when every named
capture is taken by both peers, the manual save is saved by both writers, and the periodic saves keep coming.
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
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--retire-ms', type=int, default=1500)
    p.add_argument('--autosave-seconds', type=int, default=1)
    p.add_argument('--warmup-ticks', type=int, default=900)
    p.add_argument('--named-captures', type=int, default=6)
    p.add_argument('--port', type=int, default=49480)
    a = p.parse_args()
    repo = a.repo.resolve()
    sys.path.insert(0, str(repo / 'tools'))
    from run_sim_test import make_run, seed_settings, engine_executable, file_sha256
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out / 'driver-as-run.py').write_bytes(Path(__file__).read_bytes())
    quit_path = out / 'quit.json'
    menu = out / 'menu.txt'
    menu.write_text(f'in_play\nwait_file {quit_path.as_posix()} 600\nexit\n')
    common = ['-net-port', str(a.port), '-net-match-peers', '2', '-net-match-input-delay', '1',
              '-net-autosave-seconds', str(a.autosave_seconds), '-net-match-ticks', '30000', '-max-ticks', '30000',
              '-menu-script', str(menu)]
    argv = {'host': ['-net-match-service-e2e', '-net-host'] + common,
            'client': ['-net-match-service-e2e', '-net-join', '127.0.0.1'] + common}
    env = {'CCCP_HEADLESS': '1', 'CC_RUNNER_IGNORE_FULLSCREEN': '1', 'CC_TEST_MEMORY_CENSUS_TICKS': '60',
           'CC_TEST_NET_MATCH_E2E_WAIT_PEERS': '1', 'CCCP_TEST_SLOW_CAPTURE_RETIRE_MS': str(a.retire_ms)}
    settings = {'AutosaveSeconds': '0', 'NetworkHostDelayPolicy': 'Fixed', 'NetworkIceEnable': '0',
                'NetworkPortMapEnable': '0', 'NetworkHostVisibility': 'Unlisted', 'EnableLuaDebugging': '0',
                'ResolutionX': '960', 'ResolutionY': '540', 'EnableVSync': '0'}
    runs, records, started = {}, {}, []
    for role in ('host', 'client'):
        args = argv[role] + ['-net-match-report', str(out / f'{role}-report.json'),
                             '-input-script', str(repo / 'tools/e2e/play-input.txt'),
                             '-net-match-service-preset', 'Determinism FeelBaseline', '-net-match-service-module', 'UserScenes.rte']
        role_env = dict(env)
        probe_dir = out / f'{role}-probe'
        probe_dir.mkdir()
        steps = []
        if role == 'host':
            steps = [
                {'op': 'wait', 'service': 'Running', 'sim_at_least': a.warmup_ticks},
                {'op': 'wait_file', 'path': str(out / 'request.json')},
                {'op': 'key_down', 'key': 'F5'}, {'op': 'key_up', 'key': 'F5'},
                {'op': 'signal', 'name': 'requested'}]
        steps += [{'op': 'wait_file', 'path': str(quit_path)}, {'op': 'finish', 'quit': True}]
        probe_path = probe_dir / 'probe.json'
        probe_path.write_text(json.dumps({'schema': 1, 'timeout_ms': 540000, 'steps': steps}, indent=2))
        role_env['CC_TEST_NET_UI_SCRIPT'] = str(probe_path)
        runs[role] = make_run(repo, args, out / role, 600, env=role_env)
        seed_settings(runs[role], settings)
        fixture = Path(runs[role].cwd) / 'Userdata/UserScenes.rte'
        fixture.mkdir()
        source = (repo / 'tools/feel/FeelBaseline.lua').read_text()
        source, count = re.subn(r'^local WINDOW_TICKS = \d+;$', 'local WINDOW_TICKS = 30000;', source, flags=re.M)
        assert count == 1
        (fixture / 'FeelBaseline.lua').write_text(source)
        (fixture / 'Index.ini').write_text('DataModule\n\tModuleName = User Scenes\n\tScanFolderContents = 1\n\tIgnoreMissingItems = 1\n'
            '\tAddActivity = GAScripted\n\t\tCopyOf = P4 Alpha Duel\n\t\tPresetName = Determinism FeelBaseline\n'
            '\t\tScriptPath = UserScenes.rte/FeelBaseline.lua\n\t\tLuaClassName = FeelBaseline\n'
            '\t\tTeamOfPlayer1 = 0\n\t\tPlayer1IsHuman = 1\n\t\tTeamOfPlayer2 = 1\n\t\tPlayer2IsHuman = 1\n')
    config = {'topology': 'single-box', 'proof': False, 'box': platform.node(), 'retire_ms': a.retire_ms,
              'autosave_seconds': a.autosave_seconds, 'input_delay_frames': 1, 'warmup_ticks': a.warmup_ticks,
              'settings': settings, 'environment': env,
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

    def named(role):
        return [(int(t), int(at)) for t, at in re.findall(r'\[autosave\] (?:manual save )?named tick=(\d+) at=(\d+) writers=\d+', log(role))]

    def wait_for(check, description, limit=420):
        until = time.monotonic() + limit
        while time.monotonic() < until:
            if check():
                return
            if any(runs[role].poll() is not None for role in started):
                raise RuntimeError(f'engine ended before {description}')
            time.sleep(0.2)
        raise RuntimeError(f'timeout waiting for {description}')

    failure = None
    try:
        for role in ('host', 'client'):
            runs[role].start()
            started.append(role)
        wait_for(lambda: all(tick(role) >= a.warmup_ticks for role in started), 'both peers warm-up')
        (out / 'request.json').write_text(json.dumps({'ticks': {role: tick(role) for role in started}}))
        wait_for(lambda: re.search(r'\[autosave\] manual save (?:saved|failed) tick=\d+', log('host')), 'the manual save verdict')
        settled = len(named('host'))
        wait_for(lambda: len(named('host')) >= max(a.named_captures, settled + 2), 'periodic captures after the manual save')
    except Exception as error:
        failure = repr(error)
    finally:
        quit_path.write_text(json.dumps({'requested_utc': datetime.now(timezone.utc).isoformat()}))
        for role in started:
            records[role] = runs[role].finish()
        for run in runs.values():
            run.close()
    not_taken = {role: re.findall(r'^\[autosave\] named tick=\d+ not taken: .*$', log(role), re.M) for role in started}
    failed = {role: re.findall(r'^\[autosave\] failed .*$', log(role), re.M) for role in started}
    manual = re.findall(r'\[autosave\] manual save (saved|failed) tick=(\d+) writers=(\d+)/(\d+)', log('host'))
    lever = all(f'[checkpoint-lever] slow_capture_retire_ms={a.retire_ms}' in log(role) for role in started)
    checks = {'no_driver_failure': failure is None,
              'all_exit_zero': len(records) == 2 and all(r.get('exit_code') == 0 and not r.get('timed_out') for r in records.values()),
              'retirement_slowed_on_both_peers': lever,
              'periodic_captures_named': len(named('host')) >= a.named_captures,
              'manual_saved_by_both_writers': manual == [('saved', manual[0][1], '2', '2')] if manual else False,
              'no_capture_refused': all(not not_taken[role] and not failed[role] for role in started)}
    result = {**config, 'pass': all(checks.values()), 'checks': checks, 'failure': failure, 'named': named('host'),
              'manual': manual, 'not_taken': not_taken, 'failed': failed,
              'exits': {role: r.get('exit_code') for role, r in records.items()}}
    (out / 'result.json').write_text(json.dumps(result, indent=2))
    print(json.dumps({k: result[k] for k in ('pass', 'checks', 'failure', 'manual')}))
    return 0 if result['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
