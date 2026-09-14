"""Require complete dumps, committed seat edges, and sustained pie closure."""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import hashlib
import json
import os
from pathlib import Path
import re

LO, HI = 27, 320
TICKS = set(range(LO, HI + 1))
SUBSYSTEMS = {'actors', 'controller', 'funds', 'lua_state', 'rot_angle',
              'rot_angvel', 'scene', 'sim_rng', 'terrain', 'tick'}
ACTOR_FIELDS = set('pos prev vel angvel moid mass frc imps moi atoms rest osc settle pvel '
                   'wdmg air rot awm inv gold base ninv ctrl mode dis status team aimode wp '
                   'health aim flip pie mstate goldpicked atmr recent view prevhealth alarm '
                   'imp wounds scr'.split())
HUMAN_FIELDS = set('jet bonus emit hstate htmr crouch fg bg offhandwait limbs paths feet'.split())
GUN_FIELDS = set('gun rounds reloading reloadms fire fired1 clicked full reload lastfire acttmr prefire frame'.split())
HEAD = re.compile(r'^(\d+) actor uid=(\d+) (.*?) (?=pos=)(.*)$')
ACTIVITY = re.compile(r'^(\d+) activity state=(\d+) t0=([01])/(\d+) t1=([01])/(\d+) t2=([01])/(\d+) t3=([01])/(\d+)$')
FIELD = re.compile(r'(?:^| )([a-zA-Z0-9_]+)=')
HASH = re.compile('[0-9a-f]{64}')
PRESS = re.compile(r'\[input-script\] tick (\d+) player (\d+) pressed (\S+)')
OBSERVE = re.compile(r'\[pie-observe\] uid=(\d+) step=(\d+) mode=(\d+) state=([01]{6}) callbacks=(\d+) modechanges=(\d+)')
PLAYER_BITS = sum(1 << bit for bit in (0, 1, *range(19, 23), *range(35, 56)))
CANCEL_CASES = ('actor_cancel', 'delivery_cancel')


def normalized_path(value):
    return os.path.normcase(str(Path(value).resolve()))


def successful_launches(run, case, sp, allow_synthetic=False):
    manifest = json.loads((run / 'manifest.json').read_text(encoding='utf-8-sig'))
    results = json.loads((run / 'run_result.json').read_text(encoding='utf-8-sig'))
    peers = ['sp'] if sp else ['host', 'client']
    failures = []
    synthetic = manifest.get('synthetic') is True
    if synthetic and not allow_synthetic:
        failures.append('synthetic launch metadata requires unit-test opt-in')
    if manifest.get('case') != case or manifest.get('sp') is not sp:
        failures.append('run manifest case or SP mode differs')
    if manifest.get('input_delay') != (0 if sp else 3):
        failures.append('run manifest input delay differs')
    if not isinstance(manifest.get('exe_sha256'), str) or not HASH.fullmatch(manifest['exe_sha256']):
        failures.append('run manifest lacks executable identity')
    if set(results) != set(peers):
        failures.append('run result peer set differs')
    for peer in peers:
        launch = json.loads((run / peer / 'launch.json').read_text(encoding='utf-8-sig'))
        result = results.get(peer, {})
        runner = 'synthetic-fixture' if synthetic else 'win32_test_runner.py'
        if launch.get('runner') != runner or (synthetic and launch.get('synthetic') is not True):
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
        if sp:
            if argument('-scenario') != 'PieSwitchSP' or any(arg.startswith('-net-') for arg in argv):
                failures.append(f'{peer} launch is not the SP scenario')
        elif ('-net-match-service-e2e' not in argv or argument('-net-match-ticks') != str(HI)
              or argument('-net-match-input-delay') != '3' or argument('-net-port') != str(manifest.get('port'))
              or not 47921 <= manifest.get('port', 0) <= 47926
              or (peer == 'host' and '-net-host' not in argv)
              or (peer == 'client' and argument('-net-join') != '127.0.0.1')):
            failures.append(f'{peer} launch is not the specified lockstep arm')
        required = {normalized_path(trace), normalized_path(str(trace) + '.simdump.txt')}
        for key in ('evidence_expected', 'evidence_present'):
            if {normalized_path(path) for path in launch.get(key, [])} != required:
                failures.append(f'{peer} launch {key} does not name both complete trace artifacts')
        if launch.get('evidence_missing') != []:
            failures.append(f'{peer} launch reports missing evidence')
        checks = launch.get('startup_checks', [])
        required_checks = {'exe_exists', 'exe_sha256_recorded', 'cwd_exists', 'data_junction_resolves',
                           'userdata_writable', 'out_dir_private_writable'}
        if not sp:
            required_checks.add('firewall_allow_rule_present')
        if not required_checks <= {row.get('check') for row in checks if row.get('ok') is True} or any(row.get('ok') is not True for row in checks):
            failures.append(f'{peer} launch startup gates are incomplete or failed')
    return manifest, failures


