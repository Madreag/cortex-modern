"""Five A7 socket gates on one pinned approved build; missing evidence is a failed result."""

from __future__ import annotations

import argparse
import datetime
import json
from pathlib import Path
import time
import uuid

from a7_support import (COMMON_CAPABILITIES, HERE, REPO, EvidenceGap, GateError, Group, GuardedFactory,
                        Pin, PinError, SHA, at, block, clean_environment, count, digest, integer,
                        read_bytes, read_json, require, string, validate_output, write_json, DECISION_SOURCES)


ARMS = {
    'stagger_seat_survives': {'folder': 'a', 'port': 44510, 'frames': 900, 'capabilities': set()},
    'silent_socket_bound': {'folder': 'c', 'port': 44512, 'frames': 12000, 'preset': 'Net Lifecycle Test',
                            'capabilities': {'connect_gate', 'handshake_age', 'drop', 'reclaim', 'resumption'}},
    'leave_ack_dropped': {'folder': 'd', 'port': 44514, 'frames': 900, 'capabilities': {'leave_exchange', 'leave_queue_clock_v1'}},
    'slow_resync_save': {'folder': 'e', 'port': 44516, 'frames': 12000, 'preset': 'Net Lifecycle Test',
                         'capabilities': {'connect_gate', 'drop', 'reclaim', 'save_gap', 'resumption'}},
    'coop_hand_back': {'folder': 'f', 'port': 44518, 'frames': 12000,
                       'capabilities': {'connect_gate', 'drop', 'reclaim', 'owner_observation', 'local_player_view_v1', 'resumption'}},
}
P14_MS, P21_MS, HOLD_MS = 5000, 2000, 20000


def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def args_for(spec, ticket, host=False, leave=False, resync=False, coop=False):
    args = ['-net-match-service-e2e', '-net-port', str(spec['port']), '-net-match-ticks', str(spec['frames']),
            '-net-reconnect-ticket', str(ticket), '-net-match-peers', '2', '-net-match-mode',
            'coop-pve' if coop else 'pvp-skirmish', '-seed', '42']
    args += ['-net-host'] if host else ['-net-join', '127.0.0.1']
    if leave:
        args += ['-net-match-e2e-leave']
    if resync:
        args += ['-net-match-e2e-resync']
    if spec.get('preset'):
        args += ['-module', 'Tests.rte', '-net-match-service-preset', spec['preset']]
    return args


def rec(report):
    return block(report, 'service', 'reconnect')


def session(report):
    return block(report, 'service', 'runner', 'session')


def lockstep(report):
    return block(report, 'service', 'runner', 'lockstep')


def event_number(event, key, minimum=0):
    return integer(event.get(key), f'{event.get("event")}.{key}', minimum)


def decision_ms(event):
    require(event.get('decision_clock') == 'authority_session' and
            event.get('decision_source') == DECISION_SOURCES.get(event.get('event')),
            f'{event.get("event")}: missing actual decision clock', EvidenceGap)
    return event_number(event, 'session_ms')


def rows(value, label):
    require(type(value) is list and all(type(row) is dict for row in value), f'{label}: expected object array', EvidenceGap)
    return value


def commit(peer, reused=False):
    value = peer.one('commit')
    require(value.get('used_stored_ticket') is reused, f'{peer.name}: wrong commit kind')
    for key in ('stable_seat', 'peer_id', 'incarnation', 'holder_generation'):
        event_number(value, key, 1)
    return value


def running(group, peer, new_round=None):
    def candidates():
        return [row for row in peer.events('running') if new_round is None or row.get('round_id') != new_round]
    group.wait(f'{peer.name}.running', lambda: bool(candidates()), [peer])
    event = candidates()[-1]
    for key in ('round_id', 'frame', 'peer_id'):
        event_number(event, key, 1)
    return event


def progress(group, peer, round_id, after_frame, minimum=60):
    def candidates():
        return [row for row in peer.events('progress', round_id=round_id)
                if event_number(row, 'frame', 1) >= after_frame + minimum]
    group.wait(f'{peer.name}.applied_frames', lambda: bool(candidates()), [peer])
    return candidates()[-1]


def saved_ticket(path):
    raw = read_bytes(path, 65536)
    require(bool(raw), f'empty retained ticket: {path}', EvidenceGap)
    return digest(path)


