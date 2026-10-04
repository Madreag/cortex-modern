"""Ordinary strict pair verdicts and native receipts for the section 0b controls.

The expected failure is judged by the downstream FINAL LIST. This reducer never
turns a planted divergence or hold into a passing product verdict.
"""
from __future__ import annotations

import re
import argparse
import json
import os
from pathlib import Path
import platform
import sys
import time

from acceptance_runtime import storage_scope, write_json
from acceptance_mod import sha256


PERTURB = re.compile(r'^\[net-test\] live perturb frame=(\d+)$', re.M)
STALL = re.compile(r'^\[net-test\] live stall frame=(\d+) ms=(\d+)\b', re.M)
HOLD = re.compile(r'^\[net-match\] hold peer=(\d+) frame=(\d+) AI in control', re.M)
DESYNC = re.compile(r'\[lockstep\] desync at frame (\d+)')


def ordinary_verdict(pair_result, texts):
    """All three controls use exactly the same unexcused pair policy."""
    receipt = injection_receipt(texts)
    reasons, divergent = [], []
    for value in (pair_result.get('simulation', {}).get('first_divergence'),
                  pair_result.get('objects', {}).get('tick'),
                  pair_result.get('fullstate', {}).get('first_divergence')):
        if type(value) is int and value > 0:
            divergent.append(value)
    divergent += [int(t) for text in texts.values() for t in DESYNC.findall(text)]
    if divergent:
        reasons.append('divergence at tick='+str(min(divergent)))
    holds = [hold for values in receipt['holds'].values() for hold in values]
    stalls = [stall for values in receipt['stall'].values() for stall in values]
    if holds:
        forced, unscheduled = [], []
        for tick in sorted({hold['tick'] for hold in holds}):
            fired = [stall['tick'] for stall in stalls if stall['tick'] <= tick]
            if fired:
                forced.append(f'forced hold at tick={tick}; stall tick={max(fired)}')
            else:
                unscheduled.append(f'unscheduled hold at tick={tick}')
        # Preserve the planted fault even when an earlier ordinary hold exists.
        # Every raw hold remains in the native pair and injection receipts.
        reasons += forced[:1]+unscheduled[:1]
    elif pair_result.get('held', {}).get('held') or any(pair_result.get('hold_lines', {}).values()):
        reasons.append('hold reported without its native seat/tick receipt')
    if pair_result.get('passed') is not True and not reasons:
        reasons.append('ordinary pair failed; see simulation, objects, fixture and process evidence')
    # Put the concise failure first: the unchanged extractor truncates long quotes.
    return dict(reason='; '.join(reasons), passed=pair_result.get('passed') is True and not reasons,
                first_divergence=min(divergent) if divergent else None, pair=pair_result)


def injection_receipt(texts):
    return dict(perturb={who: [int(t) for t in PERTURB.findall(text)] for who, text in texts.items()},
                stall={who: [dict(tick=int(t), milliseconds=int(ms)) for t, ms in STALL.findall(text)]
                       for who, text in texts.items()},
                holds={who: [dict(seat=int(seat), tick=int(t)) for seat, t in HOLD.findall(text)]
                       for who, text in texts.items()})


def flags(control, who, root, port):
    import test_determinism_pair as pair
    case = pair.CASES['fence']
    args = pair.peer_args(case, who, port, root, 60, '240:5000' if control == 'C2' else '')
    if control == 'C1' and who == 'host':
        args += ['-net-test-perturb-when-live', '-determinism-selftest-perturb',
                 '-determinism-selftest-perturb-tick', '240']
    return args


