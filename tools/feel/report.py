"""Reduce retained engine records without substituting estimates for missing samples."""
from __future__ import annotations

from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path
import re
from .records import open_record, record_path

TICKS = 1200
SIM_MS = 1000 / 60
HISTORY_FIELDS = ('session', 'match', 'history_branch', 'source_round', 'tick')


def declared_history_ranges(host_rows, boundaries, peers, *, smoke_ticks=None, final_tick=None):
    """Use native round boundaries and final outcome, never the largest observed tail as an oracle."""
    prefixes = []
    for row in host_rows:
        prefix = tuple(row.get(field) for field in HISTORY_FIELDS[:-1])
        if None not in prefix and prefix not in prefixes: prefixes.append(prefix)
    ranges, missing = [], []
    for index,prefix in enumerate(prefixes):
        endings = [r['final_tick'] for r in boundaries if tuple(r.get(field) for field in HISTORY_FIELDS[:-1]) == prefix and r.get('final_tick')]
        end = smoke_ticks if smoke_ticks is not None and len(prefixes) == 1 else \
              max(endings) if endings else final_tick if index == len(prefixes)-1 else None
        if end is None or int(end) != end or end < 1:
            missing.append(dict(zip(HISTORY_FIELDS[:-1],prefix))); continue
        ranges.append(dict(**dict(zip(HISTORY_FIELDS[:-1],prefix)),first=1,last=int(end),peers=list(peers)))
    return ranges, missing


def compare_histories(peers, ranges, required_subsystems):
    """Compare every observation, preserving disagreement in earlier executions."""
    indexed, invalid, duplicates, first_difference = {}, [], 0, None
    for peer, rows in peers.items():
        observations, seen = defaultdict(list), set()
        for number, row in enumerate(rows, 1):
            missing = [field for field in (*HISTORY_FIELDS, 'instance', 'execution', 'incarnation')
                       if field not in row or row[field] is None]
            subsystems=row.get('subsystems')
            if not isinstance(subsystems,dict): missing.append('subsystems(object)')
            else:
                missing += sorted(set(required_subsystems) - subsystems.keys())
                missing += ['valid_hash:'+key for key,value in subsystems.items()
                            if not isinstance(value,str) or not re.fullmatch('[0-9a-f]{64}',value)]
            if not isinstance(row.get('sim_gated'),str) or not re.fullmatch('[0-9a-f]{64}',row.get('sim_gated','')):
                missing.append('valid_sim_gated_hash')
            missing += ['scalar:'+field for field in (*HISTORY_FIELDS,'instance','execution','incarnation')
                        if field in row and not isinstance(row[field],(int,str))]
            if missing:
                invalid.append(dict(peer=peer, line=row.get('_line', number), missing=missing))
                continue
            key = tuple(row[field] for field in HISTORY_FIELDS)
            identity = (row['instance'], row['execution'], row['incarnation'], *key)
            if identity in seen:
                duplicates += 1
            seen.add(identity)
            signature = {'sim_gated': row.get('sim_gated'), **{name: value for name, value in row['subsystems'].items()
                                                              if name != 'controller_route'}}
            observations[key].append((signature, row.get('_line', number), row.get('_path')))
        indexed[peer] = observations
    counts = {peer: dict(expected=0, present=0, missing=0) for peer in peers}
    equal, unknown, unequal, expected_keys = 0, 0, 0, set()
    strips = []
    for interval in ranges:
        if interval['first'] > interval['last'] or len(set(interval['peers'])) < 3:
            raise ValueError('a comparable interval needs at least three distinct peers and a nonempty range')
        prefix = tuple(interval[field] for field in HISTORY_FIELDS[:-1])
        for tick in range(interval['first'], interval['last'] + 1):
            key = (*prefix, tick)
            if key in expected_keys:
                raise ValueError(f'overlapping declared history range: {key}')
            expected_keys.add(key)
            values, absent = [], []
            for peer in interval['peers']:
                counts.setdefault(peer, dict(expected=0, present=0, missing=0))['expected'] += 1
                found = indexed.get(peer, {}).get(key, [])
                counts[peer]['present' if found else 'missing'] += 1
                values.extend((peer, signature, line, path) for signature, line, path in found)
                if not found:
                    absent.append(peer)
            differs = False
            if values:
                reference_peer, reference, reference_line, reference_path = values[0]
                for peer, signature, line, path in values[1:]:
                    if signature != reference:
                        differs = True
                        if first_difference is None:
                            section = next(name for name in sorted(reference.keys() | signature.keys())
                                           if signature.get(name) != reference.get(name))
                            first_difference = dict(key=dict(zip(HISTORY_FIELDS, key)), section=section,
                                first_peer=reference_peer, first_line=reference_line, peer=peer, line=line,
                                first_path=reference_path, path=path,
                                first_value=reference.get(section), value=signature.get(section))
            unequal += differs
            unknown += bool(absent)
            equal += not differs and not absent
            status = 'UNEQUAL' if differs else 'UNKNOWN' if absent else 'EQUAL'
            if strips and strips[-1]['status'] == status and strips[-1]['prefix'] == list(prefix) and strips[-1]['last'] + 1 == tick:
                strips[-1]['last'] = tick
            else:
                strips.append(dict(prefix=list(prefix), first=tick, last=tick, status=status))
    unexpected = sum(key not in expected_keys for observations in indexed.values() for key in observations)
    return dict(passed=bool(expected_keys) and not (unknown or unequal or invalid or duplicates),
                equal_keys=equal, unknown_keys=unknown, unequal_keys=unequal, duplicates=duplicates,
                unexpected_keys=unexpected, invalid=invalid[:100], invalid_count=len(invalid),
                peers=counts, first_difference=first_difference, strips=strips,
                scope='sim_gated and hashed tick-end subsystems; controller_route is per-peer')


def reduce_recoveries(schedule, events, now_ms):
    results = []
    for case in schedule:
        if case['deadline_ms'] <= 0 or not case['outcomes']:
            raise ValueError('recovery deadlines and terminal outcomes must be declared')
        rows = [row for row in events if all(row.get(field) == case[field] for field in ('id', 'peer'))
                and row.get('incarnation') in (case['incarnation'], case.get('return_incarnation', case['incarnation']))]
        starts = [row for row in rows if row.get('incarnation') == case['incarnation'] and row['phase'] in ('loss', 'hold', 'fault_applied')]
        start = min((row['wall_ms'] for row in starts), default=None)
        terminal = next((row for row in rows if row['phase'] in case['outcomes']
                         and row.get('incarnation') == case.get('return_incarnation', case['incarnation'])
                         and row['phase'] not in ('queued_admission', 'cancelled_reclaim')
                         and start is not None and row['wall_ms'] >= start), None)
        duration = (terminal['wall_ms'] if terminal else now_ms) - start if start is not None else None
        start_upper=min((row.get('upper_wall_ms',row['wall_ms']) for row in starts),default=None)
        lower=max(0,terminal.get('lower_wall_ms',terminal['wall_ms'])-start_upper) if terminal and start_upper is not None else None
        domains={row.get('clock_domain','legacy_native') for row in [*starts,*([terminal] if terminal else [])]}
        result=dict(case)
        result.update(phases=rows, scheduled_duration_ms=case.get('duration_ms'), duration_ms=duration,
                      duration_lower_ms=lower,duration_upper_ms=duration,clock_domains=sorted(domains),
                      censored=terminal is None, outcome=terminal['phase'] if terminal else None,
                      passed=terminal is not None and len(domains)==1 and 0 <= duration <= case['deadline_ms'])
        results.append(result)
    return results