def release(group, path, peers):
    require(all(peer.record is None for peer in peers), 'connection barrier peer ended before release')
    payload = {'schema': 1, 'run_id': group.run_id, 'peers': [peer.name for peer in peers]}
    write_json(path, payload, exclusive=True)
    group.barriers.append({'name': 'connect_release', 'path': str(path), 'sha256': digest(path),
                           'observed_monotonic': group.clock.monotonic(), 'peers': payload['peers']})


def pair(group, spec, *, fault='', leave=False, resync=False, coop=False, stagger=False):
    host = group.start('host', args_for(spec, group.root / 'host.ticket', host=True, resync=resync, coop=coop), fault=fault)
    group.wait('host.listener_bound', lambda: bool(host.events('listening', port=spec['port'])), [host])
    if stagger:
        group.delay('host.listening_for_eight_seconds', 8, [host])
    ticket = group.root / 'client.ticket'
    client = group.start('client', args_for(spec, ticket, leave=leave, resync=resync, coop=coop),
                         expected_exit=137 if resync else 0)
    group.wait('client.committed', lambda: bool(client.events('commit')), [host, client])
    identity = commit(client)
    ticket_sha = saved_ticket(ticket)
    host_running, client_running = running(group, host), running(group, client)
    require(host_running['round_id'] == client_running['round_id'], 'initial peers run different rounds')
    require(client_running['peer_id'] == identity['peer_id'], 'committed peer differs from running peer')
    return host, client, {'ticket': str(ticket), 'ticket_before_sha256': ticket_sha, 'identity': identity,
                          'initial_round': client_running['round_id'], 'initial_frame': client_running['frame']}


def warm_returner(group, spec, context, coop=False):
    gate = group.root / 'returner-go.json'
    returner = group.start('returner', args_for(spec, Path(context['ticket']), resync=True, coop=coop),
                           environment={'CC_A7_CONNECT_GATE': str(gate)})
    group.wait('returner.loaded_before_drop', lambda: bool(returner.events('connect_waiting')), [returner])
    waiting = returner.one('connect_waiting')
    require(waiting.get('ticket_sha256') == context['ticket_before_sha256'], 'returner did not load the retained ticket')
    return returner, gate


def drop_and_return(group, host, client, returner, gate, context):
    progress(group, client, context['initial_round'], context['initial_frame'])
    require(saved_ticket(Path(context['ticket'])) == context['ticket_before_sha256'], 'ticket changed before deliberate drop')
    group.drop(client)
    seat = context['identity']['stable_seat']
    group.wait('host.adjudicated_drop', lambda: bool(host.events('drop', stable_seat=seat)), [host, returner])
    dropped = host.one('drop', stable_seat=seat)
    require(dropped.get('reason') == 'connection lost' and dropped.get('peer_id') == context['identity']['peer_id'],
            'host drop is not the intended client connection loss')
    event_number(dropped, 'frame', context['initial_frame'] + 1)
    context['drop'] = dropped
    release(group, gate, [returner])
    group.wait('returner.reclaim_committed', lambda: bool(returner.events('commit')), [host, returner])
    returned = commit(returner, reused=True)
    loaded = returner.one('ticket_loaded')
    require(type(loaded.get('load_result')) is int and loaded['load_result'] == 0 and loaded.get('host_matches') is True and
            loaded['seq'] < returned['seq'] and loaded.get('ticket_sha256') == returned.get('loaded_ticket_sha256') == context['ticket_before_sha256'],
            'returner admission did not use the same bytes as its retained-ticket preload')
    for field in ('stable_seat', 'peer_id', 'holder_generation'):
        require(returned[field] == context['identity'][field], f'returner changed {field}')
    require(returned['incarnation'] > context['identity']['incarnation'], 'reclaim did not advance the incarnation')
    group.wait('host.reclaim_committed', lambda: bool(host.events('reclaim', stable_seat=seat)), [host, returner])
    accepted = host.one('reclaim', stable_seat=seat)
    for field in ('peer_id', 'incarnation', 'holder_generation'):
        require(event_number(accepted, field, 1) == returned[field], f'host reclaim changed {field}')
    elapsed = decision_ms(accepted) - decision_ms(dropped)
    require(0 <= elapsed < HOLD_MS, f'reclaim missed the pinned hold window: {elapsed} ms')
    new_host, new_client = running(group, host, context['initial_round']), running(group, returner)
    require(new_host['round_id'] == new_client['round_id'] and new_host['round_id'] != context['initial_round'],
            'resumed peers did not enter the same new round')
    context['resumed_round'] = new_host['round_id']
    progress(group, host, new_host['round_id'], new_host['frame'])
    progress(group, returner, new_client['round_id'], new_client['frame'])


