"""HL4 evidence; ordinary all-peer workload/completion requirements stay separate."""
import math
import re

from compare_sim_traces import CORE
from . import report


def scheduled_moderation(manifest, peers, events, context, kill_tick, state):
    """Only the two X6 preparations, proved by their own applied and committed native receipts."""
    host = manifest['host']
    extras = [row for row in manifest.get('faults', []) if row.get('action') != 'host-kill']
    if not extras:
        return dict(passed=True, errors=[], holds=[], bans=[], removed={})
    silence = [row for row in extras if row.get('action') == 'silence']
    bans = [row for row in extras if row.get('action') == 'moderation-ban']
    if len(extras) != 2 or len(silence) != 1 or len(bans) != 1:
        return dict(passed=False, errors=['HL4 preparation must be exactly one silence and one moderation-ban'], holds=[], bans=[], removed={})
    silence, ban = silence[0], bans[0]
    errors, scheduled_holds, scheduled_bans, removed = [], [], [], {}

    def bound(row, owner, incarnation):
        return (type(incarnation) is int and row.get('instance') == owner and row.get('incarnation') == incarnation
                and isinstance(row.get('execution'), str) and row['execution'].startswith(f'process-{incarnation}/')
                and all(row.get(key) == value for key, value in context.items())
                and type(row.get('tick')) is int and 0 < row['tick'] < kill_tick)

    target = silence['peer']
    target_seats = {row.get('peer') for row in peers.get(target, {}).get('configs', [])}
    fired = [row for row in events.get(target, []) if row.get('type') == 'fault' and row.get('id') == silence['id']
             and row.get('action') in ('silence', 'outage') and row.get('applied') is True and row.get('send_recv_armed') is True
             and bound(row, target, silence.get('incarnation', 0)) and row.get('budget_tick', -1) >= silence['tick']]
    held = [row for row in events.get(host, []) if row.get('type') == 'scheduled_hold' and row.get('id') == silence['id']
            and row.get('cause') == 'silent' and row.get('peer') in target_seats and row.get('ai_in_control') is True
            and bound(row, host, ban.get('incarnation', 0)) and type(row.get('silence_ms')) in (int, float)
            and type(row.get('bound_ms')) in (int, float) and math.isfinite(row['silence_ms']) and math.isfinite(row['bound_ms'])
            and row['silence_ms'] > row['bound_ms'] > 0
            and any(receipt['tick'] <= row['tick'] for receipt in fired)]
    if len(fired) != 1 or len(held) != 1 or not isinstance(state, dict) or not any(
            row.get('seat') == held[0]['peer'] and row.get('cause') == 'silent' for row in state.get('held_seats', []) if isinstance(row, dict)):
        errors.append(f'{silence["id"]}: missing applied silence, host committed silent hold beyond the bound, or carried held-seat cause')
    else:
        scheduled_holds.append(dict(held[0], classification='SCHEDULED', scheduled_recovery_id=silence['id'],
                                    reason='HL4 scheduled silence: native applied fault and committed host silent hold beyond the bound'))
    target = ban.get('target_peer')
    target_seats = {row.get('peer') for row in peers.get(target, {}).get('configs', [])}
    applied = [row for row in events.get(host, []) if row.get('type') == 'moderation_action' and row.get('id') == ban['id']
               and row.get('action') == 'Ban' and row.get('applied') is True and row.get('target_peer') == target
               and row.get('target_seat') in target_seats and bound(row, host, ban.get('incarnation', 0))
               and row.get('budget_tick', -1) >= ban['tick'] and isinstance(row.get('identity_sha256'), str)
               and re.fullmatch('[0-9a-f]{64}', row['identity_sha256'])]
    receipt = applied[0] if len(applied) == 1 else {}
    terminal = [row for row in events.get(target, []) if row.get('type') == 'moderation_terminal' and row.get('id') == ban['id']
                and row.get('result') == 'ParticipantBanned' and row.get('terminal') is True
                and row.get('identity_sha256') == receipt.get('identity_sha256') and receipt
                and row.get('last_live_tick') == receipt['tick'] - 1
                and bound(row, target, ban.get('target_incarnation', 0))]
    target_record = peers.get(target, {}).get('record', {})
    valid_ban = (receipt and len(terminal) == 1 and isinstance(state, dict)
                 and receipt['identity_sha256'] in state.get('bans', [])
                 and type(target_record.get('exit_code')) is int and not target_record.get('timed_out'))
    if not valid_ban:
        errors.append(f'{ban["id"]}: missing host applied Ban, target terminal receipt, finished target process or carried ban identity')
    else:
        scheduled_bans.append(dict(receipt, classification='SCHEDULED', terminal=terminal[0]))
        removed[target] = receipt['tick'] - 1
    if target == host or target == silence['peer'] or ban['peer'] != host or silence['peer'] == host:
        errors.append('HL4 preparations target the host or the same seat')
    return dict(passed=not errors, errors=errors, holds=scheduled_holds, bans=scheduled_bans, removed=removed)


def scheduled_hold_classification(hold, host_loss):
    preparations = host_loss.get('preparation', {})
    candidates = [row for row in preparations.get('holds', []) if row.get('peer') == hold.get('peer')]
    candidates += [row for row in preparations.get('bans', []) if row.get('target_seat') == hold.get('peer')]
    matches = [row for row in candidates if row.get('tick') == hold.get('tick') and row.get('round') == hold.get('round')]
    if len(matches) != 1: return {}
    return dict(classification='SCHEDULED', scheduled_recovery_id=matches[0]['id'],
                reason='HL4 native committed preparation: ' + matches[0]['id'])


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
    preparation = scheduled_moderation(manifest, peers, events, dict(context, round=actual.get('round')), tick, state)
    errors.extend(preparation['errors'])
    removed = preparation['removed']
    survivors = [name for name in survivors if name not in removed]
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
            if name in removed and row.get('tick', 0) > removed[name]:
                errors.append(f'{name}: live tick {row.get("tick")} after its committed Ban boundary')
            if row.get('tick', 0) >= settled and (row.get('host_peer') != successor or row.get('authority_generation') != generation + 1):
                errors.append(f'{name}: tick {row.get("tick")} has host={row.get("host_peer")}, generation={row.get("authority_generation")}')
    first = 1
    if removed:
        last = next(iter(removed.values()))
        ranges.append(dict(context, first=first, last=last, peers=[host, *survivors, *removed]))
        first = last + 1
    ranges += [dict(context, first=first, last=tick, peers=[host, *survivors]),
               dict(context, first=tick + 1, last=bound, peers=survivors)]
    # X6: an actually banned participant ends at its native boundary; two continuing peers still satisfy Q1.
    history = report.compare_histories(live, ranges, CORE | {'controller'}, minimum_peers=2 if removed else 3)
    if not history['passed']:
        errors.append(f'history: unknown={history["unknown_keys"]}, unequal={history["unequal_keys"]}, invalid={history["invalid"]}, unexpected={history["unexpected_keys"]}')
    return dict(status='FAIL' if errors else 'PASS', passed=not errors, reason='; '.join(errors),
                ranges=ranges, termination=receipt, successor=successor, history=history, preparation=preparation, survivors=survivors,
                rule='HL4: actual host loss, continuing survivor history/feel, carried held causes, bans and tickets')