def validate_box(box, source_sha, exe_sha256):
    if (sys.platform != 'win32' or box.get('name') != 'EROL-PC' or box.get('kind') != 'windows-local'
            or box.get('hostname', '').casefold() != platform.node().casefold()
            or box.get('compiler_overlap_row') or box.get('max_engines') != 4
            or box.get('affinity_mask') != '0x0000FFFF' or box.get('engine_memory_gb') != 12
            or box.get('launch_floor_gib') != 10
            or box.get('exclusive_marker') != 'D:/mx/FEEL-MATRIX-RUNNING'
            or box.get('guard_file') != 'D:/mx/BOX-FREE-FOR-CROSS'):
        raise ValueError(f'control pair box {box.get("name")!r} on {platform.node()!r} differs from the authorized PC identity, '
                         'reservation or native limits')
    exe = Path(box['executable']).resolve()
    approved = {Path('D:/Projects/takeover-build/Cortex Command.exe').resolve(),
                Path('D:/Projects/fencing-warm/Cortex Command.exe').resolve()}
    if exe not in approved or exe.parent != Path(box['tree']).resolve():
        raise ValueError('control executable is outside the approved ruled engine trees')
    if not re.fullmatch('[0-9a-f]{40}', source_sha) or not re.fullmatch('[0-9a-f]{64}', exe_sha256):
        raise ValueError('the published alpha must be pinned by full source and executable hashes')
    build = json.loads(Path(box['build_receipt']).read_text(encoding='utf-8-sig'))
    if build.get('commit') != source_sha or build.get('executable_sha256') != exe_sha256 or sha256(exe) != exe_sha256:
        raise ValueError('control engine differs from the published alpha build receipt')
    lead = build.get('lead_build_receipt', {})
    ordinary = (build.get('configuration') == 'Final' and build.get('build_exit_code') == 0
                and build.get('build_log') and Path(build['build_log']).is_file()
                and sha256(Path(build['build_log'])) == build.get('build_log_sha256'))
    published = (lead.get('head_full') == source_sha and lead.get('configuration') == 'Final'
                 and lead.get('dirty_files') == 0 and lead.get('exe_sha256') == exe_sha256)
    if not (ordinary or published):
        raise ValueError('control engine has no successful Final build receipt')


def guard_pair(box, runs, completed):
    import cross_peers as cross
    own = [cross.engine_pid(run) for who, run in runs.items() if who not in completed]
    if load := cross.box_load([pid for pid in own if pid]):
        raise RuntimeError('foreign native workload appeared during the control pair: '+cross.describe_load(load))
    if not cross.owns_reservation(box):
        raise RuntimeError('control pair lost its physical reservation')
    if reason := cross.inventory_guard():
        raise RuntimeError(reason)
    # Account only for our verified engine PIDs, as the spectator adapter does.
    cross.assert_box_guard({**box, 'kind': 'windows-task'} if own else box)


