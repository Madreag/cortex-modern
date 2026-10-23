"""Return a departed seat to a match whose host fixed the input delay; two local peers, proof:false.

The host fixes the delay at --delay ticks. The client plays, leaves with its reconnect ticket and comes back on a
new process with the same identity. The return passes only when the host admits it at the fixed delay without asking
for more, the returner completes its private catch-up and both peers then play on together.
"""
import argparse
import json
from pathlib import Path
import re
import shutil
import sys
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--repo', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--delay', type=int, required=True, help='The host\'s fixed input delay in ticks.')
p.add_argument('--port', type=int, default=49474)
p.add_argument('--leave-tick', type=int, default=900)
p.add_argument('--auto', action='store_true', help='Control arm: the Auto policy with the same initial delay.')
a = p.parse_args()
repo = a.repo.resolve()
sys.path.insert(0, str(repo / 'tools'))
from run_sim_test import engine_executable, file_sha256, make_run, seed_settings

out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
(out / 'driver-as-run.py').write_bytes(Path(__file__).read_bytes())
ticket = out / 'client.ticket'
quit_path, leave_path = out / 'quit.json', out / 'leave.json'
menu = out / 'menu.txt'
menu.write_text(f'in_play\nwait_file {quit_path.as_posix()} 300\nexit\n')
common = ['-net-match-service-e2e', '-net-port', str(a.port), '-net-match-peers', '2',
          '-net-match-input-delay', str(a.delay), '-net-autosave-seconds', '0', '-net-match-ticks', '12000',
          '-max-ticks', '12000', '-net-match-service-preset', 'Determinism FeelBaseline',
          '-net-match-service-module', 'UserScenes.rte', '-menu-script', str(menu),
          '-input-script', str(repo / 'tools/e2e/play-input.txt')] + (['-net-match-auto-delay'] if a.auto else [])
policy = 'Auto' if a.auto else 'Fixed'
env = dict(CCCP_HEADLESS='1', CC_RUNNER_IGNORE_FULLSCREEN='1', CC_TEST_NET_MATCH_E2E_WAIT_PEERS='1', CC_TEST_MEMORY_CENSUS_TICKS='60')
settings = dict(AutosaveSeconds='0', NetworkHostDelayPolicy=policy, NetworkIceEnable='0', NetworkPortMapEnable='0',
                NetworkHostVisibility='Unlisted', EnableLuaDebugging='0', ResolutionX='960', ResolutionY='540', EnableVSync='0')
runs, records, active = {}, {}, []
result = dict(proof=False, topology='single-box', maximum_concurrent_engines=2, delay_policy=policy, input_delay_frames=a.delay,
              exe_sha256=file_sha256(engine_executable(repo)), checks={})


def prepare(role):
    steps = [{'op': 'wait_file', 'path': str(leave_path if role == 'client' else quit_path)}, {'op': 'finish', 'quit': True}]
    directory = out / f'{role}-probe'
    directory.mkdir()
    probe = directory / 'probe.json'
    probe.write_text(json.dumps({'schema': 1, 'timeout_ms': 300000, 'steps': steps}, indent=2))
    flags = common + (['-net-host'] if role == 'host' else ['-net-join', '127.0.0.1', '-net-reconnect-ticket', str(ticket)])
    flags += ['-net-match-report', str(out / f'{role}-report.json')]
    run = make_run(repo, flags, out / role, 360, env={**env, 'CC_TEST_NET_UI_SCRIPT': str(probe)})
    seed_settings(run, settings)
    fixture = Path(run.cwd) / 'Userdata/UserScenes.rte'
    fixture.mkdir()
    text, count = re.subn(r'^local WINDOW_TICKS = \d+;$', 'local WINDOW_TICKS = 12000;',
                          (repo / 'tools/feel/FeelBaseline.lua').read_text(), flags=re.M)
    assert count == 1
    (fixture / 'FeelBaseline.lua').write_text(text)
    (fixture / 'Index.ini').write_text('DataModule\n\tModuleName = User Scenes\n\tScanFolderContents = 1\n\tIgnoreMissingItems = 1\n'
        '\tAddActivity = GAScripted\n\t\tCopyOf = P4 Alpha Duel\n\t\tPresetName = Determinism FeelBaseline\n'
        '\t\tScriptPath = UserScenes.rte/FeelBaseline.lua\n\t\tLuaClassName = FeelBaseline\n'
        '\t\tTeamOfPlayer1 = 0\n\t\tPlayer1IsHuman = 1\n\t\tTeamOfPlayer2 = 1\n\t\tPlayer2IsHuman = 1\n')
    runs[role] = run
    return run


