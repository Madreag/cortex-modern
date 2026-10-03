"""Account for measured instrumentation; never estimate or subtract missing costs."""
from collections import defaultdict
import json
import math
from pathlib import Path
import re

INSTRUMENTS = ('sim_dump', 'tick_end', 'fullstate', 'census', 'preview_fidelity',
               'stall_sampler', 'screen_watches', 'recorder')
FRAME_BUDGET_MS = 50


def number(value):
    return type(value) in (int, float) and math.isfinite(value) and value >= 0


def reduce_costs(paths, *, first_frame=None, last_frame=None):
    samples = {name: [] for name in INSTRUMENTS}
    scopes, frames, errors = [], [], []
    def sample(name, path, line, **values):
        samples[name].append(dict(path=str(path), line=line, **values))
    for path in map(Path, paths):
        if not path.is_file():
            errors.append(f'{path}: log absent'); continue
        for line_number, line in enumerate(path.read_text(encoding='utf-8-sig', errors='replace').splitlines(), 1):
            for tag, collection in (('[harness-cost-scope] ', scopes), ('[harness-cost-frame] ', frames)):
                if line.startswith(tag):
                    try:
                        row = json.loads(line[len(tag):])
                        if not isinstance(row, dict): raise ValueError('object absent')
                        collection.append(dict(row, path=str(path), line=line_number))
                    except ValueError as error:
                        errors.append(f'{path}:{line_number}: {tag.strip()} {error}')
            if found := re.search(r'\[sim-dump\].*?(?:max_ms|\bms)=([0-9.eE+-]+)', line):
                sample('sim_dump', path, line_number, max_ms=float(found[1]))
            if found := re.search(r'\[harness-cost\] tick=(\d+) window=(\d+) tick_end_us p50=\S+ p95=\S+ max=([0-9.eE+-]+)', line):
                sample('tick_end', path, line_number, tick=int(found[1]), ticks=int(found[2]), max_ms=float(found[3])/1000)
            if found := re.search(r'\[fullstate-cost\] tick=(\d+) freeze_us=(\d+) hash_us=(\d+)', line):
                sample('fullstate', path, line_number, tick=int(found[1]), freeze_ms=int(found[2])/1000,
                       hash_ms=int(found[3])/1000, max_ms=max(int(found[2]), int(found[3]))/1000)
            if '[mem-census]' in line and 'census_us=' in line:
                parts = {name: float(value)/1000 for name, value in re.findall(r'([a-zA-Z_]+):([0-9.eE+-]+)', line.split('census_us=', 1)[1])}
                sample('census', path, line_number, components_ms=parts, total_ms=sum(parts.values()), max_ms=max(parts.values(), default=0))
            if found := re.search(r'\bharness_ms_total=([0-9.eE+-]+)', line):
                sample('preview_fidelity', path, line_number, cumulative_total_ms=float(found[1]))
    def key(row):
        return (row.get('path'), row.get('process'), row.get('incarnation'), row.get('round'))
    grouped = defaultdict(list)
    for row in frames: grouped[key(row)].append(row)
    covered, complete_scopes, used = defaultdict(list), [], set()
    aggregate = []
    for scope in scopes:
        identity = key(scope)
        start, end, enabled = scope.get('first_frame'), scope.get('last_frame'), scope.get('instruments')
        valid = (scope.get('version') == 1 and type(scope.get('process')) is int and scope['process'] > 0
                 and type(scope.get('incarnation')) is int and type(scope.get('round')) is int
                 and type(start) is int and type(end) is int and 0 <= start <= end
                 and isinstance(enabled, dict) and set(enabled) == set(INSTRUMENTS)
                 and all(type(value) is bool for value in enabled.values()))
        if not valid or identity in used:
            errors.append(f'{scope["path"]}:{scope["line"]}: invalid or duplicate instrumentation scope'); continue
        used.add(identity)
        if first_frame is not None and start > first_frame or last_frame is not None and end < last_frame:
            errors.append(f'{identity}: cost coverage {start}..{end}, requested {first_frame}..{last_frame}')
            valid = False
        by_frame = {}
        for row in grouped[identity]:
            frame, costs = row.get('frame'), row.get('costs_ms')
            expected = {name for name, active in enabled.items() if active}
            measured = (type(frame) is int and start <= frame <= end and frame not in by_frame
                        and row.get('partition_valid') is True and isinstance(costs, dict) and set(costs) == expected
                        and all(number(v) for v in costs.values()))
            if not measured:
                errors.append(f'{row["path"]}:{row["line"]}: invalid, duplicated or unpartitioned frame costs'); valid = False; continue
            by_frame[frame] = row
            aggregate.append(dict(process=scope['process'], incarnation=scope['incarnation'], round=scope['round'],
                                  frame=frame, total_ms=sum(costs.values()), path=row['path'], line=row['line']))
            for name, value in costs.items():
                covered[name].append(dict(frame=frame, ms=value, path=row['path'], line=row['line']))
        if len(by_frame) != end - start + 1:
            errors.append(f'{identity}: cost frames={len(by_frame)}, declared={end-start+1}'); valid = False
        for name, active in enabled.items():
            if not active and any(row['path'] == scope['path'] for row in samples[name]):
                errors.append(f'{identity}: {name} declared disabled but native cost observations exist'); valid = False
        complete_scopes.append(valid)
    if set(grouped) - used: errors.append('cost frames have no owning instrumentation scope')
    instruments = {}
    for name in INSTRUMENTS:
        declarations = [s.get('instruments', {}).get(name) for s in scopes if isinstance(s.get('instruments'), dict)]
        known = bool(declarations) and all(type(v) is bool for v in declarations)
        disabled = known and not any(declarations)
        values = [r['ms'] for r in covered[name]]
        observed_max = max([r.get('max_ms', 0) for r in samples[name]] + values, default=None)
        if observed_max is not None and not number(observed_max): errors.append(f'{name}: invalid measured cost {observed_max}')
        instruments[name] = dict(status='DISABLED' if disabled else 'MEASURED' if values and all(complete_scopes) else 'MISSING COST',
            scope_known=known, complete=disabled or bool(values) and all(complete_scopes),
            measured_total_ms=sum(values) if values else None, measured_max_ms=observed_max,
            native_samples=samples[name], frame_samples=len(values))
    maximum = max([r['total_ms'] for r in aggregate] + [r['measured_max_ms'] for r in instruments.values()
                  if r['measured_max_ms'] is not None and number(r['measured_max_ms'])], default=None)
    complete = bool(scopes) and bool(complete_scopes) and all(complete_scopes) and not errors and all(r['complete'] for r in instruments.values())
    over = maximum is not None and maximum > FRAME_BUDGET_MS
    status = 'FAIL' if errors or over else 'PASS' if complete else 'INCOMPLETE'
    missing = [name for name, row in instruments.items() if not row['complete']]
    reason = '; '.join([*errors, *([f'measured peak={maximum} ms'] if over else []), *([f'missing cost coverage: {missing}'] if missing else [])])
    return dict(status=status, passed=status == 'PASS', complete=complete, instrument_valid=status == 'PASS',
                max_frame_ms=maximum, measured_total_ms=sum(row['total_ms'] for row in aggregate) if aggregate else None,
                instruments=instruments, scope_receipts=scopes, measured_frames=len(aggregate), reason=reason,
                rule='Every enabled instrument has complete measured cost coverage; aggregate frame cost <= 50 ms',
                subtraction_ms=0, accounting='Only native partitioned frame costs are added. Summary-only costs remain partial; cumulative preview totals are not summed across snapshots.')
