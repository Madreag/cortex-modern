"""Cut the whole network path of one peer mid-match between two local engines and judge both survivors.

The joiner's process keeps running while every packet it sends or receives is lost (CC_TEST_GNS_LINK_CUT), long enough for
the transport to declare the connection dropped. The host must hold the joiner's seat within the slow-player bound and play
on at the round's rate; the joiner must hold the host and play on, and the commands nobody can acknowledge while it plays
alone must stay bounded.

--path directory (the default) joins through a local session directory with ICE, as an internet match does: the directory
referees splits, so a one-against-one hold is the case the authority ruling decides locally. --path direct joins by
address on loopback. Topology: two engines on one box (proof: false); no relay server is in the loop.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time

from run_sim_test import make_run, engine_executable, seed_settings

BOUND_MS = 50
# Every consumer wait that reached the bound is named by a frame-wait line; a hold at the bound ends it within a tick of it.
WAIT_LIMIT_MS = BOUND_MS + 20


def census_series(text):
    return [(int(match[1]), int(match[2])) for match in re.finditer(r'tick=(\d+)[^\n]*?command_outbox=(\d+)', text)]


def live_rows(path):
    parsed = [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []
    return [row for row in parsed if 'tick' in row and 'unix_ms' in row]


def direct_runs(repo, out, timeout, ticks, port, cut, lag_ms, host_blackout=None, host_stall=None, cut_peer=2):
    common = ['-net-match-service-e2e', '-net-port', str(port), '-net-match-peers', '2',
              '-net-match-mode', 'coop-pve', '-net-match-ticks', str(ticks), '-max-ticks', str(ticks),
              '-net-match-input-delay', '6', '-net-match-auto-delay', '-net-match-service-preset', 'Skirmish Defense',
              '-net-match-service-scene', 'Grasslands', '-net-match-e2e-brain-placement',
              '-seed', '42', '-num-lua-states', '4', '-memory-census-ticks', '300']
    runs = []
    for peer in (1, 2):
        folder = out / f'peer{peer}'
        flags = ['-net-host'] if peer == 1 else ['-net-join', '127.0.0.1']
        if peer == 2 and lag_ms:
            flags += ['-net-fake-lag', str(lag_ms)]
        if peer == 1 and host_stall:
            flags += ['-net-test-live-stall', host_stall]
        env = {'CC_TEST_NET_MATCH_E2E_WAIT_PEERS': '1', 'CC_TEST_RETURN_INPUT_DETAILS': '1'}
        if peer == cut_peer:
            env['CC_TEST_GNS_LINK_CUT'] = cut
        if peer == 1 and host_blackout:
            env['CC_TEST_LINK_BLACKOUT'] = host_blackout
        run = make_run(repo, common + flags + ['-net-match-report', str(folder / 'report.json'), '-net-live-tick-hashes', str(folder / 'live.jsonl')],
                       folder, timeout, env=env, expected=[folder / 'report.json', folder / 'live.jsonl'])
        runs.append(run)
        run.start()
    return runs


def directory_runs(repo, out, timeout, ticks, port, cut, cut_ms, stack):
    from test_directory_cadence import local_directory, stage_activity
    from test_lobby_lifecycle import wait_for_log
    server, _requests, pin = stack.enter_context(local_directory(out / 'directory', 'link-cut', port + 1))
    common = 'in_play\nwait_ms 1000\nactivate ButtonMainToMultiplayer\nwait_ms 300\n'
    menus = {
        'host': common + 'settext TextMultiplayerName Host\nactivate ButtonMultiplayerHostGame\nwait_ms 300\n'
                'combo_select ComboHostActivity Directory Cadence\nwait_ms 300\n'
                f'setup_host_port {port}\ncombo_select ComboHostPlayers 2\nactivate ButtonMultiplayerCreate\n'
                'wait_connected 2 80\nwait_remote_ready 80\nactivate ButtonMultiplayerStart\n'
                f'wait_lockstep 180 60\nwait_lockstep {ticks} {max(60, timeout - 60)}\n'
                'open_local_pause\nactivate ButtonEndMatch\nactivate ButtonLeaveConfirm\nwait_round_end 10\nwait_ms 1000\nexit\n',
        'guest': common + 'settext TextMultiplayerName Guest\nactivate ButtonMultiplayerJoinGame\nwait_ms 300\n'
                 'activate ButtonJoinByAddress\nwait 4\nsettext TextJoinAddress SESSION\n'
                 f'settext TextJoinPort {port}\nactivate ButtonJoinAddressGo\nwait_connected 2 80\n'
                 f'activate ButtonMultiplayerReady\nwait_lockstep 300 60\nwait_ms {cut_ms + 15000}\nexit\n',
    }
    runs = []
    for role, folder in (('host', out / 'peer1'), ('guest', out / 'peer2')):
        script = out / f'{role}.menu.txt'
        if role == 'guest':
            deadline = time.monotonic() + 20
            while not server.store._sessions and time.monotonic() < deadline:
                time.sleep(.05)
            with server.store._lock:
                session = next(iter(server.store._sessions))
            menus[role] = menus[role].replace('SESSION', 'session:' + session)
        script.write_text(menus[role])
        env = {'CC_TEST_RETURN_INPUT_DETAILS': '1'}
        if role == 'guest':
            env['CC_TEST_GNS_LINK_CUT'] = cut
        run = make_run(repo, ['-module', 'DirectoryCadence.rte', '-menu-script', script, '-num-lua-states', '4', '-net-match-ticks', '0',
                              '-net-match-report', folder / 'report.json', '-net-live-tick-hashes', folder / 'live.jsonl',
                              '-memory-census-ticks', '300'], folder, timeout=timeout, env=env)
        seed_settings(run, {'SessionDirectoryUrl': f'127.0.0.1:{port + 1}', 'SessionDirectoryCertSha256': pin, 'NetworkHostVisibility': 1,
                            'NetworkIceEnable': 1, 'NetworkConnectionMode': 1, 'NetworkHostRelayMode': 0, 'NetworkStunServers': ''})
        stage_activity(repo, run)
        run.start()
        runs.append(run)
        if role == 'host':
            wait_for_log(run, 'activate ButtonMultiplayerCreate click ButtonMultiplayerCreate PASS', 45)
    return runs


def run_case(repo, out, args):
    out.mkdir(parents=True, exist_ok=False)
    cut = args.cut_spec or f'{args.cut_frame}:{args.cut_ms}'
    with ExitStack() as stack:
        runs = []
        try:
            if args.path == 'direct':
                runs = direct_runs(repo, out, args.timeout, args.ticks, args.port, cut, args.lag_ms, args.host_blackout, args.host_stall, 1 if args.cut_peer == 'host' else 2)
            else:
                runs = directory_runs(repo, out, args.timeout, args.ticks, args.port, cut, args.cut_ms, stack)
            with ThreadPoolExecutor(max_workers=2) as pool:
                records = list(pool.map(lambda run: run.finish(), runs))
        finally:
            for run in runs:
                run.close()
    errors = []
    logs = {peer: (out / f'peer{peer}/stdout.log').read_text(errors='replace') if (out / f'peer{peer}/stdout.log').exists() else ''
            for peer in (1, 2)}
    rows = {peer: live_rows(out / f'peer{peer}/live.jsonl') for peer in (1, 2)}
    cut_log = logs[1 if args.cut_peer == 'host' else 2]
    begin = re.search(r'\[test-link-cut\] begin frame=(\d+) duration_ms=\d+ .*?unix_ms=(\d+)', cut_log)
    cut_at = int(begin[2]) if begin else None
    cut_frame = int(begin[1]) if begin else None
    if cut_at is None:
        errors.append('the cut peer never cut its link')
    dropped = re.search(r'\[net-transport\] closed .*Connection dropped', logs[1]) is not None
    # S-1: the host holds the joiner within the bound and keeps the round's rate through the cut.
    host_rows = rows[1]
    host_last = host_rows[-1]['tick'] if host_rows else None
    held = [row for row in host_rows if cut_at is not None and row['unix_ms'] >= cut_at and 2 in row['ai_held_peer_ids']]
    window = [row for row in host_rows if cut_at is not None and cut_at <= row['unix_ms'] <= cut_at + args.cut_ms]
    host_tps = None
    if len(window) >= 2:
        host_tps = (window[-1]['tick'] - window[0]['tick']) * 1000.0 / max(1, window[-1]['unix_ms'] - window[0]['unix_ms'])
    host_waits = [int(match[2]) for match in re.finditer(r'\[net-frame-wait\] frame=(\d+) wait(?:ing)?_ms=(\d+)', logs[1])
                  if cut_frame is not None and int(match[1]) >= cut_frame - 30]
    longest_wait = max(host_waits) if host_waits else 0
    if not held:
        errors.append('the host never held the joiner after its link was cut')
    if host_last is None or host_last < args.ticks - 1:
        errors.append(f'the host stopped at tick {host_last} of {args.ticks}')
    if host_tps is None or host_tps < 59.5:
        errors.append(f'the host ran {host_tps} ticks a second through the cut')
    if longest_wait > WAIT_LIMIT_MS:
        errors.append(f'the host waited {longest_wait} ms on the cut joiner')
    # S-2: the joiner holds the host and its unacknowledged commands stay bounded while nobody can acknowledge them.
    joiner_rows = rows[2]
    joiner_held = [row for row in joiner_rows if cut_at is not None and row['unix_ms'] >= cut_at and 1 in row['ai_held_peer_ids']]
    series = census_series(logs[2])
    after = [count for tick, count in series if cut_frame is not None and tick > cut_frame]
    peak = max(after) if after else None
    if not joiner_held:
        errors.append('the joiner never held the host after its link was cut')
    if args.outbox_limit is not None and (peak is None or peak > args.outbox_limit):
        errors.append(f'the cut joiner held {peak} unacknowledged commands against a bound of {args.outbox_limit}')
    for peer, record in enumerate(records, 1):
        if record.get('timed_out'):
            errors.append(f'peer {peer} did not finish before its timeout')
    result = dict(path=args.path, cut=cut, host_blackout=args.host_blackout, host_stall=args.host_stall, cut_peer=args.cut_peer, ticks=args.ticks, cut_at_unix_ms=cut_at, cut_frame=cut_frame, transport_dropped=dropped,
                  host=dict(last_tick=host_last, held_rows=len(held), first_held_tick=held[0]['tick'] if held else None,
                            tps_through_cut=host_tps, longest_wait_ms=longest_wait, exit_code=records[0].get('exit_code')),
                  joiner=dict(last_tick=joiner_rows[-1]['tick'] if joiner_rows else None, held_rows=len(joiner_held),
                              command_outbox=series, outbox_peak_after_cut=peak, exit_code=records[1].get('exit_code')),
                  errors=errors,
                  source_tip=subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip(),
                  binary_sha256=hashlib.sha256(engine_executable(repo).read_bytes()).hexdigest(),
                  topology='two engines on one box', proof=False)
    result['pass'] = not errors
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({key: value for key, value in result.items() if key != 'joiner'} | {'outbox_peak_after_cut': peak}), flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--path', choices=['directory', 'direct'], default='directory')
    parser.add_argument('--timeout', type=int, default=240)
    parser.add_argument('--cut-frame', type=int, default=600)
    parser.add_argument('--cut-ms', type=int, default=30000)
    parser.add_argument('--cut-spec', default=None,
                        help="the lever's whole value, several cuts separated by ';' (frame:ms[:send|receive])")
    parser.add_argument('--ticks', type=int, default=3600)
    parser.add_argument('--port', type=int, default=41861)
    parser.add_argument('--lag-ms', type=int, default=0, help="direct path only: the joiner's fake lag a leg")
    parser.add_argument('--host-blackout', default=None,
                        help="direct path only: CC_TEST_LINK_BLACKOUT for the host (seat:frame:ms), an earlier hold and return before the cut")
    parser.add_argument('--cut-peer', choices=['joiner', 'host'], default='joiner',
                        help="direct path only: whose process runs the cut; the host's send first starves its receipts while the joiner's inputs arrive")
    parser.add_argument('--host-stall', default=None,
                        help="direct path only: -net-test-live-stall for the host (tick:ms), its own seat held and returned before the cut")
    parser.add_argument('--outbox-limit', type=int, default=None,
                        help='the most unacknowledged commands the cut joiner may hold; unset reports the series only')
    args = parser.parse_args()
    result = run_case(args.repo.resolve(), args.out.resolve(), args)
    print(('PASS' if result['pass'] else 'FAIL') + ' ' + '; '.join(result['errors']), flush=True)
    return 0 if result['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