def reduce_net_window(committed, waits, first, last, sim_tick_ms, missing_frame_stalls=None):
    by_tick = defaultdict(list)
    for row in committed:
        if first <= row['tick'] <= last:
            by_tick[row['tick']].append(row['wall_ms'])
    complete = last > first and set(by_tick) == set(range(first, last + 1)) and all(len(v) == 1 for v in by_tick.values())
    wall = by_tick[last][0] - by_tick[first][0] if complete else None
    if wall is not None and wall <= 0:
        complete, wall = False, None
    eligible = [row for row in (waits or []) if first < row['tick'] <= last]
    wait = sum(row['wait_ms'] for row in eligible) if waits is not None else None
    horizon = max(0, max(by_tick[t][0] - by_tick[first][0] - (t - first) * sim_tick_ms for t in by_tick)) if complete and sim_tick_ms else None
    return dict(complete=complete, first_tick=first, last_tick=last, eligible_ticks=last-first,
                missing_ticks=last-first+1-len(by_tick), steady_wall_ms=wall,
                steady_wall_tps=(last-first)*1000/wall if wall else None,
                net_wait_ms=wait, waiting_percent=100*wait/wall if wall and wait is not None else None,
                longest_stall_ms=max((row['wait_ms'] for row in eligible), default=0) if complete else None,
                steady_waits_over_50=sum(row['wait_ms'] > 50 for row in eligible) if complete else None,
                steady_missing_frame_stalls=missing_frame_stalls, confirmed_horizon_lag_ms=horizon,
                sim_tick_ms=sim_tick_ms)


def reduce_memory(samples, *, warmup_s, slope_bytes_per_minute, retained_bytes, sample_seconds, elapsed_s):
    if min(warmup_s, slope_bytes_per_minute, retained_bytes) < 0 or sample_seconds <= 0:
        raise ValueError('invalid memory bounds')
    expected = math.floor(elapsed_s / sample_seconds) + 1
    slots = {math.floor(row['elapsed_s'] / sample_seconds) for row in samples}
    sizes = {}
    for field in ('working_set', 'resident', 'private', 'virtual'):
        values = [(r['elapsed_s'], r[field]) for r in samples if r['elapsed_s'] >= warmup_s and r.get(field) is not None]
        if len(values) < 2:
            continue
        x = sum(t for t, _ in values) / len(values); y = sum(v for _, v in values) / len(values)
        divisor = sum((t-x)**2 for t, _ in values)
        slope = sum((t-x)*(v-y) for t, v in values) / divisor * 60 if divisor else None
        growth = values[-1][1] - values[0][1]
        sizes[field] = dict(first=values[0][1], last=values[-1][1], peak=max(v for _, v in values),
                            slope_bytes_per_minute=slope, retained_bytes=growth,
                            passed=slope is not None and slope <= slope_bytes_per_minute and growth <= retained_bytes)
    return dict(passed=bool(sizes) and len(slots) >= expected and all(v['passed'] for v in sizes.values()),
                warmup_s=warmup_s, slope_bound=slope_bytes_per_minute, retention_bound=retained_bytes,
                expected_samples=expected, observed_samples=len(samples), missing_samples=max(0, expected-len(slots)),
                sizes=sizes, instrumentation_growth='N/A without measured allocation records; no subtraction')


HOLD_OF_THIS_SEAT = re.compile(r'^\[net-lockstep\] hold of this seat at (\d+) ')
ROUND_START = re.compile(r'^\[net-lockstep\] start round=(\d+) frame=\d+ local_peer=(\d+) ')
SEAT_RECLAIMED = re.compile(r'^\[net-match\] seat-reclaimed peer=(\d+) frame=(\d+) ')


