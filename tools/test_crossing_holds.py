"""Load both machines of a two-player match at once, at a fixed one-tick input delay; two local peers, proof:false.

With --host-dies-when-client-held the host is unloaded and the client loaded until the host holds it; the host's engine is then
dropped, and the run passes only when the held client, its history now coming from nobody, takes its seat back and plays on alone.

Each peer's simulation is slowed over its own window of ticks (CCCP_TEST_SIM_COST_US), the windows overlapping, so each machine
can find the other slow while it is slow itself and both seats can be held at once. The run passes only when the round never
loses its last playing seat: both peers play on past the end tick, at least one seat was held and returned, the live hash
compares agree - after the losing half rejoined through the image, when the two machines held each other and split - and
nobody waits on a round with no playing member.
"""
import argparse
import json
from pathlib import Path
import re
import sys
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--repo', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--port', type=int, default=49476)
p.add_argument('--delay', type=int, default=1, help='The host\'s fixed input delay in ticks.')
p.add_argument('--host-load', default='60000:1500:1620', help='COST_US:FROM_TICK:UNTIL_TICK on the host.')
p.add_argument('--client-load', default='60000:1530:1650', help='COST_US:FROM_TICK:UNTIL_TICK on the client.')
p.add_argument('--end-tick', type=int, default=2700)
p.add_argument('--freeze-seconds', type=int, default=90, help='How long the round may stand still before the run calls it frozen.')
p.add_argument('--host-dies-when-client-held', action='store_true', help='Drop the host engine once the client holds its own seat.')
a = p.parse_args()
repo = a.repo.resolve()
sys.path.insert(0, str(repo / 'tools'))
from run_sim_test import engine_executable, file_sha256, make_run, seed_settings

out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
(out / 'driver-as-run.py').write_bytes(Path(__file__).read_bytes())
quit_path = out / 'quit.json'
ticket = out / 'client.ticket'
menu = out / 'menu.txt'
menu.write_text(f'in_play\nwait_file {quit_path.as_posix()} 600\nexit\n')
common = ['-net-match-service-e2e', '-net-port', str(a.port), '-net-match-peers', '2',
          '-net-match-input-delay', str(a.delay), '-net-autosave-seconds', '0', '-net-match-ticks', '12000',
          '-max-ticks', '12000', '-net-match-service-preset', 'Determinism FeelBaseline',
          '-net-match-service-module', 'UserScenes.rte', '-menu-script', str(menu),
          '-input-script', str(repo / 'tools/e2e/play-input.txt')]
env = dict(CCCP_HEADLESS='1', CC_RUNNER_IGNORE_FULLSCREEN='1', CC_TEST_NET_MATCH_E2E_WAIT_PEERS='1', CC_TEST_MEMORY_CENSUS_TICKS='60')
settings = dict(AutosaveSeconds='0', NetworkHostDelayPolicy='Fixed', NetworkIceEnable='0', NetworkPortMapEnable='0',
                NetworkHostVisibility='Unlisted', EnableLuaDebugging='0', ResolutionX='960', ResolutionY='540', EnableVSync='0')
runs, records = {}, {}
result = dict(proof=False, topology='single-box', maximum_concurrent_engines=2, delay_policy='Fixed', input_delay_frames=a.delay,
              host_load=a.host_load, client_load=a.client_load, exe_sha256=file_sha256(engine_executable(repo)), checks={})


def load_env(spec):
    if spec == 'none':
        return {}
    cost, start, until = (int(part) for part in spec.split(':'))
    return dict(CCCP_TEST_SIM_COST_US=str(cost), CCCP_TEST_SIM_COST_FROM_TICK=str(start), CCCP_TEST_SIM_COST_UNTIL_TICK=str(until))


def prepare(role):
    steps = [{'op': 'wait_file', 'path': str(quit_path)}, {'op': 'finish', 'quit': True}]
    directory = out / f'{role}-probe'
    directory.mkdir()
    probe = directory / 'probe.json'
    probe.write_text(json.dumps({'schema': 1, 'timeout_ms': 600000, 'steps': steps}, indent=2))
    flags = common + (['-net-host'] if role == 'host' else ['-net-join', '127.0.0.1', '-net-reconnect-ticket', str(ticket)])
    flags += ['-net-match-report', str(out / f'{role}-report.json')]
    load = load_env(a.host_load if role == 'host' else a.client_load)
    run = make_run(repo, flags, out / role, 660, env={**env, **load, 'CC_TEST_NET_UI_SCRIPT': str(probe)})
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