def run_stagger(group, spec):
    host, client, context = pair(group, spec, stagger=True)
    progress(group, host, context['initial_round'], context['initial_frame'])
    progress(group, client, context['initial_round'], context['initial_frame'])
    group.finish()
    return context


def run_leave(group, spec):
    host, client, context = pair(group, spec, fault='drop_leave_ack', leave=True)
    group.wait('client.leave_exchange_began', lambda: bool(client.events('leave_begin')), [host, client])
    begun = client.one('leave_begin')
    require(event_number(begun, 'frame') == 300, 'leave was not the declared tick-300 trigger')
    group.finish()
    client.one('leave_settled')
    return context


def run_slow(group, spec):
    host, client, context = pair(group, spec, fault='slow_resync_save', resync=True)
    returner, gate = warm_returner(group, spec, context)
    drop_and_return(group, host, client, returner, gate, context)
    group.finish()
    return context


def run_coop(group, spec):
    host, client, context = pair(group, spec, resync=True, coop=True)
    group.wait('client.live_owned_unit', lambda: bool(client.events('unit_owner', owner_peer_id=context['identity']['peer_id'], team=0, alive=True)),
               [host, client])
    owned = client.events('unit_owner', owner_peer_id=context['identity']['peer_id'], team=0, alive=True)[-1]
    context['unit_uid'] = event_number(owned, 'uid', 1)
    context['local_views'] = {}
    for peer in (host, client):
        group.wait(f'{peer.name}.initial_local_control', lambda peer=peer: bool(peer.events('local_control', round_id=context['initial_round'])), [host, client])
        view = peer.events('local_control', round_id=context['initial_round'])[0]
        validate_local_control(view, view)
        context['local_views'][peer.name] = view
    require(context['local_views']['client']['controlled_uid'] == context['unit_uid'], 'fixture did not observe the client\'s selected unit')
    returner, gate = warm_returner(group, spec, context, coop=True)
    drop_and_return(group, host, client, returner, gate, context)
    for peer in (host, returner):
        group.wait(f'{peer.name}.unit_handed_back', lambda peer=peer: bool(peer.events('unit_owner', uid=context['unit_uid'],
            owner_peer_id=context['identity']['peer_id'], team=0, alive=True, round_id=context['resumed_round'])), [host, returner])
        group.wait(f'{peer.name}.resumed_local_control', lambda peer=peer: bool(peer.events('local_control', round_id=context['resumed_round'])), [host, returner])
        expected = context['local_views']['host' if peer is host else 'client']
        for observed in peer.events('local_control', round_id=context['resumed_round'])[:61]:
            validate_local_control(observed, expected)
    group.finish()
    return context


def validate_local_control(observed, expected):
    for field in ('controlled_uid', 'brain_uid'):
        event_number(observed, field, 1)
    require(observed.get('player_active') is True and observed.get('player_human') is True,
            'local player is inactive or not human after reload')
    for field in ('peer_id', 'team', 'screen', 'controlled_uid', 'brain_uid', 'seat_mode', 'seat_player'):
        require(type(observed.get(field)) is int and type(expected.get(field)) is int and observed[field] == expected[field],
                f'local control changed {field}: expected {expected.get(field)}, observed {observed.get(field)}')
    require(observed['seat_mode'] == 1 and observed['seat_player'] == 0, 'selected actor is not seated at the local player')


