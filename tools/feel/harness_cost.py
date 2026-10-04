"""Account for measured instrumentation; never estimate or subtract missing costs."""
from collections import defaultdict
import json
import math
from pathlib import Path
import re

INSTRUMENTS = ('sim_dump', 'tick_end', 'fullstate', 'census', 'preview_fidelity',
               'stall_sampler', 'screen_watches', 'recorder', 'controller_trace', 'feel_recorder')
# The instruments each receipt version names (HarnessCost::c_ReceiptVersion); version 1 had no controller trace or feel recorder.
VERSION_INSTRUMENTS = {1: INSTRUMENTS[:8], 2: INSTRUMENTS}
FRAME_BUDGET_MS = 50
# The menu loop's stays are numbered from this bit up (Main.cpp c_HarnessMenuRounds); no match round reaches it.
MENU_ROUNDS = 1 << 62
TAGS = ('[harness-cost-scope] ', '[harness-cost-scope-open] ', '[harness-cost-outside] ', '[harness-cost-frame] ')


def number(value):
    return type(value) in (int, float) and math.isfinite(value) and value >= 0


def key(row):
    return (row.get('path'), row.get('process'), row.get('incarnation'), row.get('round'), row.get('segment', 0))


def scope_valid(scope, closed):
    start, end, enabled = scope.get('first_frame'), scope.get('last_frame'), scope.get('instruments')
    return (scope.get('version') in VERSION_INSTRUMENTS and type(scope.get('process')) is int and scope['process'] > 0
            and type(scope.get('incarnation')) is int and type(scope.get('round')) is int and type(scope.get('segment', 0)) is int
            and type(start) is int and (type(end) is int and 0 <= start <= end if closed else 0 <= start)
            and isinstance(enabled, dict) and set(enabled) == set(VERSION_INSTRUMENTS[scope['version']])
            and all(type(value) is bool for value in enabled.values()))


def window_coverage(scopes, first_frame, last_frame):
    """Whether one match round's frames, over all its segments, cover the requested window; the best coverage otherwise."""
    rounds = defaultdict(set)
    for scope in scopes:
        if scope['valid'] and scope['round'] < MENU_ROUNDS:
            rounds[scope['path'], scope['process'], scope['incarnation'], scope['round']].update(scope['frames'])
    wanted = set(range(first_frame if first_frame is not None else 0, (last_frame if last_frame is not None else -1) + 1))
    if first_frame is None or last_frame is None:
        wanted = None
    best = None
    for identity, frames in rounds.items():
        lowest, highest = (min(frames), max(frames)) if frames else (None, None)
        if wanted is None:
            if (first_frame is None or lowest is not None and lowest <= first_frame) and (last_frame is None or highest is not None and highest >= last_frame):
                return True, None
        elif wanted <= frames:
            return True, None
        missing = len(wanted - frames) if wanted is not None else None
        if best is None or (missing is not None and missing < best[2]):
            best = (identity, (lowest, highest), missing if missing is not None else 0)
    return False, best


