"""Interpret retained reconnect counters without confusing dead units with an empty drop ledger."""


def reseat_evidence(reconnect, host_log):
    names = ('host_seats_dropped', 'host_ledger_drops_recorded', 'host_reseats_issued',
             'host_reseats_without_a_ledger', 'host_reseats_without_survivors', 'host_reseat_live_on_team_not_named')
    if not isinstance(reconnect, dict) or any(type(reconnect.get(name)) is not int or reconnect[name] < 0 for name in names):
        return False, 'missing or invalid drop/reseat world counters'
    issued = reconnect['host_reseats_issued']
    empty = reconnect['host_reseats_without_a_ledger']
    dead = reconnect['host_reseats_without_survivors']
    printed = '[net-reconnect] reseating team' in host_log
    ok = (empty == 0 and reconnect['host_seats_dropped'] >= 1 and reconnect['host_ledger_drops_recorded'] >= 1
          and (issued >= 1 or dead >= 1) and (issued == 0 or printed))
    world = 'empty drop ledger' if empty else 'ledgered survivors reseated' if issued else 'no ledgered survivors' if dead else 'no reseat decision'
    # A unit created after the drop can legitimately be alive but absent from its
    # ledger. Keep that census visible; zero is not a valid universal assertion.
    detail = f'{world}; printed={printed}; ' + ', '.join(f'{name}={reconnect[name]}' for name in names)
    return ok, detail