def fields(text):
    matches = list(FIELD.finditer(text))
    return {m[1]: text[m.end():matches[i + 1].start() if i + 1 < len(matches) else None]
            for i, m in enumerate(matches)}


def load_dump(path, require_script=False):
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
            required = ACTOR_FIELDS | (HUMAN_FIELDS if match[3] != 'Crab' else set())
            if row.get('fg', '0') != '0':
                required |= GUN_FIELDS
                if not re.fullmatch(r'[01]{2} gate\[act=[01]', row.get('fire', '')) or not re.fullmatch(r'\d+/\d+\]', row.get('frame', '')):
                    errors.append(f'{path}:{number}: incomplete firearm gate')
            if require_script:
                required |= {'script', 'nv'}
            missing = required - row.keys()
            if missing:
                errors.append(f'{path}:{number}: actor {uid} missing fields {sorted(missing)}')
            if (tick in actors[uid]):
                errors.append(f'{path}:{number}: duplicate actor {uid} tick {tick}')
            if len(re.findall(r'(?:^| )inv=', match[4])) != 2:
                errors.append(f'{path}:{number}: missing mass or inventory-list field')
            repeated = [key for key, count in Counter(FIELD.findall(match[4])).items() if count > 1 and key != 'inv']
            if repeated:
                errors.append(f'{path}:{number}: duplicate actor fields {repeated}')
            for key in ('mode', 'team', 'dis', 'status', 'aimode', 'wp'):
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
            activities[tick] = (int(match[2]), [(int(match[i]), int(match[i + 1])) for i in (3, 5, 7, 9)])
    if list(activities) != list(range(LO, HI + 1)):
        errors.append(f'{path}: activity coverage differs from {LO}..{HI}')
    for uid, rows in actors.items():
        if list(rows) != list(range(LO, HI + 1)):
            errors.append(f'{path}: actor {uid} coverage differs from {LO}..{HI}')
    return data, actors, activities, errors


def load_trace(path):
    document = json.loads(path.read_text(encoding='utf-8-sig'))
    runs = document.get('runs')
    if not isinstance(runs, list) or len(runs) != 1:
        raise ValueError(f'{path}: expected exactly one trace run')
    rows = runs[0].get('tick_hashes', [])
    if [r.get('tick') for r in rows] != list(range(1, HI + 1)):
        raise ValueError(f'{path}: trace ticks are not exactly 1..{HI}')
    for row in rows:
        if type(row.get('paused')) is not bool:
            raise ValueError(f'{path}: tick {row["tick"]} missing or invalid paused state')
        subs = row.get('subsystems', {})
        if not SUBSYSTEMS <= subs.keys():
            raise ValueError(f'{path}: tick {row["tick"]} missing subsystems {sorted(SUBSYSTEMS - subs.keys())}')
        if not all(isinstance(v, str) and HASH.fullmatch(v) for v in [row.get('total'), *subs.values()]):
            raise ValueError(f'{path}: tick {row["tick"]} invalid hash')
    return rows


def values(row):
    return int(row['mode']), int(row['ctrl'], 16), int(row['pie'].split(':')[0])