def round_cadence_ticks(effective_start, end_tick, every):
    """The periodic samples a round owes by the engine's capture point (Main.cpp, -net-fullstate-hash-every): its first
    tick with committed input (the lockstep's effective start frame), then every tick on the cadence while the activity
    runs; the tick the round's budget ends it at is not sampled."""
    return sorted({effective_start} | set(range(-(-effective_start // every) * every, end_tick, every)))


def own_hold_windows(lines):
    """The ticks a peer did not play live because its own seat was held: from its 'hold of this seat at F' line to its own
    'seat-reclaimed peer=<self> frame=G' line, as (round, F, G) with G excluded; the round is the one the peer was
    playing when held. A hold with no return names no window."""
    windows, round_id, local, held = [], None, None, None
    for line in lines:
        if match:=ROUND_START.match(line):
            round_id, local = int(match[1]), int(match[2])
        elif match:=HOLD_OF_THIS_SEAT.match(line):
            held = (round_id, int(match[1]))
        elif (match:=SEAT_RECLAIMED.match(line)) and held and local is not None and int(match[1]) == local:
            windows.append((held[0], held[1], int(match[2])))
            held = None
    return windows


def parse_fullstate(paths):
    samples, scopes, contexts, refusals, coalesced, holds = [], defaultdict(list), defaultdict(list), [], [], []
    sample_pattern = re.compile(r'^\[fullstate(?:-(canonical|restored))?\] tick=(\d+) hash=([0-9a-f]{16}) sections=(\S+) round=(\d+)')
    # The engine's own record of a periodic sample its writer replaced before writing it (ActivityMan's per-series bound).
    coalesced_pattern = re.compile(r'^\[fullstate-coalesced\] tick=(\d+) replaced=(\d+) ')
    scope_pattern = re.compile(r'^\[fullstate-scope\] tick=(\d+) round=(\d+) label=(\S+) per_peer=(.*)$')
    context_pattern = re.compile(r'^\[fullstate-context\] tick=(\d+) round=(\d+) label=(\S+) path=(.*)$')
    for path in paths:
        path=Path(path)
        if not path.is_file(): continue
        seat_lines=[]
        with path.open(encoding='utf-8-sig',errors='replace') as stream:
            for number,line in enumerate(stream,1):
                if line.startswith(('[net-lockstep] start round=','[net-lockstep] hold of this seat at ','[net-match] seat-reclaimed ')):
                    seat_lines.append(line.strip())
                if match:=context_pattern.match(line.strip()):
                    key=(int(match[2]),int(match[1]),match[3])
                    contexts[key].append(dict(path=match[4],log=str(path),line=number))
                elif match:=scope_pattern.match(line.strip()):
                    key=(int(match[2]),int(match[1]),match[3])
                    scopes[key].append(dict(per_peer=match[4].split(',') if match[4] else [],log=str(path),line=number))
                elif match:=coalesced_pattern.match(line.strip()):
                    coalesced.append(dict(tick=int(match[1]),replaced=int(match[2]),log=str(path),line=number))
                elif match:=sample_pattern.match(line.strip()):
                    label=match[1] or 'sample'; key=(int(match[5]),int(match[2]),label)
                    sections=dict(item.rsplit(':',1) for item in match[4].split(','))
                    samples.append(dict(key=key,hash=match[3],sections=sections,log=str(path),line=number))
                elif re.match(r'^\[fullstate(?:-[^]]+)?\] tick=\d+ (?:not taken|refused:|failed:)',line):
                    refusals.append(dict(log=str(path),line=number,text=line.strip()))
                elif line.startswith('[fullstate-refusal] '):
                    refusals.append(dict(log=str(path),line=number,text=line.strip()))
        holds.extend(own_hold_windows(seat_lines))
    ordinal=Counter()
    for sample in samples:
        key=sample['key']; index=ordinal[key]; ordinal[key]+=1
        sample['scope']=scopes[key][index] if index<len(scopes[key]) else None
        sample['context']=contexts[key][index] if index<len(contexts[key]) else None
        sample['scope_valid']=sample['scope'] is not None and 'header' in sample['sections'] and not (set(sample['sections']) & set(sample['scope']['per_peer']))
    return dict(samples=samples,refusals=refusals,coalesced=coalesced,holds=holds)


def compare_fullstate_histories(peers, expected):
    """Every expected key is sampled by every peer, or its absence is named by that peer's own lines: a [fullstate-coalesced]
    line (one line excuses one absence of its tick's periodic sample) or the peer's own hold window of that round (a held
    peer does not play the ticks, so it does not sample them); a labelled capture is never excused. The hashes are
    compared across the peers that wrote the key, and at least one key must be written by every peer."""
    missing, differences, bad_scope, restored, excused, held = [], [], [], [], [], []
    indexed={}
    replaced={peer:Counter(row['replaced'] for row in document.get('coalesced',[])) for peer,document in peers.items()}
    for peer,document in peers.items():
        indexed[peer]=defaultdict(list)
        for sample in document['samples']:
            indexed[peer][tuple(sample['key'])].append(sample)
            if not sample['scope_valid'] or sample['context'] is None: bad_scope.append(dict(peer=peer,log=sample['log'],line=sample['line']))
    for peer,document in peers.items():
        for sample in document['samples']:
            if sample['key'][2]!='restored': continue
            canonical_key=(*sample['key'][:2],'canonical')
            canonical=[c for other in indexed.values() for c in other.get(canonical_key,[])]
            restored.append(dict(peer=peer,key=sample['key'],canonical_found=bool(canonical),
                equal=bool(canonical) and all(c['sections']==sample['sections'] and c['hash']==sample['hash'] for c in canonical)))
    compared=0
    for key in expected:
        values=[]
        for peer in peers:
            found=indexed[peer].get(tuple(key),[])
            if not found and key[2]=='sample' and replaced[peer][key[1]]>0:
                replaced[peer][key[1]]-=1
                excused.append(dict(peer=peer,key=tuple(key)))
            elif not found and key[2]=='sample' and any(round_id==key[0] and first<=key[1]<end for round_id,first,end in peers[peer].get('holds',[])):
                held.append(dict(peer=peer,key=tuple(key)))
            elif not found: missing.append(dict(peer=peer,key=key))
            values.extend((peer,sample) for sample in found)
        if len({peer for peer,_ in values})==len(peers): compared+=1
        if values:
            first_peer,first=values[0]
            for peer,sample in values[1:]:
                if sample['sections']!=first['sections'] or sample['hash']!=first['hash']:
                    names=first['sections'].keys() | sample['sections'].keys()
                    differing=[n for n in sorted(names) if first['sections'].get(n)!=sample['sections'].get(n)]
                    differences.append(dict(key=key,first_peer=first_peer,peer=peer,sections=differing or ['combined_hash'],
                                            first_log=first['log'],first_line=first['line'],log=sample['log'],line=sample['line']))
    refused=[dict(peer=peer,**row) for peer,document in peers.items() for row in document['refusals']]
    return dict(passed=bool(expected) and compared>0 and not (missing or differences or bad_scope or refused) and all(r['equal'] for r in restored),
                expected_samples_per_peer=len(expected),compared_samples=compared,
                coalesced={peer:sum(row['peer']==peer for row in excused) for peer in peers},coalesced_samples=excused,
                held={peer:sum(row['peer']==peer for row in held) for peer in peers},held_samples=held,
                missing=missing,differences=differences,scope_failures=bad_scope,
                refusals=refused,restores=restored,scope='Shared sections only; PerPeer exclusions remain explicit and need behavioral oracles')
KILLALL = re.compile(r'killall sparing team \d+ at tick (\d+)')
SCENARIO_EARLY = re.compile(r'\[scenario\] \S+ passed=no ticks=(\d+)')


class EarlyDecision(ValueError):
    def __init__(self, tick, path=None):
        self.tick = tick
        self.path = path
        self.fail_line = f'FAIL: decided at tick {tick}; measurement window is {TICKS} ticks'
        super().__init__(self.fail_line)


def dump_ticks(path):
    ticks = set()
    with open_record(path, encoding='utf-8-sig') as stream:
        for line in stream:
            if re.match(r'^\d+ activity ', line):
                ticks.add(int(line.split(' ', 1)[0]))
    return ticks


def early_decision_tick(run, peer, ticks=TICKS):
    run = Path(run)
    log = run / peer / 'stdout.log'
    if log.is_file():
        text = log.read_text(encoding='utf-8-sig', errors='replace')
        match = KILLALL.search(text) or SCENARIO_EARLY.search(text)
        if match:
            tick = int(match[1])
            if 0 < tick < ticks:
                return tick
    dump = run / f'{peer}_trace.json.simdump.txt'
    if record_path(dump).is_file():
        observed = dump_ticks(dump)
        if observed and observed != set(range(1, ticks + 1)):
            return max(observed)
    return None


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, allow_nan=False) + '\n', encoding='utf-8')


def write_jsonl(path, rows):
    with Path(path).open('w', encoding='utf-8') as stream:
        for row in rows:
            stream.write(json.dumps(row, allow_nan=False) + '\n')


def read_jsonl(path):
    with open_record(path, encoding='utf-8-sig') as stream:
        for number, line in enumerate(stream, 1):
            row = json.loads(line, parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))
            if not isinstance(row, dict):
                raise ValueError(f'{path}:{number}: expected an object')
            row['_line'] = number
            yield row


def distribution(values):
    ordered = sorted(values)
    if not ordered:
        return dict(count=0, p50=None, p95=None, p99=None, max=None)
    if any(not math.isfinite(value) for value in ordered):
        raise ValueError('non-finite measurement')
    return dict(count=len(ordered), **{name: ordered[max(0, math.ceil(q * len(ordered)) - 1)]
                                     for name, q in [('p50', .5), ('p95', .95), ('p99', .99), ('max', 1)]})


def angular_delta(a, b):
    return (a - b + math.pi) % (2 * math.pi) - math.pi


def wrapped_delta(delta, size, wraps):
    return (delta + size / 2) % size - size / 2 if wraps and size else delta


def pin(value, rule, passed, evidence, detail=None, available=None):
    available = value is not None if available is None else available
    return dict(value=value, rule=rule, status=('PASS' if passed else 'FAIL') if available else 'MISS',
                evidence=[str(record_path(path)) for path in evidence], detail=detail)


def peer_id_of(report_path):
    """The lockstep seat a peer's own match report says it played."""
    path = Path(report_path)
    if not path.is_file():
        return None
    try:
        document = json.loads(path.read_text(encoding='utf-8-sig'))
    except (OSError, ValueError):
        return None
    service = document.get('service') if isinstance(document, dict) else None
    value = service.get('local_peer_id') if isinstance(service, dict) else None
    return int(value) if isinstance(value, int) and value > 0 else None


def return_hold_violations(log):
    rounds = defaultdict(lambda: dict(holds=[], returns=[]))
    current = 0
    for line in log.splitlines():
        if found := re.search(r'\[net-lockstep\] start round=(\d+)', line):
            current = int(found[1])
        if found := re.search(r'\[net-match\] hold peer=(\d+) frame=(\d+) AI in control', line):
            rounds[current]['holds'].append(tuple(map(int, found.groups())))
        if found := re.search(r'\[net-match\] seat-reclaimed peer=(\d+) frame=(\d+)', line):
            rounds[current]['returns'].append(tuple(map(int, found.groups())))
    return [dict(round=round_id, held_peer=peer, hold_tick=tick, returned_peer=returned, return_tick=back)
            for round_id, events in rounds.items() for peer, tick in events['holds'] for returned, back in events['returns']
            if 0 < tick - back <= 100]


def item9a_gates(run, peer='host', rows=None):
    run = Path(run)
    raw = record_path(run / peer / 'feel/raw.jsonl')
    manifest_path = run / 'manifest.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    final_tick = manifest.get('ticks', TICKS)
    rows = list(read_jsonl(raw)) if rows is None and raw.is_file() else rows or []
    clock_path = raw
    if not rows:
        live = run / f'{peer}-live.jsonl'
        clock_path = live
        rows = [dict(type='committed', tick=row['tick'], wall_ms=row['wall_ms'])
                for row in read_jsonl(live) if 'wall_ms' in row] if live.is_file() else []
    committed = [row for row in rows if row.get('type') == 'committed' and 300 <= row.get('tick', 0) <= final_tick]
    by_tick = defaultdict(list)
    for row in committed:
        by_tick[row['tick']].append(row['wall_ms'])
    first_tick = min(by_tick, default=None)
    wall_ms = (max(by_tick[final_tick]) - min(by_tick[first_tick])) if final_tick in by_tick and first_tick is not None and first_tick < final_tick else None
    tps = (final_tick - first_tick) * 1000 / wall_ms if wall_ms and wall_ms > 0 else None
    log_path = run / peer / 'stdout.log'
    log = log_path.read_text(encoding='utf-8-sig', errors='replace') if log_path.is_file() else ''
    waits = [(int(tick), int(ms)) for tick, ms in re.findall(r'\[net-frame-wait\] frame=(\d+) wait_ms=(\d+)', log) if 300 < int(tick) <= final_tick]
    wait_ms = sum(ms for _, ms in waits)
    wait_fraction = wait_ms / wall_ms if wall_ms and log_path.is_file() else None
    longest = max((ms for _, ms in waits), default=0) if wall_ms and log_path.is_file() else None
    steady_end = manifest.get('silent_tick') or final_tick + 1
    steady_stalls = sum(ms > 0 for tick, ms in waits if tick < steady_end) if wait_fraction is not None else None
    report_path = run / f'{peer}_report.json'
    report = json.loads(report_path.read_text(encoding='utf-8-sig')) if report_path.is_file() else {}
    def locksteps(node):
        found = []
        if isinstance(node, dict):
            if 'missing_frame_stalls' in node and 'next_frame' in node:
                found.append(node)
            for value in node.values():
                found.extend(locksteps(value))
        elif isinstance(node, list):
            for value in node:
                found.extend(locksteps(value))
        return found
    rounds = locksteps(report)
    latest = max(rounds, key=lambda value: value.get('next_frame', 0), default={})
    measured = [value['steady_missing_frame_stalls'] for value in rounds if value.get('steady_missing_frame_stalls') is not None]
    missing = sum(measured) if measured else None
    tick_ms = latest.get('sim_tick_ms')
    valid_tick = isinstance(tick_ms, (int, float)) and math.isfinite(tick_ms) and tick_ms > 0
    horizon_lag_ms = (max(0.0, max(max(stamps) - min(by_tick[first_tick]) - (tick - first_tick) * tick_ms
                                  for tick, stamps in by_tick.items())) if valid_tick and wall_ms is not None else None)
    evidence = [clock_path, log_path, report_path]
    pins = {
        'item9a_wall_tps': pin(tps, '>= 59.5 after tick 300, including recovery time', tps is not None and tps >= 59.5, evidence),
        'item9a_net_wait': pin(wait_fraction, '< 0.01 of steady wall time', wait_fraction is not None and wait_fraction < .01, evidence),
        'item9a_steady_stalls': pin(steady_stalls, '0 blocking waits before the injected spike', steady_stalls == 0, evidence),
        'item9a_longest_wait': pin(longest, '<= 50 ms', longest is not None and longest <= 50, evidence),
        'item9a_confirmed_horizon_lag': pin(horizon_lag_ms, '<= 50 ms behind the steady confirmed-tick clock, including recovery',
            horizon_lag_ms is not None and horizon_lag_ms <= 50, evidence),
    }
    if manifest.get('loss_percent'):
        loss_log = run / 'client/stdout.log'
        armed = re.findall(r'\[net-transport-loss\] percent=(\S+) send_recv_armed=(\d+) round=(\d+)', loss_log.read_text(encoding='utf-8-sig', errors='replace')) if loss_log.is_file() else []
        accepted = bool(armed) and all(float(percent) == manifest['loss_percent'] and status == '1' for percent, status, _ in armed)
        pins['item9a_loss_armed'] = pin(armed, 'GNS accepted 5 percent packet loss on client send and receive for every observed arm', accepted, [loss_log])
    if manifest.get('silent_tick'):
        # The seat the silent peer got is whatever the lobby gave it; read it instead of assuming 2.
        silent_seat = peer_id_of(run / 'client_report.json') or 2
        holds = [(int(seat), int(tick)) for seat, tick in re.findall(r'\[net-match\] hold peer=(\d+) frame=(\d+) AI in control', log)]
        injection_path = run / 'client/stdout.log'
        injection_log = injection_path.read_text(encoding='utf-8-sig', errors='replace') if injection_path.is_file() else ''
        injections = [int(tick) for tick in re.findall(r'\[selftest\] frame stall tick=(\d+)', injection_log)]
        injection = next((tick for tick in injections if tick >= manifest['silent_tick']), manifest['silent_tick'])
        steady_stalls = sum(ms > 0 for tick, ms in waits if tick < injection) if wait_fraction is not None else None
        pins['item9a_steady_stalls'] = pin(steady_stalls, '0 blocking waits before the actual injected spike', steady_stalls == 0, evidence)
        spike_waits = sum(ms > 0 for tick, ms in waits if tick >= injection) if wait_fraction is not None else None
        pins['item9a_spike_waits'] = pin(spike_waits, '<= 1 blocking wait from the single injected spike through return',
                                       spike_waits is not None and spike_waits <= 1, evidence)
        returns = [(int(seat), int(tick)) for seat, tick in re.findall(r'\[net-match\] seat-reclaimed peer=(\d+) frame=(\d+)', log)]
        before = [tick for seat, tick in holds if seat == silent_seat and tick <= injection]
        held = next((tick for seat, tick in sorted(holds, key=lambda value: value[1]) if seat == silent_seat and tick >= injection), None)
        if before and not any(seat == silent_seat and max(before) < tick <= injection for seat, tick in returns):
            held = max(before)
        # The feel window stays fixed; a return may land past it, inside the round, and the peer that returns may be a relaunch.
        rejoin_end = manifest.get('rejoin_through_tick', final_tick)
        returners = manifest.get('returner_peers', ['client'])
        pins['item9a_hold'] = pin(held, f'silent seat {silent_seat} is held from its agreed frame', held is not None,
            [log_path, injection_path], dict(requested_tick=manifest['silent_tick'], actual_tick=injection), available=log_path.is_file())
        def reclaims(name):
            path = run / name / 'stdout.log'
            text = path.read_text(encoding='utf-8-sig', errors='replace') if path.is_file() else ''
            found = re.findall(r'\[net-match\] seat-reclaimed peer=(\d+) frame=(\d+) live_actors=(\d+)', text)
            return [(int(tick), int(live)) for seat, tick, live in found
                    if held is not None and int(seat) == silent_seat and held < int(tick) <= rejoin_end and int(live) > 0], path
        host_reclaims, host_reclaim_path = reclaims('host')
        survivor_reclaims, survivor_reclaim_path = reclaims('survivor')
        returner_paths = [run / name / 'stdout.log' for name in returners]
        completed = [int(frame) for path in returner_paths if path.is_file()
                     for frame in re.findall(r'\[net-match\] private catch-up complete frame=(\d+)', path.read_text(encoding='utf-8-sig', errors='replace'))]
        rejoined = bool(host_reclaims) and host_reclaims == survivor_reclaims and all(tick in completed for tick, _ in host_reclaims)
        pins['item9a_rejoin'] = pin(rejoined, 'both survivors committed the same live reclaim and that client completed private catch-up at its activation frame',
            rejoined, [host_reclaim_path, survivor_reclaim_path, *returner_paths], dict(host=host_reclaims, survivor=survivor_reclaims, completed=completed))
        survivor_logs = [(run / name / 'stdout.log') for name in ('host', 'survivor')]
        reloads = [str(path) for path in survivor_logs if path.is_file() and re.search(
            r'\[net-match\].*(?:resync:|resyncing the match)', path.read_text(encoding='utf-8-sig', errors='replace'), re.I)]
        pins['item9a_private_rejoin'] = pin(rejoined and not reloads, 'private catch-up completes without reloading either survivor',
            rejoined and not reloads, [*survivor_logs, *returner_paths], dict(survivor_reloads=reloads))
        def hashes(name):
            path = run / f'{name}_trace.json'
            if not path.is_file():
                return {}, path
            document = json.loads(path.read_text(encoding='utf-8-sig'))
            values = {}
            for segment in document.get('runs', []):
                for value in segment.get('tick_hashes', []):
                    if value['tick'] in values and values[value['tick']] != value:
                        return {}, path
                    values[value['tick']] = value
            return values, path
        host_hashes, host_path = hashes('host')
        survivor_hashes, survivor_path = hashes('survivor')
        compare_through = max([final_tick, *(tick for tick, _ in host_reclaims), *(tick for tick, _ in survivor_reclaims)])
        same = held is not None and all(tick in host_hashes and host_hashes.get(tick) == survivor_hashes.get(tick) for tick in range(held, compare_through + 1))
        pins['item9a_ai_takeover_hash'] = pin(same, 'every committed hash from hold through rejoin equals on both survivors', same,
            [host_path, survivor_path], dict(first_tick=held, last_tick=compare_through), available=bool(host_hashes) and bool(survivor_hashes))
        reholds = return_hold_violations(log)
        pins['item9a_no_rehold'] = pin(reholds, 'no seat is held within 100 frames after any return', not reholds, [log_path])
    return dict(peer=peer, pins=pins, measurement_complete=wall_ms is not None and bool(rounds),
                pass_check=all(value['status'] == 'PASS' for value in pins.values()),
                metrics=dict(steady_wall_ms=wall_ms, steady_wall_tps=tps, net_wait_ms=wait_ms, longest_stall_ms=longest,
                             confirmed_horizon_lag_ms=horizon_lag_ms,
                             confirmed_horizon_lag_ticks=horizon_lag_ms / tick_ms if horizon_lag_ms is not None else None,
                             steady_missing_frame_stalls=missing, first_tick=first_tick, last_tick=final_tick if final_tick in by_tick else None,
                             clock_path=str(clock_path), sim_tick_ms=latest.get('sim_tick_ms'), peer_input_delays=latest.get('peer_input_delays', {})))


def apply_tps_call(result, reference):
    """Keep the matched single-player rate as evidence without changing the gate."""
    measured = result['metrics'].get('steady_wall_tps')
    baseline = reference.get('steady_wall_tps') if reference else None
    if not isinstance(baseline, (int, float)) or not math.isfinite(baseline) or baseline <= 0:
        return
    pins = result['pins']
    if 'item9a_wall_tps' not in pins:
        return
    original = dict(pins['item9a_wall_tps'])
    evidence = original['evidence'] + [reference['evidence']]
    pins['item9a_wall_tps'] = pin(measured,
        '>= 59.5 after tick 300, including recovery time', measured is not None and measured >= 59.5, evidence)
    result['tps_call'] = dict(reference=reference, absolute=original, minimum_tps=59.5, box_limited=baseline < 59.5)
    result['pass_check'] = all(value['status'] == 'PASS' for value in pins.values() if value.get('required', True))


def input_latencies(inputs, frames):
    results = []
    by_actor = defaultdict(list)
    for frame in frames:
        for actor in frame['actors']:
            by_actor[actor['uid']].append((frame, actor))
    for index, edge in enumerate(inputs):
        sequence = by_actor[edge['actor']['uid']]
        before = [(frame, actor) for frame, actor in sequence if frame['present_end_ms'] < edge['wall_ms']]
        reference = before[-1][1] if before else edge['actor']
        for change in edge['changes']:
            action, held = change['action'], change['held']
            if action not in ('AIM_VECTOR', 'L_LEFT', 'L_RIGHT'):
                continue
            def reflects(actor):
                if action == 'AIM_VECTOR':
                    if not held:
                        return False
                    target = -math.atan2(change['y'], change['x'])
                    command_angle = -math.atan2(actor['input']['aim_y'], actor['input']['aim_x'])
                    return (abs(angular_delta(actor['aim'], target)) < abs(angular_delta(reference['aim'], target))
                            and abs(angular_delta(command_angle, target)) < abs(angular_delta(reference['aim'], target))
                            and angular_delta(actor['aim'], reference['aim']) != 0)
                command = actor['input']['move_left' if action == 'L_LEFT' else 'move_right']
                if command != held:
                    return False
                if held:
                    sign = -1 if action == 'L_LEFT' else 1
                    return sign * (actor['vx'] - reference['vx']) > 0
                return abs(actor['vx']) < abs(reference['vx'])
            next_change = min((later['wall_ms'] for later in inputs[index + 1:]
                               if later['actor']['uid'] == edge['actor']['uid']
                               and any(item['action'] == action for item in later['changes'])), default=math.inf)
            found = next(((frame, actor) for frame, actor in sequence
                          if edge['wall_ms'] <= frame['draw_begin_ms'] < next_change and reflects(actor)), None)
            row = dict(input_line=edge['_line'], tick=edge['tick'], uid=edge['actor']['uid'], action=action, held=held,
                       input_wall_ms=edge['wall_ms'], last_presented_frame=edge['last_presented_frame'],
                       reference=reference, frame=None, ms=None, frames=None, budget_ms=None, pass_check=False,
                       observed_through_ms=min(next_change, frames[-1]['present_end_ms']), right_censored=found is None)
            if found:
                frame, actor = found
                elapsed = frame['present_end_ms'] - edge['wall_ms']
                frame_period = 1000 / frame['cap_hz'] if frame['cap_hz'] else frame['interval_ms']
                budget = 34.0 if frame['cap_hz'] == 60 else SIM_MS + frame_period
                row.update(frame=frame['frame'], frame_line=frame['_line'], reflected_actor=actor,
                           present_wall_ms=frame['present_end_ms'], ms=elapsed,
                           frames=frame['frame'] - edge['last_presented_frame'], budget_ms=budget,
                           pass_check=elapsed <= budget)
            row['latency_lower_bound_ms'] = row['ms'] if found else max(0, row['observed_through_ms'] - edge['wall_ms'])
            results.append(row)
    return results


def warp_records(frames):
    previous = {}
    output = []
    for frame in frames:
        visual_tick = frame['tick'] + frame['preview_depth'] - 1 + frame['alpha']
        for actor in frame['actors']:
            old = previous.get(actor['uid'])
            previous[actor['uid']] = (frame, actor, visual_tick)
            if old is None:
                continue
            old_frame, old_actor, old_tick = old
            seconds = (visual_tick - old_tick) / 60
            dx = wrapped_delta(actor['render_x'] - old_actor['render_x'], frame['scene_width'], frame['wraps_x'])
            dy = wrapped_delta(actor['render_y'] - old_actor['render_y'], frame['scene_height'], frame['wraps_y'])
            vx = (actor['vx'] + old_actor['vx']) / 2
            vy = (actor['vy'] + old_actor['vy']) / 2
            rx, ry = dx - vx * 20 * seconds, dy - vy * 20 * seconds
            residual = math.hypot(rx, ry)
            if residual > 0:
                output.append(dict(frame=frame['frame'], raw_line=frame['_line'], previous_raw_line=old_frame['_line'],
                                   tick=frame['tick'], uid=actor['uid'], wall_ms=frame['present_end_ms'],
                                   x=actor['render_x'], y=actor['render_y'], dx=dx, dy=dy,
                                   visual_sim_seconds=seconds, velocity_expected_dx=vx * 20 * seconds,
                                   velocity_expected_dy=vy * 20 * seconds, residual_px=residual,
                                   over_4_px=residual > 4, cause='unclassified kinematic residual'))
    return output


COMMAND = re.compile(r'^\[net-replay-dump\] frame=(\d+) command=(\S+) sender=(\d+)(.*)$')
VOICE = re.compile(r'^\[preview-event\] voice committed=(\d+) tick=(\d+) uid=(\d+) previewed=(\d+) '
                   r'asset=(\d+) preset=(\d+) seq=(\d+) preset_name="(.*?)" path=(.*)$')
POSITION = re.compile(r'^(\d+) actor uid=(\d+) .*? pos=(\S+),(\S+) prev=')
AUTO_DELAY = re.compile(r'^\[net-match\] auto input delay: peer (\d+) rtt (\d+)ms -> (\d+) frames \(manual floor (\d+)\)$')


def remote_commands(path, local_peer, first_tick=1, last_tick=TICKS):
    commands = defaultdict(list)
    if not path.is_file():
        return commands, False
    for line_no, line in enumerate(path.read_text(encoding='utf-8-sig', errors='replace').splitlines(), 1):
        match = COMMAND.match(line)
        if match and int(match[3]) != local_peer:
            commands[int(match[1])].append(dict(command=match[2], sender=int(match[3]), detail=match[4], raw_line=line_no))
    launch_path = path.parent / 'launch.json'
    verify_path = path.parent.parent / 'replay-report.json'
    if not launch_path.is_file() or not verify_path.is_file():
        return commands, False
    launch = json.loads(launch_path.read_text(encoding='utf-8-sig'))
    verify = json.loads(verify_path.read_text(encoding='utf-8-sig'))
    complete = (launch.get('exit_code') == 0 and launch.get('evidence_complete') is True and not launch.get('timed_out')
                and verify.get('ok') is True and verify.get('first_frame') == first_tick and verify.get('last_frame', 0) >= last_tick)
    return commands, complete


def canonical_positions(path, wanted, first_tick=1, last_tick=TICKS):
    actors, ticks, records = {}, set(), {}
    duplicate = None
    duplicate_count = 0
    repeat_count = 0
    with open_record(path, encoding='utf-8-sig') as stream:
        for number, line in enumerate(stream, 1):
            if re.match(r'^\d+ activity ', line):
                ticks.add(int(line.split(' ', 1)[0]))
            match = POSITION.match(line)
            if match:
                key = int(match[1]), int(match[2])
                if key in wanted:
                    if key in actors:
                        # A resync restarts the simdump, so an epoch can be written again; an identical
                        # record is that repetition. A record that differs at the same tick and actor is
                        # the engine committing it twice, which is a failed pin, not a trace artefact.
                        if records[key] == line:
                            repeat_count += 1
                            continue
                        duplicate_count += 1
                        if duplicate is None:
                            duplicate = dict(tick=key[0], actor=key[1], line=number,
                                             first_line=actors[key][1], path=str(path))
                        continue
                    actors[key] = (dict(pos=[float.fromhex(match[3]), float.fromhex(match[4])]), number)
                    records[key] = line
    if (ticks if first_tick == 1 and last_tick == TICKS else {tick for tick in ticks if first_tick <= tick <= last_tick}) != set(range(first_tick, last_tick + 1)):
        raise EarlyDecision(max(ticks) if ticks else 0, path)
    canonical_positions.last_duplicate = dict(first=duplicate, count=duplicate_count) if duplicate else None
    canonical_positions.last_repeats = repeat_count
    return actors


def corrections(previews, committed, canonical_path, command_path, local_peer, first_tick=1, last_tick=TICKS):
    forecasts = {}
    for row in previews:
        key = (row['target_tick'], row['actor']['uid'])
        if key not in forecasts or row['committed_tick'] > forecasts[key]['committed_tick']:
            forecasts[key] = row
    canonical = canonical_positions(canonical_path, forecasts, first_tick, last_tick)
    candidate_duplicate = getattr(canonical_positions, 'last_duplicate', None)
    corrections.last_duplicate = candidate_duplicate if isinstance(candidate_duplicate, dict) else None
    commands, commands_complete = remote_commands(command_path, local_peer, first_tick, last_tick)
    result, missing = [], []
    for (tick, uid), forecast in sorted(forecasts.items()):
        if not first_tick <= tick <= last_tick:
            continue
        if (tick, uid) not in canonical:
            missing.append(dict(tick=tick, uid=uid, preview_line=forecast['_line']))
            continue
        actor, line = canonical[tick, uid]
        observed = next((row for row in committed if row['tick'] >= tick), None)
        dx, dy = actor['pos'][0] - forecast['actor']['x'], actor['pos'][1] - forecast['actor']['y']
        if observed:
            dx = wrapped_delta(dx, observed['scene_width'], observed['wraps_x'])
            dy = wrapped_delta(dy, observed['scene_height'], observed['wraps_y'])
        distance = math.hypot(dx, dy)
        wall = observed['wall_ms'] if observed else None
        result.append(dict(tick=tick, uid=uid, displacement_px=distance, over_4_px=distance > 4,
                           predicted_x=forecast['actor']['x'], predicted_y=forecast['actor']['y'],
                           committed_x=actor['pos'][0], committed_y=actor['pos'][1],
                           preview_line=forecast['_line'], canonical_dump_line=line, commit_observed_wall_ms=wall,
                           remote_commands_same_frame=commands[tick], remote_cause_proven=False,
                           cause='remote command in same frame; causality unproven' if commands[tick] else 'no same-frame remote command recorded'))
    return result, missing, commands_complete


def max_in_window(values, width):
    values = sorted(values)
    left, maximum = 0, 0
    for right, value in enumerate(values):
        while value - values[left] >= width:
            left += 1
        maximum = max(maximum, right - left + 1)
    return maximum


def firing_records(inputs, previews, frames, stdout_path):
    voices = []
    for number, text in enumerate(stdout_path.read_text(encoding='utf-8-sig', errors='replace').splitlines(), 1):
        match = VOICE.match(text)
        if match:
            values = [int(match[i]) for i in range(1, 8)]
            voices.append(dict(zip(('committed', 'tick', 'uid', 'previewed', 'asset', 'preset', 'seq'), values),
                               name=match[8], path=match[9], line=number))
    presses = [edge for edge in inputs if any(change['action'] == 'FIRE' and change['held'] for change in edge['changes'])]
    result = []
    for index, press in enumerate(presses):
        end_ms = presses[index + 1]['wall_ms'] if index + 1 < len(presses) else math.inf
        candidates = [row for row in previews if row['actor']['uid'] == press['actor']['uid']
                      and row['actor']['fired'] and press['wall_ms'] <= row['wall_upper_ms'] < end_ms]
        first = min(candidates, key=lambda row: row['wall_upper_ms'], default=None)
        row = dict(input_line=press['_line'], tick=press['tick'], uid=press['actor']['uid'],
                   firing_ms_upper=None, audio_ms_upper=None, presented_firing_ms=None,
                   preview_tick=False, once=False, voices=[], duplicate_voices=[],
                   observation_end_ms=min(end_ms, frames[-1]['present_end_ms']) if frames else press['wall_ms'],
                   right_censored=first is None)
        if first:
            matches = [voice for voice in voices if voice['uid'] in (first['actor']['uid'], first['actor'].get('gun_uid'))
                       and abs(voice['tick'] - first['target_tick']) <= 1]
            early = [voice for voice in matches if voice['committed'] == first['committed_tick']]
            identities = Counter((voice['preset'], voice['seq'], voice['path']) for voice in matches)
            duplicate = [list(key) for key, count in identities.items() if count > 1]
            visible = next((frame for frame in frames if frame['draw_begin_ms'] >= press['wall_ms']
                            and frame['draw_begin_ms'] < end_ms
                            and any(actor['uid'] == press['actor']['uid'] and actor['fired'] for actor in frame['actors'])), None)
            row.update(preview_line=first['_line'], event_tick=first['target_tick'],
                       firing_ms_upper=first['wall_upper_ms'] - press['wall_ms'],
                       firing_ms_lower=max(0, first['wall_lower_ms'] - press['wall_ms']),
                       audio_ms_upper=first['wall_upper_ms'] - press['wall_ms'] if early else None,
                       preview_tick=first['committed_tick'] == press['tick'] and bool(early),
                       once=bool(early) and not duplicate, voices=matches, duplicate_voices=duplicate,
                       presented_firing_ms=visible['present_end_ms'] - press['wall_ms'] if visible else None)
        result.append(row)
    return result


def reduce_peer(run, peer, baseline=None, *, ticks=TICKS, first_tick=1, allow_natural_end=False,
                host_peer="host", trace_run=0, expected_input_schedule=None, expected_capture_ticks=None,
                is_single_player=None, round_id=None):
    run = Path(run)
    if ticks < first_tick or first_tick < 1:
        raise ValueError("invalid measurement window")
    single_player = peer == "sp" if is_single_player is None else is_single_player
    decided = early_decision_tick(run, peer, ticks)
    if decided is not None and not allow_natural_end:
        raise EarlyDecision(decided, run / f'{peer}_trace.json.simdump.txt')
    if decided is not None and allow_natural_end:
        ticks = min(ticks, decided)
    raw = record_path(run / peer / 'feel/raw.jsonl')
    rows = list(read_jsonl(raw))
    if len([row for row in rows if row['type'] == 'schema' and row['version'] == 1]) != 1:
        raise ValueError(f'{raw}: missing or invalid schema')
    if round_id is not None:
        typed = [row for row in rows if row['type'] not in ('schema', 'end')]
        if any('round' not in row for row in typed):
            raise ValueError('round reduction requires round-labelled raw records')
        rows = [row for row in rows if row['type'] in ('schema', 'end') or row.get('round') == round_id]
    all_frames = [row for row in rows if row['type'] == 'frame']
    if [row['frame'] for row in all_frames] != list(range(1, len(all_frames) + 1)):
        raise ValueError(f'{raw}: missing or reordered frame')
    frames = [row for row in all_frames if row['active'] and first_tick <= row['tick'] <= ticks]
    inputs = [row for row in rows if row['type'] == 'input' and first_tick <= row['tick'] <= ticks]
    previews = [row for row in rows if row['type'] == 'preview' and first_tick <= row['committed_tick'] <= ticks]
    committed = [row for row in rows if row['type'] == 'committed']
    all_iterations = [row for row in rows if row['type'] == 'iteration']
    iterations = [row for row in all_iterations if row['active'] and first_tick <= row['tick'] <= ticks]
    if not frames or not iterations:
        raise ValueError(f'{raw}: no measured match frames or iterations')
    destination = run / peer / 'analysis'
    destination.mkdir(exist_ok=True)
    latency = input_latencies(inputs, frames)
    warps = warp_records(frames)
    controller = run / f'{peer}_controller.jsonl'
    canonical_dump = run / f'{peer}_trace.json.simdump.txt'
    command_log = run / 'replay-inspect/stdout.log'
    correction_rows, correction_missing, commands_complete = corrections(previews, committed, canonical_dump, command_log, frames[-1]['peer'], first_tick, ticks)
    duplicate_actor = getattr(corrections, 'last_duplicate', None)
    firing = firing_records(inputs, previews, frames, run / peer / 'stdout.log')
    paths = {name: destination / (name + '.jsonl') for name in ('latencies', 'warps', 'corrections', 'correction-missing', 'firing')}
    for name, values in [('latencies', latency), ('warps', warps), ('corrections', correction_rows),
                         ('correction-missing', correction_missing), ('firing', firing)]:
        write_jsonl(paths[name], values)
    trace = run / f'{peer}_trace.json'
    trace_document = json.loads(trace.read_text(encoding='utf-8-sig'))
    segments = trace_document['runs'] if trace_run is None else [trace_document['runs'][trace_run]]
    # The ship-feel gate (the default window) counts every trace row: a row outside 1..TICKS fails coverage as it always did;
    # only an explicit window (a cross-run round) reduces the rows inside it.
    windowed = (first_tick, ticks) != (1, TICKS)
    trace_ticks = [row for segment in segments for row in segment['tick_hashes'] if not windowed or first_tick <= row['tick'] <= ticks]
    coverage = [row['tick'] for row in trace_ticks] == list(range(first_tick, ticks + 1))
    ended = any(row['type'] == 'end' for row in rows)
    draw = distribution([frame['draw_ms'] for frame in frames])
    present = distribution([frame['present_ms'] for frame in frames])
    interval = distribution([frame['interval_ms'] for frame in frames if frame['interval_ms'] is not None])
    for values, field in ((draw, 'draw_ms'), (present, 'present_ms'), (interval, 'interval_ms')):
        values['count_over_50_ms'] = sum(frame[field] is not None and frame[field] > 50 for frame in frames)
    end_records = [row for row in rows if row['type'] == 'end']
    cpu_ms = end_records[-1]['cpu_ms'] - all_iterations[0]['cpu_begin_ms'] if end_records else None
    if all_iterations[0]['cpu_begin_ms'] < 0 or cpu_ms is None or cpu_ms <= 0:
        cpu_ms = None
    pace_last = all_iterations[-1]
    wall_us = pace_last['pace_update_us'] + pace_last['pace_draw_us']
    pace_tps = pace_last['pace_sim_ticks'] * 1e6 / wall_us if wall_us else None
    sim_cost = pace_last['pace_sim_us'] / 1000 / pace_last['pace_sim_ticks'] if pace_last['pace_sim_ticks'] else None
    lp = frames[-1]['local_prediction']
    lp_cost = lp['ms_total'] / lp['previews'] if lp['previews'] else None
    latency_ms = distribution([edge['ms'] for edge in latency if edge['ms'] is not None])
    latency_frames = distribution([edge['frames'] for edge in latency if edge['frames'] is not None])
    delays = sorted({frame['delay'] for frame in frames})
    large = [row for row in correction_rows if row['over_4_px']]
    window_sim = max_in_window([row['tick'] for row in large], 600)
    window_wall = max_in_window([row['commit_observed_wall_ms'] for row in large if row['commit_observed_wall_ms'] is not None], 10000)
    cap = frames[-1]['cap_hz']
    cpu_overhead = (cpu_ms / baseline['cpu_ms'] - 1) * 100 if cpu_ms and baseline and baseline['cpu_ms'] else None
    draw_ratio = draw['p99'] / baseline['draw_ms']['p99'] if baseline and baseline['draw_ms']['p99'] else None
    manifest = json.loads((run / 'manifest.json').read_text(encoding='utf-8'))
    for source in (() if expected_input_schedule is not None else ('input_script', 'input_schedule')):
        if file_record(manifest[source]['path'])['sha256'] != manifest[source]['sha256']:
            raise ValueError(f'{run}: {source} changed after launch')
    lag = manifest.get('lag_ms', 0) if windowed else manifest['lag_ms']
    schedule = expected_input_schedule if expected_input_schedule is not None else json.loads(Path(manifest['input_schedule']['path']).read_text(encoding='utf-8'))
    expected_inputs = {(row['tick'], row['action'], row['held']) for row in schedule['probes']}
    actual_inputs = {(row['tick'], change['action'], change['held']) for row in inputs for change in row['changes']}
    missing_inputs = sorted(expected_inputs - actual_inputs)
    over_50 = [frame['frame'] for frame in frames if max(frame['draw_ms'], frame['present_ms'], frame['interval_ms'] or 0) > 50]
    captures = [row for row in rows if row['type'] == 'capture']
    capture_missing = sorted(set(range(60, ticks + 1, 60) if expected_capture_ticks is None else expected_capture_ticks) - {row['requested_tick'] for row in captures if row['saved'] and Path(row['path']).is_file()})
    rtts = [row for frame in frames for row in frame['rtt']]
    auto_picks = []
    host_log = run / host_peer / 'stdout.log'
    if host_log.is_file():
        for line_no, text in enumerate(host_log.read_text(encoding='utf-8-sig', errors='replace').splitlines(), 1):
            match = AUTO_DELAY.match(text)
            if match:
                auto_picks.append(dict(peer=int(match[1]), rtt_ms=int(match[2]), delay=int(match[3]), floor=int(match[4]), raw_line=line_no))
    local_picks = [pick for pick in auto_picks if pick['peer'] == frames[-1]['peer']]
    network = item9a_gates(run, peer, rows) if not single_player else None
    measured_tick = network['metrics']['sim_tick_ms'] if network else SIM_MS
    delay_math = measured_tick is not None and measured_tick > 0 and all(
        pick['delay'] >= max(pick['floor'], math.ceil(pick['rtt_ms'] / measured_tick) + 1) for pick in local_picks)
    live_delay = network['metrics']['peer_input_delays'].get(str(frames[-1]['peer'])) if network else 0
    metrics = dict(cpu_ms=cpu_ms, draw_ms=draw, present_ms=present, frame_interval_ms=interval,
                   cpu_window=dict(first_iteration_tick=all_iterations[0]['tick'], last_iteration_tick=all_iterations[-1]['tick'],
                                   includes_match_stop_drain=True),
                   latency_ms=latency_ms, latency_frames=latency_frames,
                   latency_lower_bounds_ms=distribution([row['latency_lower_bound_ms'] for row in latency]),
                   missing_input_stamps=missing_inputs,
                   latency_edges=len(latency), latency_unreflected=sum(row['ms'] is None for row in latency),
                   firing_presses=len(firing), local_prediction=dict(lp, avg_ms=lp_cost),
                   delays=delays, input_delay_text=frames[-1]['input_delay_text'],
                   auto_picks=auto_picks, auto_pick_source=str(host_log),
                   rtt_ms=distribution([row['ping_ms'] for row in rtts if row['ping_ms'] > 0]),
                   pace_wall_tps=pace_tps, sim_ms_per_tick=sim_cost,
                   correction_over_4_px=len(large), correction_max_in_10_sim_seconds=window_sim,
                   correction_max_in_10_wall_seconds=window_wall, correction_missing=len(correction_missing),
                   cpu_overhead_percent=cpu_overhead, draw_p99_ratio=draw_ratio,
                   frames_over_50_ms=over_50, warp_frames=len(warps), warp_frames_over_4_px=sum(row['over_4_px'] for row in warps),
                   capture_missing=capture_missing, frame_count=len(frames), cap_hz=cap,
                   effective_hz=(len(frames) - 1) * 1000 / (frames[-1]['present_end_ms'] - frames[0]['present_end_ms']) if len(frames) > 1 else None)
    measured = bool(ended and coverage and latency and not missing_inputs and not duplicate_actor and len(firing) == schedule['fire_presses'] and cpu_ms is not None
                    and (auto_picks or single_player)
                    and not correction_missing and not capture_missing and (commands_complete or single_player))
    pins = {}
    pins['canonical_duplicate_actor'] = pin(duplicate_actor, 'no duplicate committed actor in the simdump', duplicate_actor is None,
                                            [canonical_dump])
    pins['wall_tps'] = pin(pace_tps, '>= 59.5', pace_tps is not None and pace_tps >= 59.5, [raw])
    pins['sim_ms_per_tick'] = pin(sim_cost, '<= 8 ms', sim_cost is not None and sim_cost <= 8, [raw])
    pins['auto_delay'] = pin(delays, 'initial picks cover ceil(measured RTT / measured sim tick) + 1; final draw names the committed live delay',
                             delay_math and frames[-1]['delay'] == live_delay and (single_player or '(auto' in frames[-1]['input_delay_text']),
                             [raw, host_log, run / f'{peer}_report.json'] if network else [raw])
    latency_value = dict(observed_ms=latency_ms, observed_frames=latency_frames,
                         lower_bounds_ms=metrics['latency_lower_bounds_ms'], unreflected=metrics['latency_unreflected'])
    pins['input_to_photon'] = pin(latency_value, '<= 34 ms at 60 Hz; <= one 60 Hz sim tick + one actual frame when uncapped',
                                  bool(latency) and all(row['pass_check'] for row in latency), [raw, paths['latencies']],
                                  'First submitted render copy with aim closer to the scripted aim, or velocity changed in the requested direction; swap-return boundary.')
    pins['preview_ms'] = pin(lp_cost, 'ms_total / previews <= 2 ms over the full negotiated delay', lp_cost is not None and lp_cost <= 2, [raw])
    pins['violations'] = pin(max(frame['local_prediction']['violations'] for frame in frames), '= 0 always',
                             all(frame['local_prediction']['violations'] == 0 for frame in frames), [raw])
    pins['correction_remote_only'] = pin(len(large), '> 4 px only on a remote-caused event',
                                         not correction_missing and bool(correction_rows) and not large,
                                         [raw, canonical_dump, controller, command_log, paths['corrections']],
                                         'Same-frame remote commands are listed; coincidence alone never proves causality.')
    pins['correction_rate'] = pin(dict(per_10_sim_seconds=window_sim, per_10_wall_seconds=window_wall), '<= 1 in every 10 s window',
                                  bool(correction_rows) and not correction_missing and max(window_sim, window_wall) <= 1, [raw, paths['corrections']])
    response = distribution([max(row['audio_ms_upper'], row['firing_ms_upper']) for row in firing if row['audio_ms_upper'] is not None])
    response['unobserved'] = sum(row['audio_ms_upper'] is None for row in firing)
    response['unobserved_lower_bounds_ms'] = [max(0, row['observation_end_ms'] - inputs_by_line['wall_ms'])
                                             for row in firing if row['audio_ms_upper'] is None
                                             for inputs_by_line in inputs if inputs_by_line['_line'] == row['input_line']]
    pins['firing_audio_ms'] = pin(response, '<= 34 ms after each press', bool(firing) and all(
        row['audio_ms_upper'] is not None and max(row['firing_ms_upper'], row['audio_ms_upper']) <= 34 for row in firing),
        [raw, run / peer / 'stdout.log', paths['firing']], 'Conservative preview-step wall interval; playback is muted on the private desktop.')
    pins['firing_audio_once'] = pin(sum(row['once'] for row in firing), f'exactly once for each of {len(firing)} presses',
                                    bool(firing) and all(row['once'] for row in firing), [run / peer / 'stdout.log', paths['firing']])
    pins['firing_preview_tick'] = pin(sum(row['preview_tick'] for row in firing), f'at the input preview tick for all {len(firing)} presses',
                                      bool(firing) and all(row['preview_tick'] for row in firing), [raw, paths['firing']])
    pins['draw_p99'] = pin(draw_ratio, '<= 1.5 x local SP D=0 p99', draw_ratio is not None and draw_ratio <= 1.5,
                           [raw, baseline['raw_path']] if baseline else [raw])
    pins['frame_max'] = pin(dict(max_ms=max(draw['max'], present['max'], interval['max'] or 0), count_over_50_ms=len(over_50)),
                            'no frame > 50 ms', not over_50, [raw])
    pins['cpu'] = pin(cpu_overhead, '<= 15 percent process CPU time over the same-cap D=0 SP match',
                      cpu_overhead is not None and cpu_overhead <= 15, [raw, baseline['raw_path']] if baseline else [raw])
    if single_player:
        measured = bool(ended and coverage and cpu_ms is not None and not capture_missing)
        pins = {name: pins[name] for name in ('auto_delay', 'violations', 'frame_max')}
    elif network:
        pins.update(network['pins'])
    if network:
        metrics.update(network['metrics'])
    result = dict(peer=peer, raw_path=str(raw), metrics=metrics, pins=pins, measurement_complete=measured,
                  trace_1200_ticks=coverage, orderly_end_record=ended, analysis={key: str(value) for key, value in paths.items()})
    write_json(destination / 'metrics.json', result)
    return result


def file_record(path):
    path = record_path(path)
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    return dict(path=str(path), bytes=path.stat().st_size, sha256=digest.hexdigest())