def run_silent(group, spec):
    host, client, context = pair(group, spec, resync=True)
    returner, return_gate = warm_returner(group, spec, context)
    gate = group.root / 'silent-go.json'
    tag_base = int(group.run_id[:8], 16) & 0x7fffffff
    silent = [group.start(f'silent{index}', args_for(spec, group.root / f'silent{index}.ticket'),
        fault='client_never_says_hello', expected_exit=1, environment={
            'CC_A7_CONNECT_GATE': str(gate), 'CC_A7_SILENT_HEARTBEAT_MS': '4000',
            'CC_A7_SILENT_RECEIVE_BUDGET_MS': '30000',
            'CC_A7_SILENT_HEARTBEAT_TAG': str((tag_base ^ index) + 1)}) for index in range(8)]
    group.wait('eight_clients_loaded', lambda: all(peer.events('connect_waiting') for peer in silent), [host, client, *silent])
    context['silent_release_host_seq'] = host.journal.events[-1]['seq']
    release(group, gate, silent)
    group.wait('eight_host_handshakes_opened', lambda: len([row for row in host.events('handshake_open')
        if row['seq'] > context['silent_release_host_seq']]) >= 8, [host, client])
    group.wait('eight_host_handshakes_expired', lambda: len(host.events('handshake_expired')) >= 8, [host, client])
    context['expiry_barrier_monotonic'] = group.clock.monotonic()
    seat = context['identity']['stable_seat']
    expiry_seq = max(row['seq'] for row in host.events('handshake_expired'))
    def fresh_holder():
        return [event for event in host.events('progress', round_id=context['initial_round']) if event['seq'] > expiry_seq and
            any(type(row.get('stable_seat')) is int and row['stable_seat'] == seat and row.get('committed') is True and row.get('dropped') is False and row.get('closed') is False
                for row in rows(at(event, 'seats'), 'progress.seats'))]
    group.wait('holder_survived_p14_window', lambda: bool(fresh_holder()), [host, client])
    context['holder_after_expiry'] = fresh_holder()[-1]
    require(not host.events('drop', stable_seat=seat), 'P14 dropped the seated holder')
    drop_and_return(group, host, client, returner, return_gate, context)
    group.finish()
    return context


RUNNERS = dict(zip(ARMS, (run_stagger, run_silent, run_leave, run_slow, run_coop)))


def compare_progress(first, second, round_id):
    def indexed(peer):
        rows = peer.events('progress', round_id=round_id)
        result = {}
        for row in rows:
            frame = event_number(row, 'frame', 1)
            require(frame not in result, f'{peer.name}: duplicate applied-frame evidence')
            require(type(row.get('shared_hash')) is str and bool(SHA.fullmatch(row['shared_hash'])),
                    f'{peer.name}: missing approved simulation checksum at applied frame {frame}', EvidenceGap)
            result[frame] = row['shared_hash']
        require(bool(result), f'{peer.name}: no applied-frame evidence for required round', EvidenceGap)
        ordered = sorted(result)
        require(ordered == list(range(ordered[0], ordered[-1] + 1)), f'{peer.name}: applied-frame evidence has holes', EvidenceGap)
        started = peer.one('running', round_id=round_id)
        require(ordered[0] == event_number(started, 'frame', 1), f'{peer.name}: applied-frame evidence misses round start', EvidenceGap)
        native = lockstep(read_json(peer.report_path))
        require(count(native, 'round_id') == round_id and ordered[-1] + 1 == count(native, 'next_frame', minimum=1),
                f'{peer.name}: applied-frame evidence does not reach the native terminal frame', EvidenceGap)
        return result
    left, right = indexed(first), indexed(second)
    overlap = sorted(set(left) & set(right))
    require(len(overlap) >= 61 and overlap == list(range(overlap[0], overlap[-1] + 1)),
            'insufficient consecutive shared applied frames after the required barrier', EvidenceGap)
    require(all(left[frame] == right[frame] for frame in overlap), 'shared state diverged on an observed applied frame')
    return {'round_id': round_id, 'matched_frame_count': len(overlap), 'first_frame': overlap[0], 'last_frame': overlap[-1]}


def validate_reclaim(group, reports, context, maximum_resumptions=2):
    host, returner = reports['host'], reports['returner']
    r = rec(returner)
    require(r.get('client_used_stored_ticket') is True and r.get('client_state') == 'Joined', 'returner did not finish a stored-ticket reclaim')
    count(r, 'client_commits', minimum=1)
    count(block(session(host), 'admission'), 'reclaims_accepted', expected=1)
    count(rec(host), 'host_seats_dropped', expected=1)
    count(rec(host), 'host_ledger_drops_recorded', minimum=1)
    count(rec(host), 'clock_divergence_max_ms', expected=0)
    for report in (host, returner):
        stats = block(session(report), 'stats')
        require(count(stats, 'timeout_resumptions') <= maximum_resumptions, 'too many timeout resumptions')
        require(count(lockstep(report), 'round_id', minimum=1) == context['resumed_round'], 'native report names the wrong resumed round')
        count(lockstep(report), 'peers_dropped_silent', expected=0)
        require(at(lockstep(report), 'peer_leave_frames') == {}, 'resumed round retained or added a peer leave')
        count(rec(report), 'census_refusals', expected=0)
    count(host, 'resyncs', expected=1)
    return compare_progress(group.peers['host'], group.peers['returner'], context['resumed_round'])


