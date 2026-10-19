"""Capture live Constructor and stock grenade-launcher slices, then return from their world image; two local peers, proof:false."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import sys
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--repo', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--port', type=int, default=49472)
p.add_argument('--lua-owned-holder', action='store_true', help='Keep the Constructor actor under Lua ownership, outside the world actor list.')
p.add_argument('--auto-delay', action='store_true', help='Use the engine Auto return-delay policy with the same one-frame initial delay.')
a = p.parse_args()
sys.path.insert(0, str(a.repo / 'tools'))
from run_sim_test import make_run, seed_settings, engine_executable, file_sha256

plant = (Path(__file__).with_name('fixtures') / 'device_slice_checkpoint.lua').read_text()
if a.lua_owned_holder:
    assert plant.count('MovableMan:AddActor(holder);') == 1
    plant = plant.replace('MovableMan:AddActor(holder);', '-- Kept by Lua only; never entered in the world actor list.', 1)
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
(out / 'driver-as-run.py').write_bytes(Path(__file__).read_bytes())
(out / 'slice-fixture.lua').write_text(plant)
ticket = out / 'client.ticket'
quit_path, leave_path = out / 'quit.json', out / 'leave.json'
request_paths = [out / f'request-{i}.json' for i in range(2)]
menu = out / 'menu.txt'
menu.write_text(f'in_play\nwait_file {quit_path.as_posix()} 300\nexit\n')
common = ['-net-match-service-e2e', '-net-port', str(a.port), '-net-match-peers', '2',
          '-net-match-input-delay', '1', '-net-autosave-seconds', '0', '-net-match-ticks', '12000',
          '-net-local-prediction', 'off',
          '-max-ticks', '12000', '-net-match-service-preset', 'Determinism FeelBaseline',
          '-net-match-service-module', 'UserScenes.rte', '-menu-script', str(menu),
          '-input-script', str(a.repo / 'tools/e2e/play-input.txt')]
if a.auto_delay:
    common += ['-net-match-auto-delay']
env = dict(CCCP_HEADLESS='1', CC_RUNNER_IGNORE_FULLSCREEN='1', CCCP_CHECKPOINT_PHASES='1',
           CC_TEST_NET_MATCH_E2E_WAIT_PEERS='1', CC_TEST_MEMORY_CENSUS_TICKS='60')
settings = dict(AutosaveSeconds='0', NetworkHostDelayPolicy='Auto' if a.auto_delay else 'Fixed', NetworkIceEnable='0',
                NetworkPortMapEnable='0', NetworkHostVisibility='Unlisted', EnableLuaDebugging='0',
                ResolutionX='960', ResolutionY='540', EnableVSync='0')
runs, records, active = {}, {}, []
result = dict(proof=False, topology='single-box', maximum_concurrent_engines=2,
              lua_owned_holder=a.lua_owned_holder, input_delay_frames=1, delay_policy=settings['NetworkHostDelayPolicy'],
              exe_sha256=file_sha256(engine_executable(a.repo)), checks={})

def prepare(role):
    steps = []
    if role == 'host':
        for request in request_paths:
            steps += [{'op': 'wait_file', 'path': str(request)}, {'op': 'key_down', 'key': 'F5'},
                      {'op': 'key_up', 'key': 'F5'}]
    steps += [{'op': 'wait_file', 'path': str(leave_path if role == 'client' else quit_path)},
              {'op': 'finish', 'quit': True}]
    directory = out / f'{role}-probe'
    directory.mkdir()
    probe = directory / 'probe.json'
    probe.write_text(json.dumps({'schema': 1, 'timeout_ms': 240000, 'steps': steps}, indent=2))
    flags = common + (['-net-host'] if role == 'host' else ['-net-join', '127.0.0.1', '-net-reconnect-ticket', str(ticket)])
    flags += ['-net-match-report', str(out / f'{role}-report.json')]
    run = make_run(a.repo, flags, out / role, 330, env={**env, 'CC_TEST_NET_UI_SCRIPT': str(probe)})
    seed_settings(run, settings)
    fixture = Path(run.cwd) / 'Userdata/UserScenes.rte'
    fixture.mkdir()
    text, count = re.subn(r'^local WINDOW_TICKS = \d+;$', 'local WINDOW_TICKS = 12000;',
                         (a.repo / 'tools/feel/FeelBaseline.lua').read_text(), flags=re.M)
    assert count == 1
    (fixture / 'FeelBaseline.lua').write_text(text + plant)
    (fixture / 'Index.ini').write_text('DataModule\n\tModuleName = User Scenes\n\tScanFolderContents = 1\n\tIgnoreMissingItems = 1\n'
        '\tAddActivity = GAScripted\n\t\tCopyOf = P4 Alpha Duel\n\t\tPresetName = Determinism FeelBaseline\n'
        '\t\tScriptPath = UserScenes.rte/FeelBaseline.lua\n\t\tLuaClassName = FeelBaseline\n'
        '\t\tTeamOfPlayer1 = 0\n\t\tPlayer1IsHuman = 1\n\t\tTeamOfPlayer2 = 1\n\t\tPlayer2IsHuman = 1\n')
    runs[role] = run
    return run

def log(role):
    p = out / role / 'stdout.log'
    return p.read_text(errors='replace') if p.exists() else ''

def tick(role):
    rows = re.findall(r'^\[mem-census\].*?\btick=(\d+)', log(role), re.M)
    return int(rows[-1]) if rows else 0

def saves(role):
    return len(re.findall(r'^\[autosave\] tick=\d+ capture_ms=', log(role), re.M))

def wait_for(check, description, seconds=180):
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        if check(): return
        for role in active:
            if runs[role].poll() is not None: raise RuntimeError(f'{role} ended before {description}')
            if re.search(r'save refused:|FeelBaseline.lua:\d+:|stack traceback',log(role)):
                raise RuntimeError(f'{role} refused a capture or failed the slice fixture before {description}')
        time.sleep(.2)
    raise RuntimeError('timeout waiting for ' + description)

failure = None
try:
    for role in ('host', 'client'):
        prepare(role).start()
        active.append(role)
    wait_for(lambda: all(tick(role) >= 300 for role in active), 'both peers playing')
    for index, request in enumerate(request_paths):
        request.write_text(json.dumps({'requested_at': {role: tick(role) for role in active}}))
        wait_for(lambda: all(saves(role) >= index + 1 for role in active), f'save {index + 1} on both peers')
        time.sleep(3)  # Let the finished inventory release before asking for the next independent capture.
    leave_path.write_text('{}')
    records['client'] = runs['client'].finish()
    active.remove('client')
    assert records['client'].get('exit_code') == 0 and not records['client'].get('timed_out')
    assert ticket.exists(), 'the departing client left no ticket'
    wait_for(lambda: '[net-peer-departure] peer=2 ' in log('host'), 'the host noticing departure')
    returned = prepare('return')
    old_key = Path(runs['client'].cwd) / 'Userdata/NetworkIdentity.key'
    if old_key.exists(): shutil.copy2(old_key, Path(returned.cwd) / 'Userdata/NetworkIdentity.key')
    returned.start()
    active.append('return')
    wait_for(lambda: '[net-match] private base published from the announced capture' in log('host'), 'rejoin base publication')
    wait_for(lambda: '[net-match] private catch-up complete frame=' in log('return'), 'image return completion')
    at = tick('return')
    wait_for(lambda: tick('return') >= at + 120, '120 live ticks after image return')
except Exception as error:
    failure = repr(error)
finally:
    quit_path.write_text('{}')
    for role in active:
        records[role] = runs[role].finish()
    for run in runs.values(): run.close()
    (out / 'run-result.json').write_text(json.dumps(records, indent=2))

logs = {role: log(role) for role in runs}
console_logs = {role: '\n'.join(path.read_text(errors='replace') for path in Path(run.cwd).rglob('LogConsole.txt'))
                for role, run in runs.items()}
result['failure'] = failure
result['checks'] = {
    'all_normal_exits': set(records) == {'host', 'client', 'return'} and all(r.get('exit_code') == 0 and not r.get('timed_out') for r in records.values()),
    'two_saves_each_before_departure': all(saves(role) >= 2 for role in ('host', 'client')),
    'no_capture_refusal': all('save refused:' not in text and 'not taken:' not in text for text in logs.values()),
    'base_published': '[net-match] private base published from the announced capture' in logs['host'],
    'return_complete': '[net-match] private catch-up complete frame=' in logs.get('return', ''),
    'no_fixture_error': all(not re.search(r'FeelBaseline.lua:\d+:|restored device slice lost|image dropped the slice holder|stack traceback', text)
                            for text in [*logs.values(), *console_logs.values()]),
    'no_restore_error': all(not re.search(r'saved game could not be constructed|saved script state did not restore|could not launch the activity', text)
                           for text in [*logs.values(), *console_logs.values()]),
    'returned_value_and_alias_checks_executed': len(set(re.findall(r'\[r0-slice-fixture\][ _]+verified[ _]+updates=(\d+)[ _]+grenade=5[ _]+aliases=6', logs.get('return', '')))) >= 4,
}
result['lines'] = {role: [line for line in text.splitlines() if re.match(r'^\[(autosave|net-match|r0-slice-fixture)\]', line)
                         and any(word in line for word in ('base', 'capture', 'complete', 'bootstrap', 'refused', 'image start'))]
                   for role, text in logs.items()}
result['pass'] = not failure and all(result['checks'].values())
(out / 'summary.json').write_text(json.dumps(result, indent=2))
print(json.dumps(result))
raise SystemExit(0 if result['pass'] else 1)
