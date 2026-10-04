"""NOTE 14 permits PC compiler overlap for the two functional mod rows only."""
from __future__ import annotations

import datetime as dt
import json
import os
from pathlib import Path
import secrets
import time

import cross_peers as cross
from acceptance_runtime import write_json

COMPILERS = {'cl.exe', 'link.exe', 'msbuild.exe'}


def compiler_overlap_allowed(box):
    return (box.get('name') == 'EROL-PC' and box.get('kind') == 'windows-local' and
            box.get('compiler_overlap_row') in ('mod-match', 'mod-refusal'))


def blocking_load(box, measured):
    if not compiler_overlap_allowed(box):
        return list(measured)
    return [row for row in measured if str(row.get('Name', '')).casefold() not in COMPILERS]


def acquire_reservation(box, root, timeout):
    if not compiler_overlap_allowed(box):
        return cross.acquire_reservation(box, root, timeout)
    marker = Path(box['exclusive_marker'])
    previous = os.environ.get('CCCP_FEEL_MATRIX_RUN')
    if cross.owns_reservation(box):
        return dict(marker=str(marker), record=json.loads(marker.read_text()), previous=previous, borrowed=True)
    deadline = time.monotonic()+timeout
    while time.monotonic() < deadline:
        measured = cross.box_load()
        if marker.exists() or blocking_load(box, measured):
            time.sleep(.5)
            continue
        cross.assert_box_guard(box)
        value = dict(stream_root=str(root), pid=os.getpid(), token=secrets.token_hex(24),
                     stamp=dt.datetime.now(cross.MST).strftime('%Y-%m-%d %I:%M:%S %p MST'))
        try:
            with marker.open('x', encoding='utf-8') as output:
                json.dump(value, output)
        except FileExistsError:
            continue
        os.environ['CCCP_FEEL_MATRIX_RUN'] = value['token']
        claim = dict(marker=str(marker), record=value, previous=previous)
        if not cross.owns_reservation(box):
            cross.release_reservation(claim)
            raise RuntimeError('physical reservation changed before native launch')
        fresh = cross.box_load()
        if blocking_load(box, fresh):
            cross.release_reservation(claim)
            time.sleep(.5)
            continue
        write_json(Path(root)/'compiler-overlap-policy.json', dict(
            authorization='LEAD-NOTES NOTE 14', row=box['compiler_overlap_row'], measured_load=fresh,
            engine_affinity_mask=box['affinity_mask'], ignored_names=sorted(COMPILERS)))
        return claim
    raise TimeoutError('PC engine or reservation is occupied; no native launch forced')


def apply_report_policy(inputs):
    manifest = inputs['manifest']
    if manifest.get('acceptance_row') not in ('mod-match', 'mod-refusal'):
        return inputs
    for box in manifest['boxes']:
        if not compiler_overlap_allowed(box) or box['compiler_overlap_row'] != manifest['acceptance_row']:
            continue
        pre = manifest.get('preflights', {}).get(box['name'], {})
        for peer in inputs['peers'].values():
            if peer['box'] != box['name']:
                continue
            samples = peer['samples']
            raw = [*pre.get('load', []), *[entry for sample in samples for entry in sample.get('load', [])]]
            admitted = bool(samples) and not blocking_load(box, raw) and all(sample.get('same_box_instances', 1) == 1 for sample in samples)
            peer['compiler_overlap_policy'] = dict(authorization='LEAD-NOTES NOTE 14', admitted=admitted,
                                                   measured_load=raw, timing_gates_unchanged=True)
            peer['feel_gated'] = bool(manifest.get('quiet_window')) and admitted
            peer['feel_status'] = ('PASS' if peer['feel_pass'] else 'FAIL') if peer['feel_gated'] else 'UNDER LOAD'
    return inputs
