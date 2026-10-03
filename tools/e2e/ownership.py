"""Native ownership at activation, linked to the returned player's fresh applied controller."""
from pathlib import Path

from cross_report import event_paths, source_rows
from soak_two_peer import fresh_control


def reclaim_evidence(root, spec):
    root = Path(root)
    rows = [row for path in event_paths(root) for row in source_rows(path, root)]
    claims = [row for row in rows if row.get('type') == 'ownership_reclaim']
    errors, paired = [], []
    if not claims: errors.append('no native ownership_reclaim receipt')
    if any(row.get('type') == 'malformed_record' for row in rows): errors.append('malformed native ownership/input stream')
    for claim in claims:
        seat, actor, peer = claim.get('stable_seat'), claim.get('actor'), claim.get('peer')
        activation, round_id, incarnation = claim.get('activation_tick'), claim.get('round'), claim.get('incarnation')
        ticket = claim.get('ticket_incarnation')
        valid = (type(seat) is int and seat >= 0 and ('seat' not in spec or spec['seat'] == seat)
                 and type(actor) is int and actor > 0 and type(peer) is int and peer > 0 and claim.get('owner_peer') == peer
                 and type(round_id) is int and type(incarnation) is int and type(activation) is int
                 and type(ticket) is int and ticket >= 0 and claim.get('seat_incarnation') == ticket
                 and claim.get('committed') is True and claim.get('tick') == activation)
        controls = [row for row in rows if valid and row.get('actor') == actor and row.get('seat_incarnation') == ticket
                    and fresh_control(row, round_id, peer, incarnation, activation)]
        if not valid: errors.append(f'invalid native ownership receipt: {claim}')
        elif not controls: errors.append(f'seat={seat}, actor={actor}, ticket incarnation={ticket}: no matching fresh applied input after tick {activation}')
        else: paired.append(dict(ownership=claim, input=min(controls, key=lambda row: row['tick'])))
    return dict(passed=not errors, errors=errors, receipts=paired)
