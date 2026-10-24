"""A host the directory rules against in a split comes back into the ruling half's match.

Two engines join through a local session directory with ICE, as an internet match does. The guest stops receiving at
--cut-frame and stops sending a moment later, so its half holds the host first and asks the directory's referee first; when
the cut heals each half has held the other, the referee's ruling names the guest's half, and the host stops at the decision
frame to rejoin that half through the image return. Green: the host was ruled against, its rejoin never dials its own
listing, and it is back in the guest's match (its seat reclaimed there) while the guest plays on - and stays back: no
hold of its seat between its return and its last tick on either side, both engines exit 0, and neither stderr carries an
assertion or abort.
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


ABORTS = re.compile(r"^.*(?:[Aa]ssertion\b.*\bfailed|Stack trace \(most recent call last\)|terminate called after).*$", re.M)


def held_again(logs, back, host_last):
    """Every hold of the returned host's seat after its return, from either side, while it still played."""
    holds = [(int(frame), f'the guest held the returned host at frame {frame} (cause {cause})')
             for frame, cause in re.findall(r'\[net-peer-hold-installed\] local=2 peer=1 frame=(\d+) .*?cause=(\w+)', logs[2])]
    holds += [(int(frame), f'the returned host held its own seat at frame {frame} (cause {cause})')
              for frame, cause in re.findall(r'\[net-peer-hold-installed\] local=1 peer=1 frame=(\d+) .*?cause=(\w+)', logs[1])]
    holds += [(int(frame), f'the returned host saw its seat held at frame {frame}')
              for frame in re.findall(r'\[net-peer-foreign-hold\] local=1 frame=(\d+)', logs[1])]
    # A hold past the host's last tick is its scripted end of stay; the host's own view of a hold is never that.
    late = [(frame, text) for frame, text in holds
            if frame > back and (host_last is None or frame <= host_last or 'saw its seat' in text)]
    return [text for _, text in sorted(set(late))[:1]] + ([f'{len(set(late)) - 1} more hold line(s) of the returned host'] if len(set(late)) > 1 else [])


def judge(logs, rows, exits=None, stderrs=None):
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
    if back:
        errors += held_again(logs, int(back[1]), host_last)
    for peer, role in ((1, 'host'), (2, 'guest')):
        code = (exits or {}).get(peer)
        if code != 0:
            errors.append(f'the {role} exited with code {code}')
        abort = ABORTS.search((stderrs or {}).get(peer, ''))
        if abort:
            errors.append(f"the {role}'s stderr carries an abort: {abort[0].strip()[:200]}")
    return errors, dict(ruling_frame=int(ruling[1]) if ruling else None, winners=int(ruling[2]) if ruling else None,
                        self_dials=len(dialled_self), host_back_at=int(back[1]) if back else None,
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
    stderrs = {peer: (out / f'peer{peer}/stderr.log').read_text(errors='replace') if (out / f'peer{peer}/stderr.log').exists() else ''
               for peer in (1, 2)}
    errors, facts = judge(logs, rows, {peer: record.get('exit_code') for peer, record in enumerate(records, 1)}, stderrs)
    for peer, record in enumerate(records, 1):
        if record.get('timed_out'):
            errors.append(f'peer {peer} did not finish before its timeout')
    result = dict(cut=cut, **facts, errors=errors, host_exit=records[0].get('exit_code'), guest_exit=records[1].get('exit_code'),
                  source_tip=subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip(),
                  binary_sha256=hashlib.sha256(engine_executable(repo).read_bytes()).hexdigest(),
                  topology='two engines on one box', proof=False)
    result['pass'] = not errors
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result), flush=True)
    return result


