"""Section 0b: declared native controls judged at the ordinary merged FINAL LIST.

This adapter calls the existing acceptance Share collector and merge_defects.
Expected RED products stay RED. A separate gate records whether all three
expected defects survived the complete pipeline before section 1 may start.
"""
from __future__ import annotations

import argparse
import copy
import importlib
import json
from pathlib import Path
import re
import subprocess
import sys

from acceptance_collection import Share, inventory, read, start
from acceptance_mod import sha256
from acceptance_runtime import storage_scope, write_json

CONTROLS = ('C1', 'C2', 'C3')
SHARE = 'X.controls'


def plan_rows():
    descriptions = dict(C1='one-side live perturbation; final divergence and tick required',
                        C2='client stall; final forced hold and fired stall tick required',
                        C3='passing native run-result.json removed before collection; final result absent required')
    return [dict(id='control.'+control, share=SHARE, section='0b', group='controls',
                 acceptance_ids=[], required=False,
                 reason='mandatory section 0b pipeline control; expected RED, no product acceptance credit',
                 control=control, expected=descriptions[control], runner='acceptance_pipeline_controls',
                 driver='tools/acceptance_control_pair.py', driver_box='EROL-PC', box='EROL-PC',
                 boxes=['EROL-PC'], engine_boxes={'EROL-PC': 2}, window_required=True,
                 product=dict(path=f'{SHARE}/control.{control}/run-result.json'),
                 identities=[dict(box='EROL-PC', path=f'{SHARE}/control.{control}/identity.json')],
                 collection=dict(path=SHARE+'/DEFECTS.json'), terminal=dict(path=SHARE+'/terminal.json'),
                 minutes=dict(earliest=1, likely=3, cap=5),
                 argv=['{PY}', '-B', '{CONTROL_HELPERS}/tools/acceptance_control_pair.py', '--control', control,
                       '--profile', '{CONTROL_PROFILE}', '--source-sha', '{CONTROL_SOURCE}',
                       '--exe-sha256', '{EXE}', '--collection-id', '{COLLECTION_ID}',
                       '--out', '{DIR}', '--evidence', '{CONTROL_EVIDENCE}/'+control]) for control in CONTROLS]


def augment(plan, schedule):
    """Return new declarations; never modify a caller's frozen plan in place."""
    plan, schedule = copy.deepcopy(plan), copy.deepcopy(schedule)
    if any(row.get('share') == SHARE or row.get('section') == '0b' for row in plan['rows']):
        raise ValueError('section 0b is already declared; never duplicate the controls')
    rows = plan_rows()
    plan['rows'] = rows+plan['rows']
    schedule['shares'].insert(0, dict(label=SHARE, box='EROL-PC', ids=[row['id'] for row in rows],
                                      whole=False, runner='acceptance_pipeline_controls', section='0b'))
    plan['controls'] = dict(section='0b', ids=[row['id'] for row in rows], required=3,
                            final_list='controls/LIST/DEFECTS-all.md',
                            gate='controls/gate.json', before_section=1)
    if 'sections' in plan:
        plan['sections'].insert(1, dict(id='0b', name='controls',
                                       rows=[[row['share'], row['id']] for row in rows]))
    schedule['scheduled'] = [share['label'] for share in schedule['shares']]
    return plan, schedule


def finish_required(share, command_id, code, log):
    """A missing declared product cannot be replaced by an incidental PASS file."""
    product = share.product(command_id)
    reason = 'result absent: '+product.relative_to(share.out).as_posix() if not product.is_file() else ''
    return share.finish(command_id, code, log, reason)


