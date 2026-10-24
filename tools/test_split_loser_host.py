"""A host the directory rules against in a split comes back into the ruling half's match.

Two engines join through a local session directory with ICE, as an internet match does. The guest stops receiving at
--cut-frame and stops sending a moment later, so its half holds the host first and asks the directory's referee first; when
the cut heals each half has held the other, the referee's ruling names the guest's half, and the host stops at the decision
frame to rejoin that half through the image return. Green: the host was ruled against, its rejoin never dials its own
listing, and it is back in the guest's match (its seat reclaimed there) while the guest plays on.
Topology: two engines on one box (proof: false); no relay server is in the loop.
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
from test_link_cut_hold import live_rows


def runs(repo, out, timeout, port, cut, stay_ms, stack):
    from test_directory_cadence import local_directory, stage_activity
    from test_lobby_lifecycle import wait_for_log
    server, _requests, pin = stack.enter_context(local_directory(out / 'directory', 'split-loser-host', port + 1))
    common = 'in_play\nwait_ms 1000\nactivate ButtonMainToMultiplayer\nwait_ms 300\n'
    menus = {
        'host': common + 'settext TextMultiplayerName Host\nactivate ButtonMultiplayerHostGame\nwait_ms 300\n'
                'combo_select ComboHostActivity Directory Cadence\nwait_ms 300\n'
                f'setup_host_port {port}\ncombo_select ComboHostPlayers 2\nactivate ButtonMultiplayerCreate\n'
                'wait_connected 2 80\nwait_remote_ready 80\nactivate ButtonMultiplayerStart\n'
                f'wait_lockstep 180 60\nwait_ms {stay_ms}\nexit\n',
        'guest': common + 'settext TextMultiplayerName Guest\nactivate ButtonMultiplayerJoinGame\nwait_ms 300\n'
                 'activate ButtonJoinByAddress\nwait 4\nsettext TextJoinAddress SESSION\n'
                 f'settext TextJoinPort {port}\nactivate ButtonJoinAddressGo\nwait_connected 2 80\n'
                 f'activate ButtonMultiplayerReady\nwait_lockstep 300 60\nwait_ms {stay_ms}\nexit\n',
    }
    started = []
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
        started.append(run)
        if role == 'host':
            wait_for_log(run, 'activate ButtonMultiplayerCreate click ButtonMultiplayerCreate PASS', 45)
    return started


def judge(logs, rows):
    errors = []
    host, guest = logs[1], logs[2]
    ruling = re.search(r'\[net-split-arbitration\] local=1 split=\d+ frame=(\d+) winners=(\d+) .*action=image_return', host)
    if not ruling:
        errors.append('the directory never ruled against the host (no image return on the host)')
    # The host's own listening identity: the identity its clients dial for it.
    own = set(re.findall(r'remote=(str:h-\d+)', guest))
    dialled_self = [line for line in re.findall(r'\[net-transport\] closed peer=1 reason=[^\n]*side=client remote=(str:h-\d+)', host) if line in own]
    if dialled_self:
        errors.append(f'the ruled-out host dialled its own listing {len(dialled_self)} time(s) ({dialled_self[0]})')
    back = ruling and re.search(r'\[net-match\] seat-reclaimed peer=1 frame=(\d+)', guest[guest.find('[net-peer-hold-installed] local=2 peer=1'):] if '[net-peer-hold-installed] local=2 peer=1' in guest else '')
    if not back:
        errors.append("the host never took its seat back in the guest's match")
    guest_rows = rows[2]
    host_rows = rows[1]
    guest_last = guest_rows[-1]['tick'] if guest_rows else None
    host_last = host_rows[-1]['tick'] if host_rows else None
    if back and (host_last is None or host_last <= int(back[1])):
        errors.append(f'the host did not play on past its return (last tick {host_last})')
    # Back means playing: a returned seat the round holds again before the guest's stay ends never got back.
    reheld = []
    if back:
        # The hold the host's own exit at its stay's end draws comes after its last tick and is not a return failing.
        reheld = [int(frame) for frame in re.findall(r'\[net-peer-hold-installed\] local=2 peer=1 frame=(\d+)', guest)
                  if int(frame) > int(back[1]) and (host_last is None or int(frame) < host_last)]
        if reheld:
            errors.append(f'the returned host was held again at frame {reheld[0]}')
    return errors, dict(ruling_frame=int(ruling[1]) if ruling else None, winners=int(ruling[2]) if ruling else None,
                        self_dials=len(dialled_self), host_back_at=int(back[1]) if back else None, reheld_at=reheld[0] if reheld else None,
                        host_last_tick=host_last, guest_last_tick=guest_last)


def run_case(repo, out, args):
    out.mkdir(parents=True, exist_ok=False)
    cut = f'{args.cut_frame}:{args.cut_ms}:receive;{args.cut_frame + 30}:{args.cut_ms}:send'
    with ExitStack() as stack:
        started = []
        try:
            started = runs(repo, out, args.timeout, args.port, cut, args.stay_ms, stack)
            with ThreadPoolExecutor(max_workers=2) as pool:
                records = list(pool.map(lambda run: run.finish(), started))
        finally:
            for run in started:
                run.close()
    logs = {peer: (out / f'peer{peer}/stdout.log').read_text(errors='replace') if (out / f'peer{peer}/stdout.log').exists() else ''
            for peer in (1, 2)}
    rows = {peer: live_rows(out / f'peer{peer}/live.jsonl') for peer in (1, 2)}
    errors, facts = judge(logs, rows)
    for peer, record in enumerate(records, 1):
        if record.get('timed_out'):
            errors.append(f'peer {peer} did not finish before its timeout')
        elif record.get('exit_code') != 0:
            errors.append(f'peer {peer} exited with {record.get("exit_code")}')
    result = dict(cut=cut, **facts, errors=errors, host_exit=records[0].get('exit_code'), guest_exit=records[1].get('exit_code'),
                  source_tip=subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip(),
                  binary_sha256=hashlib.sha256(engine_executable(repo).read_bytes()).hexdigest(),
                  topology='two engines on one box', proof=False)
    result['pass'] = not errors
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result), flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=360)
    parser.add_argument('--cut-frame', type=int, default=600)
    parser.add_argument('--cut-ms', type=int, default=20000)
    parser.add_argument('--stay-ms', type=int, default=150000, help="how long each player stays after its round starts")
    parser.add_argument('--port', type=int, default=41861)
    args = parser.parse_args()
    result = run_case(args.repo.resolve(), args.out.resolve(), args)
    print(('PASS' if result['pass'] else 'FAIL') + ' ' + '; '.join(result['errors']), flush=True)
    return 0 if result['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
