"""HL4 evidence; ordinary all-peer workload/completion requirements stay separate."""
import math

from compare_sim_traces import CORE
from . import report


def host_loss_evidence(manifest, peers, events, live, terminations):
    faults = [f for f in manifest.get('faults', []) if f.get('action') == 'host-kill']
    if not faults:
        return dict(status='NOT APPLICABLE', passed=False, reason='no host-kill was scheduled', ranges=[])
    errors, ranges = [], []
    host = manifest.get('host')
    survivors = [p['name'] for p in manifest['instances'] if p['name'] != host]
    if len(faults) != 1 or faults[0].get('peer') != host or len(survivors) < 3:
        return dict(status='INCOMPLETE', passed=False, reason=f'host={host}, faults={faults}, survivors={survivors}', ranges=[])
    fault = faults[0]
    owned = [r for r in terminations if r.get('id') == fault['id'] and r.get('peer') == host
             and r.get('incarnation') == fault.get('incarnation', 0) and r.get('action') == 'host-kill']
    receipt = owned[0] if len(owned) == 1 else {}
    clock = [receipt.get('before_wall_ms'), receipt.get('after_wall_ms')]
    valid_clock = all(type(v) in (float, int) and math.isfinite(v) for v in clock) and clock[0] <= clock[1]
    actual = receipt.get('actual', {})
    tick = actual.get('applied_frame')
    bound = manifest.get('ticks')
    valid = (receipt.get('process_terminated') is True and type(receipt.get('engine_pid')) is int
             and receipt['engine_pid'] > 0 and type(receipt.get('exit_code')) is int and valid_clock
             and isinstance(receipt.get('execution'), str)
             and receipt['execution'].startswith(f'process-{fault.get("incarnation", 0)}/')
             and type(tick) is int and type(bound) is int and 0 < tick < bound
             and type(actual.get('budget_tick')) is int and actual['budget_tick'] >= fault['tick'])
    context = {key: actual.get(key) for key in report.HISTORY_FIELDS[:-1]}
    valid &= all(v is not None for v in context.values())
    if not valid:
        return dict(status='INCOMPLETE', passed=False, reason=f'owned completed termination receipt is absent or invalid: {receipt}', ranges=[])
    original_id, generation = actual.get('host_peer'), actual.get('authority_generation')
    if type(original_id) is not int or type(generation) is not int:
        return dict(status='INCOMPLETE', passed=False, reason=f'original host_peer={original_id}, authority_generation={generation}', ranges=[])
    owner_record = peers.get(host, {}).get('record', {})
    owner_pid = (owner_record.get('hop') or {}).get('engine_pid', owner_record.get('pid'))
    if (owner_pid != receipt['engine_pid'] or owner_record.get('exit_code') != receipt['exit_code']
            or owner_record.get('injected_termination') != 'scheduled crash ' + fault['id']):
        errors.append(f'{host}: termination PID/exit/injection differs from the owning runner record')
    before = [r for r in events.get(host, []) if r.get('type') == 'moderation_snapshot' and r.get('stage') == 'before'
              and all(r.get(k) == v for k, v in context.items()) and r.get('tick') == tick
              and r.get('host_peer') == original_id and r.get('authority_generation') == generation
              and r.get('incarnation') == receipt['incarnation'] and r.get('execution') == receipt['execution']
              and r.get('instance') == host]
    state = before[0].get('state') if len(before) == 1 else None
    if not isinstance(state, dict) or any(not isinstance(state.get(k), list) for k in ('held_seats', 'bans', 'tickets')):
        errors.append('pre-termination moderation snapshot lacks held_seats, bans or tickets')
    elif not state['held_seats'] or not state['bans'] or not state['tickets'] or any(
            not isinstance(r, dict) or r.get('seat') is None or not r.get('cause') for r in state['held_seats']):
        errors.append('pre-termination snapshot lacks exercised held causes, bans or tickets')
    elif any(not isinstance(r, dict) or type(r.get('seat')) is not int or type(r.get('incarnation')) is not int
             or not isinstance(r.get('identity_sha256'), str) or len(r['identity_sha256']) != 64 for r in state['tickets']):
        errors.append('pre-termination ticket identity/seat/incarnation is incomplete')
    successor_ids, after_ticks = set(), []
    for name in survivors:
        after = [r for r in events.get(name, []) if r.get('type') == 'moderation_snapshot' and r.get('stage') == 'after'
                 and all(r.get(k) == v for k, v in context.items()) and type(r.get('tick')) is int
                 and tick < r['tick'] <= bound and r.get('instance') == name
                 and r.get('execution') is not None and type(r.get('incarnation')) is int]
        if not after:
            errors.append(f'{name}: post-migration moderation snapshot absent')
        for row in after:
            if row.get('state') != state or generation is None or row.get('authority_generation') != generation + 1:
                errors.append(f'{name}: post-migration moderation state or authority differs')
            successor_ids.add(row.get('host_peer')); after_ticks.append(row['tick'])
        peer = peers.get(name, {})
        if peer.get('feel_gated') is not True or peer.get('feel_pass') is not True:
            errors.append(f'{name}: feel_gated={peer.get("feel_gated")}, feel_pass={peer.get("feel_pass")}')
        if (peer.get('record', {}).get('exit_code') != 0 or peer.get('record', {}).get('timed_out')
                or peer.get('native_completion', {}).get('completion') != 'completed'):
            errors.append(f'{name}: successful native completion absent')
    numeric_survivors = {c.get('peer') for n in survivors for c in peers.get(n, {}).get('configs', [])}
    successor = next(iter(successor_ids)) if len(successor_ids) == 1 else None
    if successor is None or successor == original_id or successor not in numeric_survivors:
        errors.append(f'successor host receipts={sorted(successor_ids, key=str)}, survivor IDs={numeric_survivors}')
    settled = max(after_ticks, default=bound + 1)
    for name, rows in live.items():
        for row in rows:
            if name == host and row.get('tick', 0) > tick:
                errors.append(f'{name}: live tick {row.get("tick")} after termination tick {tick}')
            if row.get('tick', 0) >= settled and (row.get('host_peer') != successor or row.get('authority_generation') != generation + 1):
                errors.append(f'{name}: tick {row.get("tick")} has host={row.get("host_peer")}, generation={row.get("authority_generation")}')
    ranges = [dict(context, first=1, last=tick, peers=[host, *survivors]),
              dict(context, first=tick + 1, last=bound, peers=survivors)]
    history = report.compare_histories(live, ranges, CORE | {'controller'})
    if not history['passed']:
        errors.append(f'history: unknown={history["unknown_keys"]}, unequal={history["unequal_keys"]}, invalid={history["invalid"]}, unexpected={history["unexpected_keys"]}')
    return dict(status='FAIL' if errors else 'PASS', passed=not errors, reason='; '.join(errors),
                ranges=ranges, termination=receipt, successor=successor, history=history,
                rule='HL4: actual host loss, continuing survivor history/feel, carried held causes, bans and tickets')