failure = None
dropped_at = None
live = ['host', 'client']
try:
    for role in live:
        prepare(role).start()
    if a.host_dies_when_client_held:
        # The client is held once it installs its own hold: from then on its history comes only from the host, which is dropped.
        until = time.monotonic() + 300
        while not re.search(r'^\[net-peer-hold-installed\] local=2 peer=2 ', log('client'), re.M):
            if time.monotonic() > until or any(run.poll() is not None for run in runs.values()):
                raise RuntimeError('the client was never held')
            time.sleep(.2)
        dropped_at = tick('client')
        runs['host'].terminate(reason='the host engine dropped while it held the client')
        live.remove('host')
    # A round that stands still for the freeze window is frozen; one that plays on reaches the end tick on both peers.
    last, since = None, time.monotonic()
    until = time.monotonic() + 540
    while time.monotonic() < until:
        ticks = tuple(tick(role) for role in live)
        if min(ticks) >= a.end_tick:
            break
        for role in live:
            if runs[role].poll() is not None:
                raise RuntimeError(f'{role} ended at tick {tick(role)}')
        if ticks != last:
            last, since = ticks, time.monotonic()
        elif time.monotonic() - since > a.freeze_seconds:
            raise RuntimeError(f'the round stood still for {a.freeze_seconds} s at ticks {ticks}')
        time.sleep(.5)
    else:
        raise RuntimeError(f'the peers did not reach tick {a.end_tick}: {last}')
    # A losing half rejoins through the image: the match ends only once that rejoin is complete and both peers play on together.
    if any('[net-split-arbitration]' in log(role) for role in live):
        until = time.monotonic() + 240
        while '[net-match] private catch-up complete frame=' not in log('client'):
            if time.monotonic() > until or any(runs[role].poll() is not None for role in live):
                raise RuntimeError('the losing half did not complete its image rejoin')
            time.sleep(.5)
        at = {role: tick(role) for role in live}
        until = time.monotonic() + 120
        while not all(tick(role) >= at[role] + 300 for role in live):
            if time.monotonic() > until or any(runs[role].poll() is not None for role in live):
                raise RuntimeError('the peers did not play 300 ticks together after the rejoin')
            time.sleep(.5)
except Exception as error:
    failure = repr(error)
finally:
    quit_path.write_text('{}')
    for role in runs:
        records[role] = runs[role].finish()
    for run in runs.values():
        run.close()
    (out / 'run-result.json').write_text(json.dumps(records, indent=2))

logs = {role: log(role) for role in runs}
reports = {}
for role in runs:
    path = out / f'{role}-report.json'
    reports[role] = json.loads(path.read_text()) if path.exists() else {}
holds = {role: re.findall(r'^\[net-peer-hold-installed\] local=\d+ peer=(\d+) frame=(\d+)', text, re.M) for role, text in logs.items()}
returns = {role: re.findall(r'^\[net-frame-group\] return frame=(\d+)', text, re.M) for role, text in logs.items()}
empty = {role: [line for line in text.splitlines() if re.search(r'\bmembers=0\b', line)][:3] for role, text in logs.items()}
checks = {role: reports[role].get('desync_check', {}) for role in runs}
# Two machines that held each other played two continuations until the ruling: their compares differ until the losing half has
# rejoined through the image, and never after.
desyncs = {role: [int(frame) for frame in re.findall(r'^\[lockstep\] desync at frame (\d+)', text, re.M)] for role, text in logs.items()}
splits = {role: re.findall(r'^\[net-split-arbitration\] local=\d+ split=(\d+) frame=(\d+) winners=(\d+)', text, re.M) for role, text in logs.items()}
rejoined = [int(frame) for text in logs.values() for frame in re.findall(r'^\[net-match\] private catch-up complete frame=(\d+)', text, re.M)]
rejoin_frame = max(rejoined) if rejoined else None
result['failure'] = failure
result['host_dropped_at_client_tick'] = dropped_at
if a.host_dies_when_client_held:
    client = logs.get('client', '')
    result['checks'] = {
        'host_dropped_while_client_held': dropped_at is not None and bool(records.get('host', {}).get('injected_termination')),
        'client_exited_normally': records.get('client', {}).get('exit_code') == 0 and not records.get('client', {}).get('timed_out'),
        'client_played_past_the_end_tick': not failure,
        'client_took_its_seat_back_alone': bool(re.search(r'^\[net-peer-recovery\] peer=2 .* lone=1 ', client, re.M)),
        'never_a_round_without_a_player': not any(empty.values()),
        'client_hashes_agreed_before_the_drop': checks.get('client', {}).get('compares', 0) > 0 and not desyncs.get('client'),
    }
else:
    result['checks'] = {
        'all_normal_exits': set(records) == {'host', 'client'} and all(r.get('exit_code') == 0 and not r.get('timed_out') for r in records.values()),
        'played_past_the_end_tick': not failure,
        'a_seat_was_held': any(holds.values()),
        'every_held_seat_returned': all(len(returns[role]) >= 1 for role in runs) if any(holds.values()) else False,
        'never_a_round_without_a_player': not any(empty.values()),
        'hashes_equal': all(check.get('compares', 0) > 0 for check in checks.values()) and
                        (not any(desyncs.values()) or (any(splits.values()) and rejoin_frame is not None and
                         all(frame < rejoin_frame for frames in desyncs.values() for frame in frames))),
    }
result['holds'] = holds
result['returns'] = {role: values[:10] for role, values in returns.items()}
result['empty_round_lines'] = empty
result['desync_check'] = checks
result['desyncs'] = desyncs
result['splits'] = splits
result['rejoin_frame'] = rejoin_frame
result['ticks'] = {role: tick(role) for role in runs}
result['pass'] = all(result['checks'].values())
(out / 'summary.json').write_text(json.dumps(result, indent=2))
print(json.dumps({key: result[key] for key in ('pass', 'failure', 'checks', 'ticks')}))
raise SystemExit(0 if result['pass'] else 1)