def self_test():
    """The judge on two recorded shapes: a returned host re-held and aborted must fail; a clean return must pass."""
    ruling = '[net-split-arbitration] local=1 split=598 frame=601 winners=2 decider=2 action=image_return\n'
    guest_hold = '[net-peer-hold-installed] local=2 peer=1 frame=600 ai_frame=600 voters=1 now_ms=29562 cause=certified_peer_decision\n'
    reclaim = '[net-match] seat-reclaimed peer=1 frame=4541 live_actors=2\n'
    host_rehold = ('[net-peer-foreign-hold] local=1 frame=4620 next=4653 displayed=4652 waiting=18446744073709551615 now_ms=97099\n'
                   '[net-peer-decision-refused] local=1 frame=4620 kind=17 reason=no playing seat next=4653 displayed=4652\n')
    abort = ("/usr/include/c++/13/bits/unique_ptr.h:453: std::unique_ptr<_Tp, _Dp>::operator*() const [with _Tp = RTE::INetTransport]: "
             "Assertion 'get() != pointer()' failed.\nStack trace (most recent call last):\n")
    end_of_stay = '[net-peer-hold-installed] local=2 peer=1 frame=9183 ai_frame=9183 voters=1 now_ms=190000 cause=declared_leave\n'
    rows = lambda last: [{'tick': tick} for tick in (4541, last)]
    cases = (
        ('re-held and aborted (the recorded Linux run)', {1: ruling + reclaim + host_rehold, 2: guest_hold + reclaim}, {1: rows(5526), 2: rows(5525)},
         {1: -6, 2: 0}, {1: abort, 2: ''}, ('returned host saw its seat held at frame 4620', 'exited with code -6', 'abort')),
        ('re-held only', {1: ruling + reclaim + host_rehold, 2: guest_hold + reclaim}, {1: rows(5526), 2: rows(5525)}, {1: 0, 2: 0}, {}, ('returned host saw its seat held',)),
        ('held by the guest while still playing', {1: ruling + reclaim, 2: guest_hold + reclaim + end_of_stay.replace('9183', '5000')},
         {1: rows(9181), 2: rows(9302)}, {1: 0, 2: 0}, {}, ('guest held the returned host at frame 5000',)),
        ('exit code only', {1: ruling + reclaim, 2: guest_hold + reclaim}, {1: rows(9181), 2: rows(9302)}, {1: 0, 2: 3}, {}, ('exited with code 3',)),
        ('abort with exit 0', {1: ruling + reclaim, 2: guest_hold + reclaim}, {1: rows(9181), 2: rows(9302)}, {1: 0, 2: 0}, {1: abort}, ('abort',)),
        ('clean return, held only at its end of stay (the recorded Mac run)', {1: ruling + reclaim, 2: guest_hold + reclaim + end_of_stay},
         {1: rows(9181), 2: rows(9302)}, {1: 0, 2: 0}, {1: '', 2: ''}, ()),
    )
    failures = []
    for name, logs, case_rows, exits, stderrs, expected in cases:
        errors = judge(logs, case_rows, exits, stderrs)[0]
        text = '; '.join(errors)
        if bool(errors) != bool(expected) or any(word not in text for word in expected):
            failures.append(f'{name}: expected {list(expected) or "a pass"}, the judge said {errors or "pass"}')
    for line in failures:
        print('FAIL ' + line)
    print(f'{len(cases) - len(failures)}/{len(cases)} judge cases', flush=True)
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--self-test', action='store_true', help="judge recorded run shapes; no engine starts")
    parser.add_argument('--out', type=Path)
    parser.add_argument('--timeout', type=int, default=360)
    parser.add_argument('--cut-frame', type=int, default=600)
    parser.add_argument('--cut-ms', type=int, default=20000)
    parser.add_argument('--stay-ms', type=int, default=150000, help="how long each player stays after its round starts")
    parser.add_argument('--port', type=int, default=41861)
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.out:
        parser.error('--out is required')
    result = run_case(args.repo.resolve(), args.out.resolve(), args)
    print(('PASS' if result['pass'] else 'FAIL') + ' ' + '; '.join(result['errors']), flush=True)
    return 0 if result['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