def native_pair(control, box, source_sha, exe_sha256, root, evidence, collection_id):
    import cross_peers as cross
    import test_determinism_pair as pair
    from acceptance_frozen_tools import receipt
    from acceptance_identity import identity
    from feel.launch_budget import install_memory_guard
    from run_sim_test import make_run, seed_settings

    validate_box(box, source_sha, exe_sha256)
    bundle = receipt(Path(__file__).resolve().parent.parent)
    if bundle is None:
        raise ValueError('native controls require the authorized immutable tool bundle')
    root, evidence = Path(root).resolve(), Path(evidence).resolve()
    scratch = Path(box['scratch']).resolve()
    if not root.is_relative_to(scratch) or root.exists() or evidence.is_relative_to(scratch):
        raise ValueError('use a fresh lane scratch root and an evidence directory outside scratch')
    install_memory_guard()
    variables = dict(CCCP_HEADLESS='1', CC_RUNNER_IGNORE_FULLSCREEN='1', CC_RUNNER_BOX_NAME='EROL-PC',
                     CC_RUNNER_MIN_FREE_GB='10', CC_RUNNER_AFFINITY_MASK='0x0000FFFF',
                     CC_RUNNER_JOB_MEMORY_GB='12', PYTHONDONTWRITEBYTECODE='1')
    previous = {key: os.environ.get(key) for key in variables}
    os.environ.update(variables)
    claim, runs, completed, errors = None, {}, {}, []
    try:
        with storage_scope(scratch, reserve=256*1024**2):
            cross.assert_box_guard(box)
            if cross.scratch_bytes(scratch)+256*1024**2 >= cross.LIMIT:
                raise RuntimeError('control pair cannot admit its native output under the cross storage limit')
            root.mkdir(parents=True, exist_ok=False)
            claim = cross.acquire_reservation(box, root, 45)
            if claim.get('borrowed'):
                raise RuntimeError('standalone controls must own their reservation')
            write_json(root/'reservation.json', {k: v for k, v in claim['record'].items() if k != 'token'})
            write_json(root/'payload.json', dict(box=box))
            cross.preflight_payload(root/'payload.json')
            preflight = json.loads((root/'preflight.json').read_text())
            measured = identity('EROL-PC', Path(box['tree']), Path(box['executable']),
                                Path(box['build_receipt']), source_sha, collection_id)
            if measured['status'] != 'PASS' or preflight['load'] or preflight['executable_sha256'] != exe_sha256:
                raise RuntimeError('native control preflight identity/load differs')
            write_json(root/'identity.json', measured)
            evidence.mkdir(parents=True, exist_ok=False)
            # The small identity and plan are preserved before the first engine.
            for name in ('preflight.json', 'identity.json', 'payload.json'):
                (evidence/name).write_bytes((root/name).read_bytes())
                if sha256(evidence/name) != sha256(root/name):
                    raise RuntimeError('control prelaunch evidence copy differs')
            next_check = 0
            for who in pair.PEERS:
                guard_pair(box, runs, completed)
                run = make_run(Path(box['tree']), flags(control, who, root, 49720), root/who, 180,
                               env={**variables, 'CC_SIM_DUMP': '1:600'})
                pair.stage_module(run.cwd, pair.CASES['fence'])
                seed_settings(run, dict(NetworkPortMapEnable='0', Fullscreen='0'))
                guard_pair(box, runs, completed)
                if sha256(Path(run.argv[0])) != exe_sha256:
                    raise RuntimeError('control executable changed immediately before launch')
                runs[who] = run
                run.start()
                if who == 'host':
                    time.sleep(2)
            while len(completed) < 2:
                if time.monotonic() >= next_check:
                    guard_pair(box, runs, completed)
                    next_check = time.monotonic()+2
                for who, run in runs.items():
                    if who not in completed and run.poll() is not None:
                        completed[who] = run.finish()
                if (root/'stop.json').exists():
                    raise RuntimeError('control cancelled by its coordinator')
                time.sleep(.1)
    except Exception as error:
        errors.append(str(error))
    finally:
        for who, run in runs.items():
            run.close()
            completed.setdefault(who, run.record)
        if claim and not claim.get('borrowed'):
            write_json(root/'reservation-released.json', dict(released=cross.release_reservation(claim)))
        for key, old in previous.items():
            if old is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = old
    if not root.is_dir():
        raise RuntimeError('; '.join(errors))
    texts = {who: (root/who/'stdout.log').read_text(encoding='utf-8', errors='replace')
             if (root/who/'stdout.log').is_file() else '' for who in pair.PEERS}
    try:
        result = ordinary_verdict(pair.score(root, 'fence', exe_sha256, completed, 60), texts)
    except Exception as error:
        result = dict(passed=False, reason='ordinary pair collection failed: '+str(error))
    if errors:
        result.update(passed=False, reason='; '.join(errors), driver_errors=errors)
    unchanged = sha256(Path(box['executable'])) == exe_sha256
    if not unchanged:
        result['reason'] = 'native executable changed during the control; '+result['reason']
    result.update(control=control, case='control.'+control, source_sha=source_sha, executable_sha256=exe_sha256,
                  collection_id=collection_id, binary_unchanged=unchanged)
    result['passed'] = result['passed'] and unchanged
    with storage_scope(scratch, reserve=1024**2):
        write_json(root/'run-result.json', result)
        write_json(root/'injection.json', dict(control=control, collection_id=collection_id,
            source_sha=source_sha, executable_sha256=exe_sha256, **injection_receipt(texts),
            log_sha256={who: sha256(root/who/'stdout.log') for who in pair.PEERS
                        if (root/who/'stdout.log').is_file()},
            records={who: {key: value.get(key) for key in ('pid', 'exit_code', 'timed_out')}
                     for who, value in completed.items()}, native_mode=True))
    print(('PASS' if result['passed'] else 'FAIL')+' '+control+' '+result['reason'])
    return 0 if result['passed'] else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--control', required=True, choices=('C1', 'C2', 'C3'))
    parser.add_argument('--profile', type=Path, required=True)
    parser.add_argument('--source-sha', required=True)
    parser.add_argument('--exe-sha256', required=True)
    parser.add_argument('--collection-id', required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    options = parser.parse_args(argv)
    return native_pair(options.control, json.loads(options.profile.read_text(encoding='utf-8-sig')),
                       options.source_sha, options.exe_sha256, options.out, options.evidence, options.collection_id)


if __name__ == '__main__':
    raise SystemExit(main())