def remove_c3_result(share, evidence):
    """The exact deletion authorized by NOTE 15, after preserving passing proof."""
    command_id = 'control.C3'
    product = share.product(command_id)
    expected = (share.out/command_id/'run-result.json').resolve()
    if product.resolve() != expected or product.is_symlink() or not expected.is_relative_to(share.out):
        raise ValueError('C3 removal target is not the exact declared run-result.json')
    result = read(product)
    injection = read(product.parent/'injection.json')
    if (result.get('passed') is not True or result.get('collection_id') != share.run_id
            or result.get('source_sha') != share.source):
        raise ValueError('C3 must first pass natively in this collection')
    if (not injection.get('native_mode') or injection.get('collection_id') != share.run_id
            or injection.get('source_sha') != share.source
            or injection.get('executable_sha256') != share.schedule['exe_sha256']
            or set(injection.get('records', {})) != {'host', 'client'}
            or any(not row.get('pid') or row.get('exit_code') != 0 or row.get('timed_out')
                   for row in injection['records'].values())):
        raise ValueError('C3 native passing runner receipts are absent')
    evidence = Path(evidence).resolve()
    if evidence.is_relative_to(share.root):
        raise ValueError('the retained C3 PASS must remain outside the collection scan')
    evidence.mkdir(parents=True, exist_ok=False)
    preserved = evidence/'run-result.json'
    preserved.write_bytes(product.read_bytes())
    digest = sha256(product)
    if sha256(preserved) != digest:
        raise ValueError('C3 preservation differs; nothing removed')
    # Only this file, in this fresh control, is removed. No tree cleanup.
    product.unlink()
    receipt = dict(control='C3', collection_id=share.run_id, source_sha=share.source,
                   removed=str(expected), original_sha256=digest,
                   preserved=str(preserved), native_passed=True, result_absent=not product.exists())
    write_json(product.parent/'removed-result-receipt.json', receipt)
    return receipt


def collect_final(share, final_root, inventory_root):
    share.collect()
    inventory(inventory_root)
    merger = importlib.import_module('merge_defects')
    code = merger.main([str(share.out/'DEFECTS.json'), '--out', str(final_root)])
    return code


def judge_final(root, inventory_root):
    """Credit comes only from exact lines in the unchanged merger's final list."""
    root = Path(root).resolve()
    plan, schedule = read(root/'acceptance-plan.json'), read(root/'split-plan.json')
    if [row.get('control') for row in plan['rows'] if row.get('share') == SHARE] != list(CONTROLS):
        raise ValueError('the frozen plan lacks exactly the three declared controls')
    final_root = root/'controls/LIST'
    document = read(final_root/'DEFECTS-all.json')
    rendered = (final_root/'DEFECTS-all.md').read_text(encoding='utf-8')
    inventory(inventory_root)
    merger = importlib.import_module('merge_defects')
    if rendered != merger.markdown(document):
        raise ValueError('FINAL LIST differs from its collected JSON')
    results = []
    for control in CONTROLS:
        command_id = 'control.'+control
        native = root/SHARE/command_id
        problems, quoted = [], None
        injection = read(native/'injection.json')
        identity = read(native/'identity.json')
        if (identity.get('status') != 'PASS' or identity.get('collection_id') != schedule.get('collection_id')
                or identity.get('source_sha') != schedule.get('source_sha')
                or identity.get('executable_sha256') != schedule.get('exe_sha256')):
            problems.append('native build identity differs from the declared alpha')
        if (injection.get('collection_id') != schedule.get('collection_id')
                or injection.get('source_sha') != schedule.get('source_sha')
                or injection.get('executable_sha256') != schedule.get('exe_sha256')
                or injection.get('native_mode') is not True
                or set(injection.get('records', {})) != {'host', 'client'}
                or set(injection.get('log_sha256', {})) != {'host', 'client'}
                or any(type(row.get('pid')) is not int or row['pid'] <= 0 or row.get('timed_out')
                       or type(row.get('exit_code')) is not int for row in injection['records'].values())):
            problems.append('native launch/identity receipt absent or from another collection')
        for who, digest in injection.get('log_sha256', {}).items():
            if who not in ('host', 'client') or sha256(native/who/'stdout.log') != digest:
                problems.append('native injection log changed before final-list verification')
        hits = [row for row in document.get('defects', []) if row.get('kind') != 'soft'
                and command_id in str(row.get('path', '')).replace('\\', '/').split('/')]
        if control == 'C1':
            fired = injection.get('perturb', {}).get('host', [])
            if len(fired) != 1 or injection.get('perturb', {}).get('client'):
                problems.append('live perturbation did not fire exactly once on one side')
            matches = [row for row in hits if (match := re.search(
                r'(?:divergence at tick=|first_divergence=|\[lockstep\] desync at frame )(\d+)', row.get('quoted_line', '')))
                and len(fired) == 1 and int(match[1]) == fired[0]]
        elif control == 'C2':
            fired = injection.get('stall', {}).get('client', [])
            holds = injection.get('holds', {}).get('host', [])
            if len(fired) != 1 or not holds:
                problems.append('silent native stall/hold receipt')
            matches = [row for row in hits if len(fired) == 1 and any(
                f'forced hold at tick={hold["tick"]}; stall tick={fired[0]["tick"]}' in row.get('quoted_line', '')
                for hold in holds)]
        else:
            removal = read(native/'removed-result-receipt.json')
            original = read(Path(removal['preserved']))
            if (removal.get('collection_id') != schedule.get('collection_id')
                    or removal.get('native_passed') is not True or (native/'run-result.json').exists()
                    or original.get('passed') is not True
                    or original.get('collection_id') != schedule.get('collection_id')
                    or original.get('source_sha') != schedule.get('source_sha')
                    or sha256(Path(removal['preserved'])) != removal.get('original_sha256')):
                problems.append('C3 passing original/removal proof differs')
            matches = [row for row in hits if 'result absent' in row.get('quoted_line', '')]
        if document.get('status') != 'RED' or not matches:
            problems.append('expected failure or its tick/detail lost before FINAL LIST')
        else:
            prefix = '- '+matches[0]['id']+' '
            quoted = next((line for line in rendered.splitlines() if line.startswith(prefix)), None)
            if not quoted:
                problems.append('defect has no exact FINAL LIST line')
        results.append(dict(control=control, passed=not problems, final_line=quoted, errors=problems))
    return dict(section='0b', collection_id=schedule['collection_id'], source_sha=schedule['source_sha'],
                passed=all(row['passed'] for row in results), controls=results,
                final_list=str(final_root/'DEFECTS-all.md'), final_sha256=sha256(final_root/'DEFECTS-all.md'),
                collection_sha256=sha256(root/SHARE/'DEFECTS.json'))


