"""Detect start and return timing with three local engines; the relay proof is separate."""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import subprocess

from run_sim_test import make_run, engine_executable


def run_case(repo, out, case, timeout, lag_ms, initial_delay):
    out.mkdir(parents=True, exist_ok=False)
    runs = []
    participants = []
    returning = case in ('blackout-return', 'slow-return')
    ticks = 2400 if returning or case == 'shrink' else 180
    clean_requirement = 26 if case == 'shrink' else 23
    if case == 'shrink':
        initial_delay = 26
    schedule = out / 'path.json'
    schedule.write_text(json.dumps([dict(id='longer-path', action='lag', tick=600,
        lag_ms=lag_ms, duration_ms=120000, duration_ticks=10000)]))
    common = ['-net-match-service-e2e', '-net-port', '47631', '-net-match-peers', '3',
              '-net-match-mode', 'coop-pve', '-net-match-ticks', str(ticks),
              '-max-ticks', str(ticks), '-net-match-input-delay', str(initial_delay),
              '-net-match-auto-delay', '-net-match-service-preset', 'Skirmish Defense',
              '-net-match-service-scene', 'Grasslands', '-net-match-e2e-brain-placement',
              '-seed', '42', '-num-lua-states', '4']
    try:
        for peer in range(1, 4):
            folder = out / f'peer{peer}'
            report = folder / 'report.json'
            live = folder / 'live.jsonl'
            flags = ['-net-host'] if peer == 1 else ['-net-join', '127.0.0.1']
            if peer == 1:
                flags += ['-net-fake-lag', '100']
            env = {'CCCP_HEADLESS': '1', 'CC_TEST_NET_MATCH_E2E_WAIT_PEERS': '1'}
            if returning:
                env['CC_TEST_RETURN_INPUT_DETAILS'] = '1'
            if peer == 1 and returning:
                env['CC_TEST_LINK_BLACKOUT'] = '1:600:8000'
                env['CC_TEST_SCENE_PATH_RTT_MS'] = '450'
                if lag_ms != 100:
                    flags += ['-net-cross-schedule', str(schedule)]
                    env.update(CC_TEST_CROSS_RECORDS=str(folder / 'events.jsonl'),
                               CC_TEST_CROSS_INSTANCE='host', CC_TEST_CROSS_RUN='return-timing',
                               CC_TEST_CROSS_EXECUTION='first', CC_TEST_CROSS_INCARNATION='0')
            if peer == 1 and case in ('clean-start', 'saturated-start'):
                env['CC_TEST_SCENE_PATH_RTT_MS'] = '450' if case == 'clean-start' else '900'
            if case == 'shrink':
                env['CC_TEST_MATCH_DELAY_EXCESS'] = '30'
            run = make_run(repo, common + flags + ['-net-match-report', str(report),
                           '-net-live-tick-hashes', str(live)], folder, timeout,
                           env=env, expected=[report, live])
            runs.append(run)
            run.start()
        with ThreadPoolExecutor(max_workers=3) as pool:
            records = list(pool.map(lambda run: run.finish(), runs))
    finally:
        for run in runs:
            run.close()
    errors = []
    shared_hashes = {}
    hash_mismatches = set()
    host_log = (out / 'peer1/stdout.log').read_text(errors='replace')
    end = re.search(r'\[net-link-blackout\] end peer=1 .*unix_ms=(\d+)', host_log)
    end_ms = int(end[1]) if end else None
    if returning and end_ms is None:
        errors.append('the host blackout did not finish')
    for peer, record in enumerate(records, 1):
        folder = out / f'peer{peer}'
        live = folder / 'live.jsonl'
        rows = [json.loads(line) for line in live.read_text().splitlines()] if live.exists() else []
        held = [row for row in rows if 1 in row['ai_held_peer_ids']]
        first_held = held[0]['tick'] if held else None
        returned = next((row for row in rows if first_held is not None and row['tick'] > first_held
                         and 1 not in row['ai_held_peer_ids']), None)
        elapsed = returned['unix_ms'] - end_ms if returned and end_ms else None
        report = json.loads((folder / 'report.json').read_text()) if (folder / 'report.json').exists() else {}
        locked = report.get('service', {}).get('runner', {}).get('lockstep', {})
        for row in rows:
            previous = shared_hashes.setdefault(row['tick'], [])
            if previous and previous[0] != row['total']:
                hash_mismatches.add(row['tick'])
            previous.append(row['total'])
        hold_counts = {key: value['holds'] for key, value in locked.get('peers', {}).items()}
        first_delay = rows[0]['input_delay_ticks'] if rows else None
        duplicate_tails = locked.get('peer_tail_duplicate_frames')
        recovery_frames = returned['tick'] - first_held if returned and first_held else 0
        shrunk = next((row for row in rows if row['input_delay_ticks'] <= clean_requirement + 2), None)
        shrink_ms = shrunk['unix_ms'] - rows[0]['unix_ms'] if shrunk and rows else None
        participants.append(dict(peer=peer, rows=len(rows), first_held=first_held,
                                 returned=returned['tick'] if returned else None,
                                 first_delay=first_delay, last_delay=rows[-1]['input_delay_ticks'] if rows else None,
                                 shrink_ms=shrink_ms, tail_frames_sent=locked.get('peer_tail_frames_sent'),
                                 duplicate_tails=duplicate_tails, recovery_frames=recovery_frames,
                                 holds=hold_counts,
                                 return_after_end_ms=elapsed, exit_code=record.get('exit_code'),
                                 timed_out=record.get('timed_out')))
        if returning and not held:
            errors.append(f'peer {peer} did not record the held host')
        if returning and (elapsed is None or elapsed > (10000 if case == 'slow-return' else 12000)):
            seconds = 10 if case == 'slow-return' else 12
            errors.append(f'peer {peer} did not return the host within {seconds} seconds after the blackout')
        if returning and (hold_counts.get('1') != 1 or any(value != 0 for key, value in hold_counts.items() if key != '1')):
            errors.append(f'peer {peer} records an unintended hold')
        if case == 'slow-return' and (duplicate_tails is None or recovery_frames <= 0 or duplicate_tails >= recovery_frames):
            errors.append(f'peer {peer} has at least 1000 duplicate tail replies per 1000 recovery frames')
        if case in ('clean-start', 'saturated-start'):
            tolerance = 2 if case == 'clean-start' else 6
            if first_delay is None or first_delay > 36 or abs(first_delay - clean_requirement) > tolerance:
                errors.append(f'peer {peer} start delay differs from the clean path requirement or exceeds 36')
        if case == 'shrink':
            if first_delay is None or first_delay < clean_requirement + 30:
                errors.append(f'peer {peer} did not begin with a 30-tick excess')
            if shrink_ms is None or shrink_ms > 30000:
                errors.append(f'peer {peer} did not remove the excess delay within 30 seconds')
        if record.get('exit_code') != 0 or record.get('timed_out'):
            errors.append(f'peer {peer} did not finish normally')
    if hash_mismatches:
        errors.append('the participants disagree on shared tick hashes')
    result = dict(case=case, lag_ms=lag_ms, initial_delay=initial_delay, pass_=not errors, errors=errors, participants=participants,
                  source_tip=subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip(),
                  binary_sha256=hashlib.sha256(engine_executable(repo).read_bytes()).hexdigest(),
                  shared_ticks=sum(len(values) > 1 for values in shared_hashes.values()), hash_mismatches=sorted(hash_mismatches),
                  topology='three engines on one Linux box', relay_proof=False)
    result['pass'] = result.pop('pass_')
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result), flush=True)
    return 0 if result['pass'] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--case', choices=['blackout-return', 'clean-start', 'saturated-start', 'shrink', 'slow-return'], required=True)
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--lag-ms', type=int, default=500)
    parser.add_argument('--initial-delay', type=int, default=1)
    options = parser.parse_args()
    return run_case(options.repo.resolve(), options.out.resolve(), options.case, options.timeout,
                    options.lag_ms, options.initial_delay)


if __name__ == '__main__':
    raise SystemExit(main())
