"""Require complete two-peer evidence and identical pie, controller and hash state on both peers.

The F23c sites are local UI writes into pie and controller state the sim dumps, so the oracle is peer
equality: for every actor and tick the pie interaction state, control bits, input mode and disabled bit
must match, every tick hash must match, and the raw dumps must be identical. Each case also has to prove
its site was reached, or a run that never opened the menu would pass without testing anything.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import json
import os
from pathlib import Path
import re

LO, HI = 27, 320
TICKS = set(range(LO, HI + 1))
CASES = ('buy_menu', 'form_squad', 'full_inventory')
PORT_LO, PORT_HI = 48171, 48179
SUBSYSTEMS = {'actors', 'controller', 'funds', 'lua_state', 'rot_angle',
              'rot_angvel', 'scene', 'sim_rng', 'terrain', 'tick'}
ACTOR_FIELDS = set('pos prev vel angvel moid mass frc imps moi atoms rest osc settle pvel '
                   'wdmg air rot awm inv gold base ninv ctrl mode dis status team aimode wp '
                   'health aim flip pie mstate goldpicked atmr recent view prevhealth alarm '
                   'imp wounds scr'.split())
PEER_FIELDS = ('pie', 'ctrl', 'mode', 'dis')
HEAD = re.compile(r'^(\d+) actor uid=(\d+) (.*?) (?=pos=)(.*)$')
ACTIVITY = re.compile(r'^(\d+) activity state=(\d+) t0=([01])/(\d+) t1=([01])/(\d+) t2=([01])/(\d+) t3=([01])/(\d+)$')
FIELD = re.compile(r'(?:^| )([a-zA-Z0-9_]+)=')
HASH = re.compile('[0-9a-f]{64}')
# The slice the fixture activates, and the slice count the site A removal leaves behind.
ACTIVATED_SLICE = {'form_squad': 'Follow', 'full_inventory': 'Inventory'}
CRAB_SLICES_WITH_BUY_MENU = 8


def normalized_path(value):
    return os.path.normcase(str(Path(value).resolve()))


def successful_launches(run, case):
    manifest = json.loads((run / 'manifest.json').read_text(encoding='utf-8-sig'))
    results = json.loads((run / 'run_result.json').read_text(encoding='utf-8-sig'))
    failures = []
    if manifest.get('case') != case:
        failures.append('run manifest case differs')
    if manifest.get('input_delay') != 3:
        failures.append('run manifest input delay differs')
    if not isinstance(manifest.get('exe_sha256'), str) or not HASH.fullmatch(manifest['exe_sha256']):
        failures.append('run manifest lacks executable identity')
    if set(results) != {'host', 'client'}:
        failures.append('run result peer set differs')
    for peer in ('host', 'client'):
        launch = json.loads((run / peer / 'launch.json').read_text(encoding='utf-8-sig'))
        result = results.get(peer, {})
        if launch.get('runner') != 'win32_test_runner.py':
            failures.append(f'{peer} launch runner identity differs')
        for kind, record in (('launch', launch), ('result', result)):
            for key, expected in (('exit_code', 0), ('timed_out', False), ('evidence_complete', True)):
                if type(record.get(key)) is not type(expected) or record[key] != expected:
                    failures.append(f'{peer} {kind} {key}={record.get(key)!r}, expected {expected!r}')
        if launch.get('started') is not True or launch.get('job_closed') is not True:
            failures.append(f'{peer} launch did not start and close successfully')
        if type(launch.get('pid')) is not int or launch['pid'] <= 0 or type(result.get('pid')) is not int or result['pid'] != launch['pid']:
            failures.append(f'{peer} launch/result process identity differs')
        if launch.get('exe_sha256') != manifest.get('exe_sha256'):
            failures.append(f'{peer} launch executable hash differs from manifest')
        argv = launch.get('argv', [])
        if not isinstance(argv, list) or not argv or not all(isinstance(arg, str) for arg in argv):
            failures.append(f'{peer} launch has invalid arguments')
            continue
        if normalized_path(argv[0]) != normalized_path(manifest.get('exe', '')) or normalized_path(launch.get('exe_path', '')) != normalized_path(argv[0]):
            failures.append(f'{peer} launch executable path differs from manifest')
        if '-headless' not in argv or launch.get('headless_env') != '1' or launch.get('env_set', {}).get('CCCP_HEADLESS') != '1':
            failures.append(f'{peer} launch is not headless')
        if launch.get('env_set', {}).get('CC_SIM_DUMP') != '27:320':
            failures.append(f'{peer} launch dump range differs')

        def argument(flag):
            positions = [index for index, arg in enumerate(argv) if arg == flag]
            return argv[positions[0] + 1] if len(positions) == 1 and positions[0] + 1 < len(argv) else None

        trace = run / (peer + '_trace.json')
        if argument('-max-ticks') != str(HI) or '-tick-hashes' not in argv or normalized_path(argument('-out') or '') != normalized_path(trace):
            failures.append(f'{peer} launch trace target or tick count differs')
        if ('-net-match-service-e2e' not in argv or argument('-net-match-ticks') != str(HI)
                or argument('-net-match-input-delay') != '3' or argument('-net-port') != str(manifest.get('port'))
                or not PORT_LO <= manifest.get('port', 0) <= PORT_HI
                or (peer == 'host' and '-net-host' not in argv)
                or (peer == 'client' and argument('-net-join') != '127.0.0.1')):
            failures.append(f'{peer} launch is not the specified lockstep arm')
        if argument('-test-script') != 'UserScenes.rte/PieWriteObserver.lua':
            failures.append(f'{peer} launch does not carry the lane observer')
        if (peer == 'host') != ('-input-script' in argv):
            failures.append(f'{peer} launch input-script placement differs')
        if peer == 'host' and normalized_path(argument('-input-script') or '') != normalized_path(Path(__file__).resolve().parent / 'fixtures' / (case + '.txt')):
            failures.append('host launch does not drive this case fixture')
        required = {normalized_path(trace), normalized_path(str(trace) + '.simdump.txt')}
        for key in ('evidence_expected', 'evidence_present'):
            if {normalized_path(path) for path in launch.get(key, [])} != required:
                failures.append(f'{peer} launch {key} does not name both complete trace artifacts')
        if launch.get('evidence_missing') != []:
            failures.append(f'{peer} launch reports missing evidence')
        checks = launch.get('startup_checks', [])
        required_checks = {'exe_exists', 'exe_sha256_recorded', 'cwd_exists', 'data_junction_resolves',
                           'userdata_writable', 'out_dir_private_writable', 'firewall_allow_rule_present'}
        if not required_checks <= {row.get('check') for row in checks if row.get('ok') is True} or any(row.get('ok') is not True for row in checks):
            failures.append(f'{peer} launch startup gates are incomplete or failed')
    return manifest, failures


def fields(text):
    matches = list(FIELD.finditer(text))
    return {m[1]: text[m.end():matches[i + 1].start() if i + 1 < len(matches) else None]
            for i, m in enumerate(matches)}


def load_dump(path):
    data = path.read_bytes()
    actors, activities, errors = defaultdict(dict), {}, []
    previous_tick = LO
    for number, line in enumerate(data.decode('utf-8-sig').splitlines(), 1):
        if not re.match(r'^\d+ (activity|actor|att|wnd|item|particle) ', line):
            errors.append(f'{path}:{number}: malformed dump row')
            continue
        row_tick = int(line.split(' ', 1)[0])
        if row_tick < previous_tick or row_tick not in TICKS:
            errors.append(f'{path}:{number}: dump tick order or range differs')
        previous_tick = row_tick
        if ' actor ' in line:
            match = HEAD.fullmatch(line)
            if not match:
                errors.append(f'{path}:{number}: malformed actor row')
                continue
            tick, uid = int(match[1]), int(match[2])
            row = fields(match[4])
            missing = ACTOR_FIELDS - row.keys()
            if missing:
                errors.append(f'{path}:{number}: actor {uid} missing fields {sorted(missing)}')
            if tick in actors[uid]:
                errors.append(f'{path}:{number}: duplicate actor {uid} tick {tick}')
            repeated = [key for key, count in Counter(FIELD.findall(match[4])).items() if count > 1 and key != 'inv']
            if repeated:
                errors.append(f'{path}:{number}: duplicate actor fields {repeated}')
            for key in ('mode', 'team', 'dis', 'status'):
                if not re.fullmatch(r'-?\d+', row.get(key, '')):
                    errors.append(f'{path}:{number}: invalid {key}')
            if not re.fullmatch(r'0x[0-9a-f]+', row.get('ctrl', '')):
                errors.append(f'{path}:{number}: invalid control bits')
            if not re.fullmatch(r'[0-3]:\d+:.*:.*:.*', row.get('pie', '')):
                errors.append(f'{path}:{number}: invalid pie state')
            actors[uid][tick] = dict(row, name=match[3], raw=line, line=number)
        elif ' activity ' in line:
            match = ACTIVITY.fullmatch(line)
            if not match:
                errors.append(f'{path}:{number}: malformed activity row')
                continue
            tick = int(match[1])
            if tick in activities:
                errors.append(f'{path}:{number}: duplicate activity tick {tick}')
            activities[tick] = line
    if list(activities) != list(range(LO, HI + 1)):
        errors.append(f'{path}: activity coverage differs from {LO}..{HI}')
    for uid, rows in actors.items():
        if list(rows) != list(range(LO, HI + 1)):
            errors.append(f'{path}: actor {uid} coverage differs from {LO}..{HI}')
    return data, actors, errors


def load_trace(path):
    document = json.loads(path.read_text(encoding='utf-8-sig'))
    runs = document.get('runs')
    if not isinstance(runs, list) or len(runs) != 1:
        raise ValueError(f'{path}: expected exactly one trace run')
    rows = runs[0].get('tick_hashes', [])
    if [r.get('tick') for r in rows] != list(range(1, HI + 1)):
        raise ValueError(f'{path}: trace ticks are not exactly 1..{HI}')
    for row in rows:
        subs = row.get('subsystems', {})
        if not SUBSYSTEMS <= subs.keys():
            raise ValueError(f'{path}: tick {row["tick"]} missing subsystems {sorted(SUBSYSTEMS - subs.keys())}')
        if not all(isinstance(v, str) and HASH.fullmatch(v) for v in [row.get('total'), *subs.values()]):
            raise ValueError(f'{path}: tick {row["tick"]} invalid hash')
    return rows


def pie_parts(row):
    return row['pie'].split(':')


def crab_uids(actors):
    return sorted(uid for uid, rows in actors.items() if rows[LO]['name'] == 'Crab')


def stimulus_failures(case, peers, stdout):
    """The site has to be reached on the seat peer, or peer equality proves nothing."""
    failures = []
    for peer in ('host', 'client'):
        if case == 'buy_menu' and '[pie-write] buymenu off' not in stdout[peer]:
            failures.append(f'{peer} never disabled the activity buy menu')
    host = peers['host']
    uids = crab_uids(host)
    if len(uids) != 1:
        failures.append(f'host dump holds {len(uids)} crabs, expected exactly 1')
        return failures
    rows = host[uids[0]]
    seated = [tick for tick, row in rows.items() if row['mode'] == '1']
    if not seated:
        failures.append('the crab was never seated by a player on the host')
    opened = [tick for tick, row in rows.items() if pie_parts(row)[0] in ('0', '1')]
    if not opened:
        failures.append('the crab pie menu never opened on the host')
    if case == 'buy_menu':
        removed = [peer for peer, actors in peers.items()
                   if any(pie_parts(row)[1] != str(CRAB_SLICES_WITH_BUY_MENU) for row in actors[uids[0]].values())]
        if not removed:
            failures.append('no peer ever removed the BuyMenu slice, so site A was not reached')
    else:
        wanted = ACTIVATED_SLICE[case]
        picked = [tick for tick, row in rows.items() if wanted in pie_parts(row)[3:5]]
        if not picked:
            failures.append(f'the {wanted} slice was never activated on the host, so the site was not reached')
    return failures


def compare_peers(peers, traces):
    failures = []
    host, client = peers['host'], peers['client']
    if set(host) != set(client):
        failures.append(f'peer actor sets differ: host only {sorted(set(host) - set(client))}, '
                        f'client only {sorted(set(client) - set(host))}')
    for uid in sorted(set(host) & set(client)):
        for tick in sorted(set(host[uid]) & set(client[uid])):
            for field in PEER_FIELDS:
                if host[uid][tick].get(field) != client[uid][tick].get(field):
                    failures.append(f'actor {uid} tick {tick} {field} host {host[uid][tick].get(field)!r} '
                                    f'client {client[uid][tick].get(field)!r}')
    for index, (left, right) in enumerate(zip(traces['host'], traces['client']), 1):
        if left != right:
            differing = sorted(key for key in set(left['subsystems']) | set(right['subsystems'])
                               if left['subsystems'].get(key) != right['subsystems'].get(key))
            failures.append(f'tick {index} hash differs, subsystems {differing or ["total"]}')
    return failures


def raw_failures(raw):
    host = raw['host'].decode('utf-8-sig').splitlines()
    client = raw['client'].decode('utf-8-sig').splitlines()
    if host == client:
        return []
    for number, (left, right) in enumerate(zip(host, client), 1):
        if left != right:
            left_fields, right_fields = fields(left), fields(right)
            differing = sorted(key for key in set(left_fields) | set(right_fields)
                               if left_fields.get(key) != right_fields.get(key))
            return [f'full raw dumps differ; first differing line {number}, fields {differing}']
    return [f'full raw dumps differ in length: host {len(host)} lines, client {len(client)} lines']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('case', choices=CASES)
    parser.add_argument('run', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    run = args.run.resolve()
    manifest, failures = successful_launches(run, args.case)
    peers, raw, traces, stdout = {}, {}, {}, {}
    for peer in ('host', 'client'):
        data, actors, errors = load_dump(run / (peer + '_trace.json.simdump.txt'))
        failures += errors
        peers[peer], raw[peer] = actors, data
        traces[peer] = load_trace(run / (peer + '_trace.json'))
        stdout[peer] = (run / peer / 'stdout.log').read_text(encoding='utf-8-sig', errors='replace')
    if not failures:
        failures += stimulus_failures(args.case, peers, stdout)
        failures += compare_peers(peers, traces)
        failures += raw_failures(raw)
    result = {'case': args.case, 'run': str(run), 'exe_sha256': manifest.get('exe_sha256'),
              'pass_check': not failures, 'failure_count': len(failures), 'failures': failures[:200]}
    args.out.write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps({key: result[key] for key in ('case', 'exe_sha256', 'pass_check', 'failure_count')}, indent=2))
    for row in failures[:20]:
        print('FAIL: ' + row)
    return 0 if not failures else 1


if __name__ == '__main__':
    raise SystemExit(main())