def load_observations(text, actors, sp):
    rows, errors = {}, []
    for uid, step, mode, state, callbacks, changes in OBSERVE.findall(text):
        key = int(uid), int(step)
        if key in rows:
            errors.append(f'duplicate Lua observation {key}')
        rows[key] = (int(mode), state, int(callbacks), int(changes))
    for uid in actors:
        expected = set(range(1, HI + 1 - (22 if not sp and uid == 1048736 else 0)))
        steps = {step for actor, step in rows if actor == uid}
        if steps != expected:
            errors.append(f'Lua observation coverage differs for actor {uid}: {len(steps)}/{len(expected)}')
    if {uid for uid, _ in rows} != set(actors):
        errors.append('Lua observation actor set differs from dump')
    return rows, errors


def semantic_checks(case, actors, activities, stdout, sp=False):
    errors, observed = [], {}
    expected_uids = {1048577, 1048587} if sp else {1048577, 1048596, 1048615, 1048634, 1048736}
    expected_names = {uid: 'Green Dummy' for uid in expected_uids}
    if not sp:
        expected_names.update({1048577: 'Brain Robot', 1048596: 'Brain Robot', 1048736: 'Crab'})
    if set(actors) != expected_uids:
        errors.append(f'actor identities {sorted(actors)}, expected {sorted(expected_uids)}')
    for tick, (state, teams) in activities.items():
        if state != 4:
            errors.append(f'tick {tick} activity state {state}, expected Running (4)')
        for team, (brain, count) in enumerate(teams):
            team_rows = [by[tick] for by in actors.values() if tick in by and int(by[tick]['team']) == team]
            if count != len(team_rows):
                errors.append(f'tick {tick} activity roster team {team} count {count}, actors {len(team_rows)}')
            expected_brain = 0 if sp else int(team in (0, 1))
            if brain != expected_brain:
                errors.append(f'tick {tick} activity brain flag team {team} {brain}, expected {expected_brain}')
    for uid, by in actors.items():
        for tick, row in by.items():
            if row['name'] != expected_names.get(uid):
                errors.append(f'actor {uid} tick {tick} preset {row["name"]!r}, expected {expected_names.get(uid)!r}')
            mode, bits, pie = values(row)
            if mode == 2 and bits & PLAYER_BITS:
                errors.append(f'AI actor {uid} tick {tick} carries player bits {hex(bits & PLAYER_BITS)} (ctrl={row["ctrl"]})')
            if 'Brain' in row['name'] and mode != 1 and bits & (1 << 38):
                errors.append(f'unseated brain {uid} tick {tick} carries ACTOR_NEXT_PREP')
    departing = 1048587 if sp else 1048736
    replacement = 1048577 if sp else 1048615
    if departing not in actors or replacement not in actors or any(set(by) != TICKS for by in actors.values()):
        return errors + ['incomplete actors prevent seat validation'], observed
    old, new = actors[departing], actors[replacement]
    handoffs = [t for t in range(LO + 1, HI + 1) if values(old[t - 1])[0] == 1 and values(old[t])[0] == 2]
    observed['handoffs'] = handoffs
    observed['departing_uid'], observed['replacement_uid'] = departing, replacement
    if case in ('next', 'prev', 'goto'):
        switch = 240 if case == 'goto' else 199
        handoff = switch + 2 + (0 if sp else 3)
        initial = 32 if sp else 35
        edge = 36 if case == 'prev' else 35
        observed['expected_handoff'] = handoff
        if handoffs != [handoff]:
            errors.append(f'actor {departing} handoffs {handoffs}, expected only {handoff}')
        if not values(old[initial])[1] & (1 << 35):
            errors.append(f'initial replacement {departing} tick {initial} did not consume ACTOR_NEXT')
        for tick in range(initial, handoff):
            if values(old[tick])[0] != 1:
                errors.append(f'departing actor {departing} tick {tick} is not seated')
        for tick in range(handoff, HI + 1):
            if values(old[tick])[0] != 2 or values(new[tick])[0] != 1:
                errors.append(f'tick {tick} replacement seat {departing}->{replacement} is not committed')
        if values(new[handoff - 1])[0] != 2 or not values(new[handoff])[1] & (1 << edge):
            errors.append(f'replacement {replacement} tick {handoff} did not consume switch edge bit {edge}')
        observed['replacement_edge'] = new[handoff]['ctrl']
        if case in ('next', 'prev'):
            if not sp:
                for tick in range(switch, handoff):
                    if values(old[tick])[2] not in (0, 1) or not values(old[tick])[1] & (1 << 20):
                        errors.append(f'tick {tick} loses open pie before committed handoff')
            start = handoff + (4 if sp else 1)
            for tick in range(start, HI + 1):
                if values(old[tick])[2] != 3:
                    errors.append(f'departing actor {departing} tick {tick} pie {old[tick]["pie"]}, SP reference Disabled (3)')
            observed['post_handoff_range'] = [start, HI]
        else:
            goto = [t for t in sorted(old) if int(old[t]['aimode']) == 3]
            hovered = [t for t in old if 'go to' in old[t]['pie'].lower().replace('-', ' ')]
            observed.update(goto_ticks=goto, goto_hover_ticks=hovered)
            if not goto or not hovered:
                errors.append('missing Go To hover and committed AI mode')
            if any(values(row)[1] & (1 << 16) for row in old.values()):
                errors.append('Go To cancellation fired a weapon')
            if goto and any(int(old[t]['wp']) for t in old if t >= goto[0]):
                errors.append('Go To cancellation created a waypoint')
    presses = [(int(t), int(p), key) for t, p, key in PRESS.findall(stdout)]
    expected = [(30, 0, 'NEXT')]
    if case in ('next', 'prev', 'goto'):
        expected += [(150, 0, 'PIEMENU_DIGITAL')]
        if case == 'goto':
            expected += [(160, 0, 'L_RIGHT'), (240, 0, 'NEXT')]
        elif case == 'next':
            expected += [(199, 0, 'NEXT')]
    else:
        expected += [(199, 0, 'PIEMENU_DIGITAL')]
        view = 'ActorSelect' if case == 'actor_cancel' else 'LandingZoneSelect'
        if f'[pie-fixture] enter {view} uid={departing}' not in stdout:
            errors.append(f'missing local {view} entry on actor {departing}')
        views = [(int(step), int(value)) for uid, step, value in re.findall(r'\[pie-view\] uid=(\d+) step=(\d+) view=(\d+)', stdout) if int(uid) == departing]
        selected = [step for step, value in views if value == (3 if case == 'actor_cancel' else 8)]
        if not selected or not any(step > selected[-1] and value == 0 for step, value in views):
            errors.append(f'missing {view} entry and cancel-to-Normal sequence')
        observed['view_ticks'] = selected
    if presses != expected:
        errors.append(f'input presses {presses}, expected {expected}')
    observed['presses'] = presses
    return errors, observed


