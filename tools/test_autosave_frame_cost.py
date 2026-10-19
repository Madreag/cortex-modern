"""A save that is not this machine's own overdue one costs its player no frames (one machine, two engines, proof:false).

A hosted two-peer match at the host's default automatic input delay saves every --autosave-seconds. Every per-second
[frame-pace] window of each peer is placed by its ticks: no save running (idle), inside one of this peer's saves whose own
deadline had not yet passed (save), inside the part of a save that ran past the next deadline (overdue: the late machine
may pay for its own save there), or, on the host, from a returning seat's base being asked to its being taken (base).
--client-rejoin TICK drops the client once its game reaches TICK and starts it again with its reconnect ticket, so it
returns through the host's image and the host asks for its base; a tick shortly after a capture meets the host's writer
still busy with that save.

For each peer and kind the result gives the median frame rate, the 95th percentile of the per-window worst frame interval,
the median tick rate and the seat holds whose frame falls there. The verdict: each peer's ordinary saves (not in a rejoin
run, whose client is replaced) and the host's frames while a base waits keep at least 95 % of the idle frame rate and at
most 125 % of the idle worst frame interval, and the host keeps its idle tick rate within 0.5 ticks/s while a base waits.
The same numbers on a base build are the reference a tip is compared with.
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


def fields(line):
    return {key: value for key, value in re.findall(r'(\w+)=(-?[0-9.]+)\b', line)}


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(fraction * len(ordered)))]


def timeline(text, host):
    """The peer's frame windows, saves, deadlines, bases and holds, in its log's order."""
    windows, saves, dues, bases, holds = [], [], [], [], []
    latest = 0
    asked = None
    for line in text.splitlines():
        if line.startswith('[frame-pace] '):
            f = fields(line)
            if 'first_tick' in f and 'tick' in f and float(f.get('wall_ms', 0)) > 0:
                window = {'first': int(f['first_tick']), 'last': int(f['tick']), 'fps': float(f['fps']),
                          'worst_ms': float(f['interval_max_ms']), 'tps': int(f.get('ticks', 0)) * 1000.0 / float(f['wall_ms'])}
                windows.append(window)
                latest = window['last']
        elif match := re.match(r'^\[autosave\] tick=(\d+) capture_ms=', line):
            # Printed when the save is written: its window runs from its boundary to the tick the game had reached.
            saves.append((int(match.group(1)), max(latest, int(match.group(1)))))
        elif host and (match := re.match(r'^\[autosave\] periodic due named at=(\d+) late_ticks=(-?\d+)', line)):
            dues.append(int(match.group(1)) - int(match.group(2)))
        elif host and (match := re.search(r'private base asked of the checkpoint schedule at tick (\d+)', line)):
            asked = int(match.group(1))
        elif host and asked is not None and (match := re.search(r'private base taken at the announced tick (\d+)', line)):
            bases.append((asked, int(match.group(1))))
            asked = None
        elif match := re.match(r'^\[net-peer-hold-installed\] local=(\d+) peer=(\d+) frame=(\d+)', line):
            holds.append({'local': int(match.group(1)), 'peer': int(match.group(2)), 'frame': int(match.group(3))})
    if asked is not None:
        bases.append((asked, latest))
    return windows, saves, dues, bases, holds


def classify(windows, saves, dues, bases, warmup):
    """Places each window: base, overdue, save, idle, or edge (half in a save) and warmup, which are not judged."""
    overdue = []
    for start, end in saves:
        later = [due for due in dues if due > start]
        if later and end > later[0]:
            overdue.append((later[0], end))
    kinds = []
    for w in windows:
        def overlaps(span):
            return w['first'] <= span[1] and w['last'] >= span[0]
        if w['first'] < warmup:
            kind = 'warmup'
        elif any(overlaps(span) for span in bases):
            kind = 'base'
        elif any(overlaps(span) for span in overdue):
            kind = 'overdue'
        elif any(w['first'] >= start and w['last'] <= end for start, end in saves):
            kind = 'save'
        elif any(overlaps(span) for span in saves):
            kind = 'edge'
        else:
            kind = 'idle'
        kinds.append(kind)
    return kinds, overdue


def summary(windows, kinds, holds, local):
    out = {}
    for kind in ('idle', 'save', 'overdue', 'base', 'edge', 'warmup'):
        chosen = [w for w, k in zip(windows, kinds) if k == kind]
        in_kind = [h for h in holds if h['local'] == local and any(w['first'] <= h['frame'] <= w['last'] for w in chosen)]
        out[kind] = {'windows': len(chosen),
                     'fps_median': round(statistics.median([w['fps'] for w in chosen]), 2) if chosen else None,
                     'worst_ms_p95': round(percentile([w['worst_ms'] for w in chosen], 0.95), 2) if chosen else None,
                     'tps_median': round(statistics.median([w['tps'] for w in chosen]), 2) if chosen else None,
                     'holds': len(in_kind), 'own_seat_holds': sum(1 for h in in_kind if h['peer'] == local)}
    return out


def keeps(kind, idle, minimum):
    """The kind's frames against the idle ones: at least 95 % of the frame rate, at most 125 % of the worst frame interval."""
    if kind['windows'] < minimum or idle['windows'] < 10:
        return {'enough_windows': False}
    return {'enough_windows': True, 'keeps_frame_rate': kind['fps_median'] >= 0.95 * idle['fps_median'],
            'keeps_worst_frame': kind['worst_ms_p95'] <= 1.25 * idle['worst_ms_p95']}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repo', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--autosave-seconds', type=int, default=90)
    p.add_argument('--periodic-captures', type=int, default=5)
    p.add_argument('--client-rejoin', type=int, default=0, help='the client drops once its game reaches this tick and rejoins')
    p.add_argument('--warmup-ticks', type=int, default=1200)
    p.add_argument('--limit-seconds', type=int, default=1500)
    p.add_argument('--port', type=int, default=49492)
    a = p.parse_args()
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
    common = ['-net-port', str(a.port), '-net-match-peers', '2', '-net-match-auto-delay',
              '-net-autosave-seconds', str(a.autosave_seconds), '-net-match-ticks', ticks, '-max-ticks', ticks,
              '-menu-script', str(menu), '-input-script', str(repo / 'tools/e2e/play-input.txt'),
              '-net-match-service-preset', 'Determinism FeelBaseline', '-net-match-service-module', 'UserScenes.rte']
    argv = {'host': ['-net-match-service-e2e', '-net-host', '-net-match-report', str(out / 'host-report.json'),
                     '-net-reconnect-ticket', str(out / 'host.ticket')] + common,
            'client': ['-net-match-service-e2e', '-net-join', '127.0.0.1', '-net-match-report', str(out / 'client-report.json'),
                       '-net-reconnect-ticket', str(out / 'client.ticket')] + common}
    env = {'CCCP_HEADLESS': '1', 'CC_RUNNER_IGNORE_FULLSCREEN': '1', 'CC_TEST_NET_MATCH_E2E_WAIT_PEERS': '1'}
    settings = {'AutosaveSeconds': '0', 'NetworkHostDelayPolicy': 'Auto', 'NetworkInputDelayFrames': '0', 'NetworkIceEnable': '0',
                'NetworkPortMapEnable': '0', 'NetworkHostVisibility': 'Unlisted', 'EnableLuaDebugging': '0',
                'NetworkAutoReconnect': '1', 'NetworkOfferStoredRejoin': '1',
                'ResolutionX': '960', 'ResolutionY': '540', 'EnableVSync': '0'}
    def probe_env(name):
        # Each engine leaves when the driver has seen its captures; every engine keeps its probe's results apart.
        probe_dir = out / f'{name}-probe'
        probe_dir.mkdir()
        (probe_dir / 'probe.json').write_text(json.dumps({'schema': 1, 'timeout_ms': (a.limit_seconds + 300) * 1000,
                                                          'steps': [{'op': 'wait_file', 'path': str(quit_path)}, {'op': 'finish', 'quit': True}]}, indent=2))
        return {**env, 'CC_TEST_NET_UI_SCRIPT': str(probe_dir / 'probe.json')}

    runs, records, started = {}, {}, []
    for role in ('host', 'client'):
        runs[role] = make_run(repo, argv[role], out / role, a.limit_seconds + 600, env=probe_env(role))
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
    config = {'topology': 'single-box', 'proof': False, 'box': platform.node(), 'autosave_seconds': a.autosave_seconds,
              'periodic_captures': a.periodic_captures, 'client_rejoin': a.client_rejoin, 'input_delay': 'auto', 'settings': settings,
              'environment': env, 'source_head': subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip(),
              'exe_sha256': file_sha256(engine_executable(repo)), 'started_utc': datetime.now(timezone.utc).isoformat()}
    (out / 'configuration.json').write_text(json.dumps(config, indent=2))

    def log(role):
        path = out / role / 'stdout.log'
        return path.read_text(errors='replace') if path.exists() else ''

    def named():
        return [int(t) for t in re.findall(r'^\[autosave\] named tick=(\d+) at=\d+ writers=\d+', log('host'), re.M)]

    def written(role):
        return len(re.findall(r'^\[autosave\] tick=\d+ capture_ms=', log(role), re.M))

    def reached(role):
        ticks_seen = re.findall(r'^\[frame-pace\] .*? tick=(\d+) ', log(role), re.M)
        return int(ticks_seen[-1]) if ticks_seen else 0

    failure = None
    rejoined_at = None
    began = time.monotonic()
    try:
        for role in ('host', 'client'):
            runs[role].start()
            started.append(role)
        until = time.monotonic() + a.limit_seconds
        while True:
            if a.client_rejoin and rejoined_at is None and reached('client') >= a.client_rejoin:
                runs['client'].terminate(reason=f'the client drops at tick {a.client_rejoin}')
                records['client-dropped'] = runs['client'].finish()
                runs['client-dropped'] = runs.pop('client')
                rejoined_at = reached('host')
                runs['client'] = make_run(repo, argv['client'], out / 'client-rejoined', a.limit_seconds + 600,
                                          env=probe_env('client-rejoined'), runtime=Path(runs['client-dropped'].cwd))
                runs['client'].start()
            rejoin_pending = bool(a.client_rejoin) and (rejoined_at is None or not re.search('private base taken', log('host')))
            if len(named()) >= a.periodic_captures and written('host') >= len(named()) and not rejoin_pending:
                break
            if time.monotonic() > until:
                raise RuntimeError(f'{len(named())} captures named, {written("host")} written by the host in {a.limit_seconds} s')
            if any(runs[role].poll() is not None for role in ('host', 'client')):
                raise RuntimeError('an engine ended before the captures')
            time.sleep(1)
    except Exception as error:
        failure = repr(error)
    finally:
        quit_path.write_text(json.dumps({'requested_utc': datetime.now(timezone.utc).isoformat()}))
        for role in ('host', 'client'):
            if role in runs and role not in records:
                records[role] = runs[role].finish()
        for run in runs.values():
            run.close()
    peers, judged = {}, {}
    host_dues = timeline(log('host'), True)[2]
    for local, role in ((1, 'host'), (2, 'client')):
        windows, saves, dues, bases, holds = timeline(log(role), role == 'host')
        # The client's own deadlines follow the same agreed schedule as the host's.
        kinds, overdue = classify(windows, saves, host_dues, bases, a.warmup_ticks)
        peers[role] = {'kinds': summary(windows, kinds, holds, local), 'saves': saves, 'overdue_spans': overdue, 'bases': bases}
        idle, save, base = (peers[role]['kinds'][k] for k in ('idle', 'save', 'base'))
        if not a.client_rejoin:
            judged[role] = {f'save_{name}': value for name, value in keeps(save, idle, 5).items()}
        elif role == 'host':
            busy = any(any(start <= asked <= end for start, end in saves) for asked, _ in bases)
            judged[role] = {'base_asked_while_writer_busy': busy, **{f'base_{name}': value for name, value in keeps(base, idle, 3).items()}}
            if base['windows'] >= 3:
                judged[role]['base_keeps_tick_rate'] = base['tps_median'] >= idle['tps_median'] - 0.5
    refused = {role: re.findall(r'^\[autosave\] (?:named tick=\d+ not taken: .*|failed .*)$', log(role), re.M) for role in ('host', 'client')}
    checks = {'no_driver_failure': failure is None,
              'all_exit_zero': all(r.get('exit_code') == 0 and not r.get('timed_out') for role, r in records.items() if role != 'client-dropped'),
              'periodic_captures_taken': len(named()) >= a.periodic_captures,
              'no_capture_refused': not refused['host'] and (bool(a.client_rejoin) or not refused['client']),
              **{f'{role}_{name}': value for role, items in judged.items() for name, value in items.items()}}
    result = {**config, 'pass': all(checks.values()), 'checks': checks, 'failure': failure, 'wall_seconds': round(time.monotonic() - began, 1),
              'captures': named(), 'rejoined_at_host_tick': rejoined_at, 'peers': peers, 'refused': refused,
              'exits': {role: r.get('exit_code') for role, r in records.items()}}
    (out / 'result.json').write_text(json.dumps(result, indent=2))
    print(json.dumps({'pass': result['pass'], 'checks': checks, 'failure': failure,
                      'kinds': {role: peers[role]['kinds'] for role in peers}}))
    return 0 if result['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