def validate_arm(name, group, spec, context):
    reports = {}
    for peer in group.peers.values():
        reports[peer.name] = group.validate_peer(peer, spec['frames'], rejected=peer.name.startswith('silent'),
            minimum_ticks=299 if name == 'leave_ack_dropped' else None)
    result = {}
    host, hr = reports['host'], rec(reports['host'])
    count(hr, 'clock_divergence_max_ms', expected=0)
    if name == 'stagger_seat_survives':
        client = reports['client']
        require(rec(client).get('client_state') == 'Joined', 'stagger client is not joined')
        count(rec(client), 'client_commits', expected=1)
        count(hr, 'host_seats_dropped', expected=0)
        seats = rows(at(hr, 'seats'), 'reconnect.seats')
        require(any(type(row.get('stable_seat')) is int and row['stable_seat'] == context['identity']['stable_seat'] and
            type(row.get('lockstep_peer_id')) is int and row['lockstep_peer_id'] == context['identity']['peer_id'] and row.get('committed') is True and
            row.get('dropped') is False and row.get('closed') is False for row in seats), 'stagger client seat did not survive')
        for report in (host, client):
            count(block(session(report), 'stats'), 'timeouts', expected=0)
        result['shared_progress'] = compare_progress(group.peers['host'], group.peers['client'], context['initial_round'])
    elif name == 'leave_ack_dropped':
        client, cr, peer = reports['client'], rec(reports['client']), group.peers['client']
        count(cr, 'client_leave_acks', expected=0)
        count(cr, 'client_unacknowledged_leaves', expected=1)
        count(cr, 'ticket_clears', expected=0)
        count(cr, 'client_confirmed_session_ends', expected=0)
        require(cr.get('ticket_stored') is True and saved_ticket(Path(context['ticket'])) == context['ticket_before_sha256'], 'unacknowledged leave lost or changed the ticket')
        count(block(session(host), 'admission'), 'seats_closed_by_leave', expected=1)
        require(any(type(row.get('stable_seat')) is int and row['stable_seat'] == context['identity']['stable_seat'] and row.get('closed') is True and row.get('committed') is False
                    for row in rows(at(hr, 'seats'), 'reconnect.seats')), 'leave did not close the intended stable seat')
        began, settled = peer.one('leave_begin'), peer.one('leave_settled')
        for peer_name, report in (('host', host), ('client', client)):
            native = lockstep(report)
            require(at(report, 'service', 'state') == 'Completed' and native.get('state') == 'Stopped' and
                    native.get('timeout_reason') == 'PeerLeft:Match left', f'{peer_name}: leave did not finish through the announced departure')
            require(count(native, 'next_frame') == event_number(began, 'frame') + 1 and
                    count(native, 'frames_accepted') == count(native, 'next_frame') - count(native, 'effective_start_frame') and
                    count(report, 'running_ticks') + 1 == count(native, 'frames_accepted'),
                    f'{peer_name}: leave terminal frame coverage differs from its trigger')
            count(native, 'timeouts', expected=0)
            count(native, 'unresolved_observation_packets', expected=0)
        result['shared_progress'] = compare_progress(group.peers['host'], peer, context['initial_round'])
        tx = string(began.get('transaction'), 'leave transaction')
        require(settled.get('transaction') == tx and settled.get('unacknowledged') is True and settled.get('acked') is False,
                'leave settlement does not describe this unacknowledged exchange')
        sends = peer.events('leave_send', transaction=tx)
        require(len(sends) >= 2, 'no observed leave-phase retransmission')
        require([event_number(row, 'ordinal') for row in sends] == list(range(len(sends))), 'leave send sequence has a gap')
        queued = peer.events('leave_queued', transaction=tx)
        require(len(queued) == len(sends) and [event_number(row, 'ordinal') for row in queued] == list(range(len(sends))),
                'leave queue observations do not match successful transport sends')
        require(all(decision_ms(later) - decision_ms(earlier) >= 250 for earlier, later in zip(queued, queued[1:])),
                'leave retransmissions bypassed the pinned 250 ms interval')
        require(all(0 <= decision_ms(queued_row) - decision_ms(began) < P21_MS and
                    queued_row['seq'] < sent_row['seq'] and decision_ms(queued_row) <= decision_ms(sent_row) and
                    0 <= decision_ms(sent_row) - decision_ms(began) < P21_MS for queued_row, sent_row in zip(queued, sends)),
                'leave retry occurred outside P21 or before its queue decision')
        require(P21_MS <= decision_ms(settled) - decision_ms(began) and event_number(settled, 'previous_tick_elapsed_ms') < P21_MS,
                'leave did not settle at the first evaluated P21 boundary')
        require(count(cr, 'client_retransmits') - event_number(began, 'client_retransmits') == len(sends) - 1,
                'native retransmits do not match the observed leave exchange')
        suppressed = group.peers['host'].events('leave_ack_suppressed', transaction=tx)
        require(bool(suppressed) and all(event_number(row, 'stable_seat', 1) == context['identity']['stable_seat'] and
            row.get('closed') is True and row.get('committed') is False for row in suppressed),
                'host did not close the seat while suppressing this LeaveAck')
        result['ambiguity_semantics'] = {'client_ambiguous_losses': count(cr, 'client_ambiguous_losses'),
            'client_unacknowledged_leaves': cr['client_unacknowledged_leaves'],
            'leave_phase_retransmits': len(sends) - 1,
            'literal_f1_9_ambiguous_counter_proven': cr['client_ambiguous_losses'] >= 1}
        if cr['client_ambiguous_losses'] < 1:
            raise EvidenceGap('F1.9 evidence gap: production records unacknowledgedLeaves but does not increment ambiguousLosses; reconcile the counter contract explicitly', result)
    else:
        result['shared_progress'] = validate_reclaim(group, reports, context, 1 if name == 'silent_socket_bound' else 2)
        if name == 'silent_socket_bound':
            opens = [row for row in group.peers['host'].events('handshake_open')
                     if context['silent_release_host_seq'] < row['seq'] < context['drop']['seq']]
            expired = group.peers['host'].events('handshake_expired')
            require(len(opens) == len(expired) == 8, 'not exactly eight opened and host-expired handshakes')
            ids = [string(row.get('connection'), 'connection identity') for row in opens]
            require(len(set(ids)) == 8 and {row.get('connection') for row in expired} == set(ids), 'expiry did not cover every distinct connection')
            require(max(decision_ms(row) for row in opens) < min(decision_ms(row) for row in expired), 'eight unauthenticated connections never coexisted')
            tags = {int(peer.environment['CC_A7_SILENT_HEARTBEAT_TAG']): peer for peer in group.peers.values() if peer.name.startswith('silent')}
            require(len(tags) == 8, 'silent heartbeat tags are not unique')
            observed_tags = set()
            for opened in opens:
                connection = opened['connection']
                ended = next(row for row in expired if row['connection'] == connection)
                require(ended.get('reason') == 'client hello timeout' and event_number(ended, 'timeout_ms') == P14_MS and
                    event_number(ended, 'age_ms') > P14_MS and event_number(ended, 'previous_check_age_ms') <= P14_MS,
                    f'{connection}: not first host expiry after the P14 connection-age budget')
                require(decision_ms(ended) - decision_ms(opened) == ended['age_ms'], 'connection age does not match host event time')
                beats = group.peers['host'].events('handshake_heartbeat', connection=connection)
                require(len(beats) == 1 and 4000 <= decision_ms(beats[0]) - decision_ms(opened) < P14_MS, f'{connection}: missing single four-second heartbeat')
                tag = event_number(beats[0], 'sequence', 1)
                require(tag in tags and tag not in observed_tags, f'{connection}: heartbeat does not identify a distinct launched silent peer')
                sent = tags[tag].one('silent_heartbeat_sent')
                require(event_number(sent, 'sequence', 1) == tag and 4000 <= event_number(sent, 'connected_age_ms') < P14_MS,
                        f'{connection}: host heartbeat does not match this client socket')
                observed_tags.add(tag)
                require(not any(row['seq'] < ended['seq'] for row in group.peers['host'].events('handshake_closed', connection=connection)),
                        f'{connection}: closed before host P14 expiry')
                require(not group.peers['host'].events('handshake_accepted', connection=connection), f'{connection}: unexpectedly sent a ClientHello')
            count(block(session(host), 'stats'), 'timeouts', expected=8)
            count(block(session(host), 'stats'), 'unauthenticated_connections_refused', expected=0)
            count(block(session(reports['returner']), 'stats'), 'timeouts', expected=0)
            survived = context['holder_after_expiry']
            require(max(row['seq'] for row in expired) < survived['seq'] < context['drop']['seq'] and
                    survived in group.peers['host'].events('progress', round_id=context['initial_round']),
                    'holder survival evidence is stale or from another round')
            result['expired_connections'] = ids
        elif name == 'slow_resync_save':
            for report in (host, reports['returner']):
                count(block(session(report), 'stats'), 'timeouts', expected=0)
            begin, end = group.peers['host'].one('save_begin'), group.peers['host'].one('save_end')
            resync = string(begin.get('resync'), 'save resync identity')
            require(resync == end.get('resync') and end.get('fault') == 'slow_resync_save' and
                    event_number(end, 'elapsed_ms') == decision_ms(end) - decision_ms(begin) and end['elapsed_ms'] >= 7000,
                    'the host save gap did not exercise the armed seven-second delay')
            resumed = group.peers['host'].events('session_resumed')
            count(block(session(host), 'stats'), 'timeout_resumptions', minimum=1)
            require(any(decision_ms(row) >= decision_ms(end) and row.get('resync') == resync and
                        event_number(row, 'evaluation_gap_ms') > P14_MS for row in resumed),
                    'no timeout-window resumption after the observed save gap')
            result['save_elapsed_ms'] = end['elapsed_ms']
        else:
            for report in (host, reports['returner']):
                config = block(report, 'service', 'runner', 'match_config')
                require(config.get('mode') == 'coop-pve', 'co-op mode did not parse to coop-pve')
                players = rows(at(config, 'players'), 'match_config.players')
                require(all(type(row.get('cpu')) is bool for row in players), 'missing human/CPU roster evidence', EvidenceGap)
                humans = [row for row in players if row['cpu'] is False]
                require(len(humans) == 2 and all(type(row.get('team')) is int and row['team'] == 0 for row in humans), 'co-op humans do not both occupy team zero')
                count(block(session(report), 'stats'), 'timeouts', expected=0)
            for key in ('host_seats_dropped', 'host_ledger_drops_recorded', 'host_reseats_issued'):
                count(hr, key, minimum=1)
            count(hr, 'host_reseats_without_a_ledger', expected=0)
            count(hr, 'host_reseats_without_survivors', expected=0)
            count(hr, 'host_reseat_live_on_team_not_named')
            ledger = at(context['drop'], 'ledger_uids')
            require(type(ledger) is list and all(type(uid) is int and uid > 0 for uid in ledger), 'drop ledger UID evidence is malformed', EvidenceGap)
            require(context['unit_uid'] in ledger, 'observed unit was never recorded in the drop ledger')
            for peer, round_id in ((group.peers['client'], context['initial_round']), (group.peers['host'], context['resumed_round']),
                                   (group.peers['returner'], context['resumed_round'])):
                require(bool(peer.events('unit_owner', uid=context['unit_uid'], owner_peer_id=context['identity']['peer_id'],
                                         team=0, alive=True, round_id=round_id)), f'{peer.name}: the same living unit was not observed under the returning owner')
            require('[net-reconnect] reseating team' in read_bytes(group.peers['host'].out / 'stdout.log').decode('utf-8-sig'), 'issued reseat has no host log')
            result['handed_back_unit_uid'] = context['unit_uid']
    return result