def check(run, case, sp=False, reference=None, allow_synthetic=False):
    peers = ['sp'] if sp else ['host', 'client']
    errors, observed, dumps, traces, observations = [], {}, {}, {}, {}
    manifest, metadata_failures = successful_launches(run, case, sp, allow_synthetic)
    errors += metadata_failures
    require_observations = manifest.get('observe', False) or case in CANCEL_CASES
    peer_actors = {}
    for peer in peers:
        raw, actors, activities, failures = load_dump(run / (peer + '_trace.json.simdump.txt'), require_script=require_observations)
        peer_actors[peer] = actors
        errors += [peer + ' ' + f for f in failures]
        stdout_path = run / peer / 'stdout.log'
        if not stdout_path.exists():
            stdout_path = run / (peer + '.out.txt')
        stdout = stdout_path.read_text(encoding='utf-8-sig')
        console = run / peer / 'runtime/LogConsole.txt'
        if console.exists():
            stdout += '\n' + console.read_text(encoding='utf-8-sig')
        if re.search(r'ERROR|stack traceback|attempt to |Lua error', stdout):
            errors.append(peer + ' stdout contains an engine or Lua error')
        if not failures:
            failures, observed[peer] = semantic_checks(case, actors, activities, stdout if peer != 'client' else host_stdout, sp)
            errors += [peer + ' ' + f for f in failures]
        if peer == 'host':
            host_stdout = stdout
        if require_observations:
            observations[peer], failures = load_observations(stdout, actors, sp)
            errors += [peer + ' ' + f for f in failures]
        dumps[peer], traces[peer] = raw, load_trace(run / (peer + '_trace.json'))
    if not sp:
        if dumps['host'] != dumps['client']:
            diff = next((n for n, (a, b) in enumerate(zip(dumps['host'].splitlines(), dumps['client'].splitlines()), 1) if a != b), 0)
            errors.append(f'full raw dumps differ; first differing line {diff}')
        if traces['host'] != traces['client']:
            ticks = [a['tick'] for a, b in zip(traces['host'], traces['client']) if a != b]
            errors.append(f'complete trace rows differ at ticks {ticks}')
        if require_observations and observations['host'] != observations['client']:
            different = sorted(key for key in observations['host'].keys() | observations['client'].keys()
                               if observations['host'].get(key) != observations['client'].get(key))
            errors.append(f'script-visible pie observations differ at {len(different)} rows; first {different[:8]}')
    reference_result, reference_ticks = None, {}
    if not sp and case in CANCEL_CASES and reference is None:
        errors.append('cancellation arm requires a successful SP reference')
    if not sp and reference is not None:
        reference_result = check(reference, case, sp=True, allow_synthetic=allow_synthetic)
        errors += ['SP reference ' + error for error in reference_result['failures']]
        _, ref_actors, _, ref_failures = load_dump(reference / 'sp_trace.json.simdump.txt')
        ref_text = (reference / 'sp/runtime/LogConsole.txt').read_text(encoding='utf-8-sig')
        ref_obs, ref_observation_failures = load_observations(ref_text, ref_actors, True)
        errors += ref_failures + ref_observation_failures
        start, delay = (205, 0) if case in ('next', 'prev') else (196, 3)
        for peer in peers:
            for tick in range(start, HI + 1 - delay):
                target_tick = tick + delay
                expected_pie = ref_actors[1048587][tick]['pie']
                actual_pie = peer_actors[peer][1048736][target_tick]['pie']
                if actual_pie != expected_pie:
                    errors.append(f'{peer} cancellation/reference tick {target_tick} pie {actual_pie}, SP tick {tick} {expected_pie}')
                expected = ref_obs.get((1048587, tick))
                actual = observations.get(peer, {}).get((1048736, target_tick - 22))
                # Delivery's direct SP SetInputMode calls do not notify; committed frames do.
                if expected is not None and case == 'delivery_cancel':
                    expected = (*expected[:3], 2 if tick < 199 else 3)
                if expected != actual:
                    errors.append(f'{peer} cancellation/reference tick {target_tick} script {actual}, SP tick {tick} expected {expected}')
            reference_ticks[peer] = [start + delay, HI]
    return dict(pass_check=not errors, failures=errors, observed=observed,
                dump_sha256={p: hashlib.sha256(b).hexdigest() for p, b in dumps.items()},
                dump_bytes={p: len(b) for p, b in dumps.items()}, hash_ticks={p: len(t) for p, t in traces.items()},
                script_rows={p: len(rows) for p, rows in observations.items()},
                reference=reference_result, reference_ticks=reference_ticks,
                synthetic_fixture=manifest.get('synthetic') is True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('case', choices=['next', 'prev', 'goto', 'actor_cancel', 'delivery_cancel'])
    parser.add_argument('run', type=Path)
    parser.add_argument('--sp', action='store_true')
    parser.add_argument('--reference', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    try:
        result = check(args.run, args.case, args.sp, reference=args.reference)
    except (ValueError, KeyError, OSError, TypeError) as exc:
        result = dict(pass_check=False, failures=[str(exc)])
    args.out.write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(dict(pass_check=result['pass_check'], failure_count=len(result['failures']),
                          first_failures=result['failures'][:12], output=str(args.out)), indent=2))
    return 0 if result['pass_check'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