def require_gate(root, inventory_root):
    """Revalidate the retained gate before section 1 and before final credit."""
    result = judge_final(root, inventory_root)
    stored = read(Path(root)/'controls/gate.json')
    if result != stored or result.get('passed') is not True:
        raise ValueError('section 0b is not 3/3 at the unchanged FINAL LIST; section 1/acceptance credit refused')
    return result


def run_controls(root, inventory_root, profile, evidence):
    root, evidence = Path(root).resolve(), Path(evidence).resolve()
    import acceptance_collection as collection
    if not hasattr(collection, 'start_section'):
        raise ValueError('native section 0b requires the frozen section-receipt collector')
    schedule = read(root/'split-plan.json')
    if '0b' not in schedule.get('section_receipts', {}):
        choice = collection.start_section(root, '0b', inventory_root=inventory_root)
    else:
        choice = read(root/schedule['section_receipts']['0b']['path'])
    if any(row['state'] != 'READY' for row in choice['rows']):
        raise RuntimeError('section 0b PC window is not authorized; no native control launched')
    share = Share(root, SHARE, inventory_root)
    rows = [row for row in share.plan['rows'] if row['share'] == SHARE]
    if rows != plan_rows():
        raise ValueError('control plan differs from this committed driver declaration')
    evidence.mkdir(parents=True, exist_ok=False)
    for name in ('acceptance-plan.json', 'split-plan.json'):
        (evidence/name).write_bytes((root/name).read_bytes())
        if sha256(evidence/name) != sha256(root/name):
            raise ValueError('prelaunch frozen plan preservation differs')
    # Bind the exact collector/merger used in this run before native work starts.
    write_json(evidence/'pipeline-sources.json', {
        str(path): sha256(path) for path in
        [Path(__file__), Path(__file__).with_name('acceptance_control_pair.py'),
         Path(__file__).with_name('acceptance_collection.py'),
         *[Path(inventory_root)/name for name in ('extract_defects.py', 'merge_defects.py', 'acceptance_manifest.py')]]})
    for spec in rows:
        control, command_id = spec['control'], spec['id']
        values = dict(PY=sys.executable, CONTROL_HELPERS=Path(__file__).resolve().parent.parent.as_posix(),
                      CONTROL_PROFILE=str(Path(profile).resolve()), CONTROL_SOURCE=share.source,
                      EXE=share.schedule['exe_sha256'], COLLECTION_ID=share.run_id,
                      DIR=str(root/SHARE/command_id), CONTROL_EVIDENCE=str(evidence))
        command = [re.sub(r'\{([A-Z_]+)\}', lambda match: values[match[1]], word) for word in spec['argv']]
        share.begin(command_id, command)
        log = share.receipt_dir(command_id)/'driver-stdout.log'
        with log.open('x', encoding='utf-8') as stream:
            code = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT).returncode
        if control == 'C3' and code == 0:
            remove_c3_result(share, evidence/'C3-removed-result')
        finish_required(share, command_id, code, log)
    collect_final(share, root/'controls/LIST', inventory_root)
    try:
        gate = judge_final(root, inventory_root)
    except (OSError, ValueError, KeyError, TypeError) as error:
        gate = dict(section='0b', passed=False, errors=[str(error)], controls=[])
    write_json(root/'controls/gate.json', gate)
    for path in [root/SHARE/'DEFECTS.json', root/SHARE/'terminal.json',
                 root/'controls/gate.json', root/'controls/LIST/DEFECTS-all.json', root/'controls/LIST/DEFECTS-all.md']:
        destination = evidence/'collected'/path.relative_to(root)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(path.read_bytes())
        if sha256(destination) != sha256(path):
            raise ValueError('control final-list preservation differs')
    for row in gate['controls']:
        print(row['control']+': '+('PASS' if row['passed'] else 'FAIL')+' '+str(row['final_line']))
    if not gate['passed']:
        print('STOP section 1: section 0b pipeline control failed')
    return 0 if gate['passed'] else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('plan', 'run', 'judge', 'gate'))
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--inventory', type=Path, required=True)
    parser.add_argument('--profile', type=Path)
    parser.add_argument('--evidence', type=Path)
    parser.add_argument('--source-sha')
    parser.add_argument('--exe-sha256')
    parser.add_argument('--sequence-ledger', type=Path)
    options = parser.parse_args(argv)
    profile = read(options.profile) if options.profile else None
    if options.action in ('judge', 'gate'):
        result = require_gate(options.root, options.inventory) if options.action == 'gate' else judge_final(options.root, options.inventory)
        print(json.dumps(result, indent=2))
        return 0 if result['passed'] else 1
    if profile is None:
        parser.error('plan/run require a native box profile')
    with storage_scope(Path(profile['scratch']), reserve=256*1024**2):
        if options.action == 'plan':
            if not options.source_sha or not options.exe_sha256 or not options.sequence_ledger:
                parser.error('plan requires the published alpha hashes and a sequence ledger')
            if options.root.exists():
                parser.error('use a fresh control run root')
            plan, schedule = augment(dict(generated=dict(head=options.source_sha), schedule='split-plan.json', rows=[]),
                                     dict(shares=[], source_sha=options.source_sha))
            options.root.mkdir(parents=True)
            write_json(options.root/'declared-plan.json', plan)
            write_json(options.root/'declared-schedule.json', schedule)
            start(options.root, options.root/'declared-plan.json', options.root/'declared-schedule.json',
                  options.source_sha, options.exe_sha256, options.sequence_ledger, options.inventory)
            return 0
        if not options.evidence:
            parser.error('run requires the retained small-evidence directory')
        return run_controls(options.root, options.inventory, options.profile, options.evidence)


if __name__ == '__main__':
    raise SystemExit(main())