def log(role):
    path = out / role / 'stdout.log'
    return path.read_text(errors='replace') if path.exists() else ''


def tick(role):
    rows = re.findall(r'^\[mem-census\].*?\btick=(\d+)', log(role), re.M)
    return int(rows[-1]) if rows else 0


def wait_for(check, description, seconds=180):
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        if check():
            return
        for role in active:
            if runs[role].poll() is not None:
                raise RuntimeError(f'{role} ended before {description}')
        time.sleep(.2)
    raise RuntimeError('timeout waiting for ' + description)


failure = None
try:
    for role in ('host', 'client'):
        prepare(role).start()
        active.append(role)
    wait_for(lambda: all(tick(role) >= a.leave_tick for role in active), 'both peers playing')
    leave_path.write_text('{}')
    records['client'] = runs['client'].finish()
    active.remove('client')
    if records['client'].get('exit_code') != 0 or records['client'].get('timed_out'):
        raise RuntimeError('the departing client did not exit normally')
    if not ticket.exists():
        raise RuntimeError('the departing client left no ticket')
    wait_for(lambda: '[net-peer-departure] peer=2 ' in log('host'), 'the host noticing the departure')
    returned = prepare('return')
    old_key = Path(runs['client'].cwd) / 'Userdata/NetworkIdentity.key'
    if old_key.exists():
        shutil.copy2(old_key, Path(returned.cwd) / 'Userdata/NetworkIdentity.key')
    returned.start()
    active.append('return')
    wait_for(lambda: '[net-match] private catch-up complete frame=' in log('return'), 'the returner completing its catch-up', 240)
    at = {role: tick(role) for role in active}
    wait_for(lambda: all(tick(role) >= at[role] + 300 for role in active), '300 live ticks on both peers after the return')
except Exception as error:
    failure = repr(error)
finally:
    quit_path.write_text('{}')
    for role in active:
        records[role] = runs[role].finish()
    for run in runs.values():
        run.close()
    (out / 'run-result.json').write_text(json.dumps(records, indent=2))

logs = {role: log(role) for role in runs}
refusals = [line for text in logs.values() for line in text.splitlines() if 'Your connection needs' in line]
result['failure'] = failure
result['checks'] = {
    'all_normal_exits': set(records) == {'host', 'client', 'return'} and all(r.get('exit_code') == 0 and not r.get('timed_out') for r in records.values()),
    'no_delay_refusal': not refusals,
    'return_complete': '[net-match] private catch-up complete frame=' in logs.get('return', ''),
    'no_delay_change': a.auto or not any(re.search(r'\[net-lockstep\] delay proposal ', text) for text in logs.values()),
}
result['refusals'] = refusals[:5]
result['lines'] = {role: [line for line in text.splitlines() if re.match(r'^\[(net-match|net-peer-departure)\]', line)
                          and any(word in line for word in ('held seat', 'rejoin', 'return', 'catch-up', 'departure', 'refused'))][:40]
                   for role, text in logs.items()}
result['pass'] = not failure and all(result['checks'].values())
(out / 'summary.json').write_text(json.dumps(result, indent=2))
print(json.dumps({key: result[key] for key in ('pass', 'failure', 'checks', 'refusals')}))
raise SystemExit(0 if result['pass'] else 1)