def run_suite(root, source, manifest, selected, *, pin_factory=Pin, factory=None, clock=time):
    root = Path(root)
    root.mkdir(parents=True, exist_ok=False)
    run_id = uuid.uuid4().hex
    summary = {'schema': 1, 'run_id': run_id, 'source': source, 'build_manifest': str(manifest),
               'started': now(), 'finished': None, 'complete': False, 'execution_pass': False,
               'expected_arms': list(selected), 'arms': [], 'stop_reason': None}
    write_json(root / 'result.json', summary, exclusive=True)
    pin, factory = None, factory or GuardedFactory()
    try:
        pin = pin_factory(source, manifest)
        summary['pin'] = pin.verify(artifacts=True)
        write_json(root / 'pin.json', {**summary['pin'], 'driver_inputs': {str(path): sha for path, sha in pin.driver_inputs.items()}}, exclusive=True)
        for name in selected:
            spec, group = ARMS[name], None
            row = {'arm': name, 'attempted': False, 'passed': False, 'started': now(), 'failures': [], 'evidence_gaps': []}
            try:
                require(summary['stop_reason'] is None, f'unattempted after unsafe preflight: {summary["stop_reason"]}', PinError)
                row['source_before'] = pin.verify()
                require(factory.engine_count() == 0, 'another engine is running; no A7 launch is permitted', PinError)
                capabilities = COMMON_CAPABILITIES | spec['capabilities']
                pin.capabilities(capabilities)
                arm_root = root / spec['folder']
                arm_root.mkdir(exist_ok=False)
                group = Group(arm_root, run_id, pin, factory, capabilities, clock=clock)
                row['attempted'] = True
                context = RUNNERS[name](group, spec)
                row['context'] = context
                group.close()
                require(not group.cleanup_errors, f'owned job cleanup failed: {group.cleanup_errors}', PinError)
                row['observations'] = validate_arm(name, group, spec, context)
                row['passed'] = True
            except EvidenceGap as error:
                row['evidence_gaps'].append(str(error))
                row['failures'].append(str(error))
                if error.observations is not None:
                    row['observations'] = error.observations
            except Exception as error:
                row['failures'].append(f'{type(error).__name__}: {error}')
                if isinstance(error, PinError):
                    summary['stop_reason'] = str(error)
            finally:
                if group:
                    group.close()
                    row['evidence'] = group.evidence()
                    if group.cleanup_errors:
                        row['passed'] = False
                        row['failures'].extend(group.cleanup_errors)
                        summary['stop_reason'] = 'owned jobs did not close cleanly'
                try:
                    row['source_after'] = pin.verify()
                    require(factory.engine_count() == 0, 'driver left an engine running or another family started', PinError)
                except Exception as error:
                    row['passed'] = False
                    row['failures'].append(f'postflight: {error}')
                    summary['stop_reason'] = str(error)
                row['finished'] = now()
                summary['arms'].append(row)
                write_json(root / 'result.json', summary)
    except Exception as error:
        summary['stop_reason'] = f'preflight: {type(error).__name__}: {error}'
    finally:
        seen = {row['arm'] for row in summary['arms']}
        for name in selected:
            if name not in seen:
                summary['arms'].append({'arm': name, 'attempted': False, 'passed': False,
                    'failures': [summary['stop_reason'] or 'collection interrupted'], 'evidence_gaps': [], 'finished': now()})
        summary['complete'] = [row['arm'] for row in summary['arms']] == list(selected)
        summary['finished'] = now()
        summary['execution_pass'] = bool(selected) and len(set(selected)) == len(selected) and summary['complete'] and not summary['stop_reason'] and all(
            row['passed'] is True and row['attempted'] is True and row['failures'] == [] for row in summary['arms'])
        write_json(root / 'result.json', summary)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=int, required=True)
    parser.add_argument('--build-manifest', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--arm', choices=[*ARMS, 'all'], default='all')
    options = parser.parse_args()
    if __import__('os').name != 'nt':
        parser.error('real A7 gates require the approved Windows runner')
    try:
        root = validate_output(options.out)
        selected = tuple(ARMS) if options.arm == 'all' else (options.arm,)
        with clean_environment() as removed:
            result = run_suite(root, options.source, options.build_manifest, selected)
        result['inherited_test_variables_removed'] = removed
        write_json(root / 'result.json', result)
    except Exception as error:
        parser.exit(2, f'A7 refused: {error}\n')
    print(json.dumps({'execution_pass': result['execution_pass'], 'result': str(root / 'result.json'),
                      'evidence_gaps': {row['arm']: row['evidence_gaps'] for row in result['arms']}}))
    return 0 if result['execution_pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