def reduce_costs(paths, *, first_frame=None, last_frame=None):
    samples = {name: [] for name in INSTRUMENTS}
    receipts = {tag: [] for tag in TAGS}
    errors = []
    def sample(name, path, line, **values):
        samples[name].append(dict(path=str(path), line=line, **values))
    for path in map(Path, paths):
        if not path.is_file():
            errors.append(f'{path}: log absent'); continue
        for line_number, line in enumerate(path.read_text(encoding='utf-8-sig', errors='replace').splitlines(), 1):
            for tag in TAGS:
                if line.startswith(tag):
                    try:
                        row = json.loads(line[len(tag):])
                        if not isinstance(row, dict): raise ValueError('object absent')
                        receipts[tag].append(dict(row, path=str(path), line=line_number))
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
                sample('census', path, line_number, components_ms=parts, total_ms=sum(parts.values()), max_ms=sum(parts.values()))
            if found := re.search(r'\bharness_ms_total=([0-9.eE+-]+)', line):
                sample('preview_fidelity', path, line_number, cumulative_total_ms=float(found[1]))
    closed, opened, outside, frames = (receipts[tag] for tag in TAGS)
    grouped = defaultdict(list)
    for row in frames: grouped[key(row)].append(row)
    # A scope is declared as it opens and receipted again as it closes; a process ended before its close keeps the open
    # receipt, and its frames end with the last one it wrote.
    declared = {}
    for state, rows in (('closed', closed), ('open', opened)):
        for scope in rows:
            identity = key(scope)
            if not scope_valid(scope, state == 'closed') or (identity, state) in declared:
                errors.append(f'{scope["path"]}:{scope["line"]}: invalid or duplicate instrumentation scope'); continue
            declared[identity, state] = scope
    scopes = []
    for identity in dict.fromkeys(identity for identity, _ in declared):
        close, open_ = declared.get((identity, 'closed')), declared.get((identity, 'open'))
        if close and open_ and (close['first_frame'] != open_['first_frame'] or close['instruments'] != open_['instruments']):
            errors.append(f'{close["path"]}:{close["line"]}: closing scope disagrees with its opening receipt at line {open_["line"]}')
        receipt = close or open_
        last = close['last_frame'] if close else max((row.get('frame') for row in grouped[identity] if type(row.get('frame')) is int), default=open_['first_frame'] - 1)
        scopes.append(dict(path=identity[0], process=identity[1], incarnation=identity[2], round=identity[3], segment=identity[4],
                           first_frame=receipt['first_frame'], last_frame=last, instruments=receipt['instruments'], closed=bool(close),
                           line=receipt['line'], valid=True, frames=set()))
    used = {key(scope) for scope in scopes}
    covered, aggregate = defaultdict(list), []
    for scope in scopes:
        identity, start, end, enabled = key(scope), scope['first_frame'], scope['last_frame'], scope['instruments']
        expected = {name for name, active in enabled.items() if active}
        by_frame = {}
        for row in grouped[identity]:
            frame, costs = row.get('frame'), row.get('costs_ms')
            measured = (type(frame) is int and start <= frame <= end and frame not in by_frame
                        and row.get('partition_valid') is True and isinstance(costs, dict) and set(costs) == expected
                        and all(number(v) for v in costs.values()))
            if not measured:
                errors.append(f'{row["path"]}:{row["line"]}: invalid, duplicated or unpartitioned frame costs'); scope['valid'] = False; continue
            by_frame[frame] = row
            aggregate.append(dict(process=scope['process'], incarnation=scope['incarnation'], round=scope['round'], segment=scope['segment'],
                                  frame=frame, total_ms=sum(costs.values()), costs_ms=dict(costs), path=row['path'], line=row['line']))
            for name, value in costs.items():
                covered[name].append(dict(frame=frame, ms=value, path=row['path'], line=row['line']))
        scope['frames'] = set(by_frame)
        if len(by_frame) != end - start + 1:
            errors.append(f'{identity}: cost frames={len(by_frame)}, declared={end-start+1}'); scope['valid'] = False
        for name, active in enabled.items():
            if not active and any(row['path'] == scope['path'] for row in samples[name]):
                errors.append(f'{identity}: {name} declared disabled but native cost observations exist'); scope['valid'] = False
    if set(grouped) - used: errors.append('cost frames have no owning instrumentation scope')
    # What ran while no frame did (a lobby, a loading screen, the sampler's symbol load) is receipted before a scope's first frame.
    outside_ms, outside_rows = defaultdict(float), []
    owners = {key(scope): scope for scope in scopes}
    for row in outside:
        scope, costs = owners.get(key(row)), row.get('costs_ms')
        if scope is None or row.get('before_frame') != scope['first_frame']:
            errors.append(f'{row["path"]}:{row["line"]}: outside-frame costs have no owning scope'); continue
        expected = {name for name, active in scope['instruments'].items() if active}
        if not isinstance(costs, dict) or set(costs) != expected or not all(number(v) for v in costs.values()):
            errors.append(f'{row["path"]}:{row["line"]}: invalid outside-frame costs'); continue
        outside_rows.append(dict(round=row.get('round'), segment=row.get('segment', 0), before_frame=row['before_frame'], costs_ms=dict(costs),
                                 path=row['path'], line=row['line']))
        for name, value in costs.items():
            outside_ms[name] += value
    if first_frame is not None or last_frame is not None:
        covers, best = window_coverage(scopes, first_frame, last_frame)
        if not covers:
            errors.append(f'cost coverage {best[1][0]}..{best[1][1]} in round {best[0][3]}, requested {first_frame}..{last_frame}'
                          if best else f'no match round has cost frames; requested {first_frame}..{last_frame}')
            for scope in scopes:
                scope['valid'] = False
    complete_scopes = [scope['valid'] for scope in scopes]
    instruments = {}
    for name in INSTRUMENTS:
        declarations = [scope['instruments'].get(name, False) for scope in scopes]
        known = bool(declarations) and all(type(v) is bool for v in declarations)
        disabled = known and not any(declarations) and not samples[name]
        values = [r['ms'] for r in covered[name]]
        observed_max = max([r.get('max_ms', 0) for r in samples[name]] + values, default=None)
        missing_sources = []
        for row in samples[name]:
            own_values = [value['ms'] for value in covered[name] if value['path'] == row['path']]
            if not own_values:
                missing_sources.append(dict(path=row['path'], line=row['line']))
            elif row.get('max_ms', 0) > max(own_values):
                errors.append(f'{row["path"]}:{row["line"]}: {name} native cost {row["max_ms"]} ms exceeds its frame partition maximum {max(own_values)} ms')
        if observed_max is not None and not number(observed_max): errors.append(f'{name}: invalid measured cost {observed_max}')
        measured = bool(values) and all(complete_scopes) and not missing_sources
        ordered = sorted(values)
        instruments[name] = dict(status='DISABLED' if disabled else 'MEASURED' if measured else 'MISSING COST',
            scope_known=known and not missing_sources, complete=disabled or measured, missing_cost_sources=missing_sources,
            measured_total_ms=sum(values) if values else None, measured_max_ms=observed_max,
            mean_frame_ms=sum(values) / len(values) if values else None,
            p95_frame_ms=ordered[min(len(ordered) - 1, int(len(ordered) * .95))] if ordered else None,
            outside_frames_ms=outside_ms[name] if name in outside_ms else None,
            native_samples=samples[name], frame_samples=len(values))
        if name == 'census':
            instruments[name]['native_instant_max_ms'] = max((r['total_ms'] for r in samples[name]), default=None)
    peak = max(aggregate, key=lambda row: row['total_ms'], default=None)
    maximum = max([r['total_ms'] for r in aggregate] + [r['measured_max_ms'] for r in instruments.values()
                  if r['measured_max_ms'] is not None and number(r['measured_max_ms'])], default=None)
    complete = bool(scopes) and bool(complete_scopes) and all(complete_scopes) and not errors and all(r['complete'] for r in instruments.values())
    over = maximum is not None and maximum > FRAME_BUDGET_MS
    status = 'FAIL' if errors or over else 'PASS' if complete else 'INCOMPLETE'
    missing = [name for name, row in instruments.items() if not row['complete']]
    over_instruments = {name: row['measured_max_ms'] for name, row in instruments.items()
                        if row['measured_max_ms'] is not None and number(row['measured_max_ms']) and row['measured_max_ms'] > FRAME_BUDGET_MS}
    peak_text = None
    if peak is not None:
        parts = ', '.join(f'{name}={value:g}' for name, value in sorted(peak['costs_ms'].items(), key=lambda item: -item[1]) if value)
        peak_text = f'peak frame {peak["frame"]} of round {peak["round"]} = {peak["total_ms"]:g} ms ({parts or "no instrument cost"})'
    over_text = []
    if over:
        over_text.append(f'measured peak={maximum} ms > {FRAME_BUDGET_MS} ms' + (f'; {peak_text}' if peak_text else ''))
        over_text += [f'{name} alone {value:g} ms' for name, value in over_instruments.items()]
    reason = '; '.join([*errors, *over_text, *([f'missing cost coverage: {missing}'] if missing else [])])
    census_instant = instruments['census']['native_instant_max_ms']
    if census_instant is not None:
        reason = '; '.join(filter(None, [reason, f'census instant={census_instant:g} ms of measured instrument work; frame partition required, no pace subtraction']))
    return dict(status=status, passed=status == 'PASS', complete=complete, instrument_valid=status == 'PASS',
                max_frame_ms=maximum, peak_frame=peak and {k: peak[k] for k in ('round', 'segment', 'frame', 'total_ms', 'costs_ms', 'path', 'line')},
                over_budget_instruments=over_instruments, budget_ms=FRAME_BUDGET_MS,
                measured_total_ms=sum(row['total_ms'] for row in aggregate) if aggregate else None,
                instruments=instruments, scope_receipts=[{k: v for k, v in scope.items() if k != 'frames'} for scope in scopes],
                outside_frames=outside_rows, measured_frames=len(aggregate), reason=reason, table=cost_table(instruments, peak_text, maximum),
                rule='Every enabled instrument has complete measured cost coverage; aggregate frame cost <= 50 ms',
                subtraction_ms=0, accounting='Only native partitioned frame costs are added. Summary-only costs remain partial; cumulative preview '
                'totals are not summed across snapshots. Work receipted before a run of frames began is listed per instrument, outside every frame.')


def cost_table(instruments, peak_text, maximum):
    """The per-instrument breakdown as text lines, the aggregate against the budget last."""
    def ms(value):
        return '-' if value is None else f'{value:.3f}'
    lines = [f'{"instrument":<17} {"status":<12} {"frames":>6} {"mean_ms":>8} {"p95_ms":>8} {"max_ms":>9} {"total_ms":>10} {"outside_ms":>10}']
    for name, row in instruments.items():
        lines.append(f'{name:<17} {row["status"]:<12} {row["frame_samples"]:>6} {ms(row["mean_frame_ms"]):>8} {ms(row["p95_frame_ms"]):>8} '
                     f'{ms(row["measured_max_ms"]):>9} {ms(row["measured_total_ms"]):>10} {ms(row["outside_frames_ms"]):>10}')
    lines.append(f'aggregate frame max {ms(maximum)} ms, budget {FRAME_BUDGET_MS} ms' + (f'; {peak_text}' if peak_text else ''))
    return lines
