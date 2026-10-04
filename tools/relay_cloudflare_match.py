"""The relay proved on real boxes: one lockstep match whose route a scenario row forces (Cloudflare through the directory,
our own relay through the directory's coturn backend, the player's fixed pair entered by hand, or Automatic), judged by
the transport's own receipts against one table of mandatory checks per row.

    python tools/relay_cloudflare_match.py --scenario mp-relay-cloudflare --out D:/mx/<lane>/<dir> [--run NAME ...]
            [--box host=edith --box client=edith] [--tree edith=D:/mx/<lane>/engine] [--alias ally=ally-ts]
            [--ticks N] [--no-replay] [--dry-run]
    python tools/relay_cloudflare_match.py --table <run dir> [<run dir> ...] --out <table.json>
    python tools/relay_cloudflare_match.py --remote-peers <spec.json>            (on a box, inside its session task)
    python tools/relay_cloudflare_match.py --evidence-list <root> --out <list>    (on a box, after its sweep)
    python tools/relay_cloudflare_match.py --bridge-edith <ip:udp-port> <tcp-port> (on EDITH, in an ssh session)

This box (the driver's) launches no engine. A lane row runs the session directory in this process on a loopback port
(TLS, a throwaway certificate every engine pins; the Cloudflare key read by path) and opens an `ssh -R` from each game
box's loopback to it; a public row uses the game's own directory and reads the session's relay offer as a joiner does.
Our relay is a coturn started for the run on the Mac with a per-run secret held in memory (a REST secret: every login the
directory mints from it is time-limited); EDITH reaches it through a UDP-over-ssh bridge, opening no router port. Each
box runs its peers through its session task (tools/edith/remote_box.py's payload) and the private-desktop runner.

The evidence per peer and connection: the same-session `[net-ice] selected candidate=` and `[net-route] RouteAllowed`
lines; the ICE candidates each peer itself sent through the directory, keyed by the sender's own identity (a Relay only
peer sends relay candidates only, at the provider's addresses: Cloudflare's registry or our relay's); the directory's
relay_offer_issued receipt for the session, fresh past the run's end; every inherited relay override cleared, so the
offer is the only TURN list a connection can hold (the provider is bound 'by exclusion' until the engine names the
selected endpoint); equal live hashes over every declared frame; the feel driver's measured pins; the transport's RTT.
Every login a run mints or observes stays in memory and is revoked at the run's end; the run's files are swept on each
box (salted digests travel, never values) and here, scrubbed, swept again, and only then fetched; every failure path
runs the same sweep. A check a row requires and the run did not produce is a failed check.
"""
from __future__ import annotations

import argparse
import base64
import datetime as dt
import hashlib
import hmac
import ipaddress
import json
import os
import re
import secrets
import socket
import ssl
import struct
import subprocess
import sys
import threading
import time
import urllib.request
from pathlib import Path
from urllib.error import HTTPError

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE / 'edith'))

MST = dt.timezone(dt.timedelta(hours=-7))
LANE = os.environ.get('CC_RELAY_LANE')  # else the lane's scratch root the --out directory sits in (D:/mx/<lane>/...)
SCENARIO_DIR = HERE / 'e2e'
CLOUDFLARE_TURN_CONFIG = Path('D:/mx/coturn-20260920/turn-config-cloudflare.json')
# The retired fixed account (turnserver-fixed.conf): read into the book only, so a leak of it is still found; never used by a run.
RETIRED_FIXED_CONF = Path('D:/mx/coturn-20260920/turnserver-fixed.conf')
# Our relay for a run: a coturn on the Mac with a per-run REST secret, on its own LAN port and relay range.
LANE_COTURN = dict(ssh='Erol-Mac', binary='/opt/homebrew/opt/coturn/bin/turnserver', address='192.168.50.122',
                   ports=(3490, 3499), relay=(49301, 49340))
# https://www.cloudflare.com/ips-v4 and /ips-v6, read 2026-10-03; turn.cloudflare.com resolved to 141.101.90.1 that day.
CLOUDFLARE_RANGES = [ipaddress.ip_network(text) for text in (
    '173.245.48.0/20', '103.21.244.0/22', '103.22.200.0/22', '103.31.4.0/22', '141.101.64.0/18', '108.162.192.0/18',
    '190.93.240.0/20', '188.114.96.0/20', '197.234.240.0/22', '198.41.128.0/17', '162.158.0.0/15', '104.16.0.0/13',
    '104.24.0.0/14', '172.64.0.0/13', '131.0.72.0/22', '2400:cb00::/32', '2606:4700::/32', '2803:f800::/32',
    '2405:b500::/32', '2405:8100::/32', '2a06:98c0::/29', '2c0f:f248::/32')]
# Cloudflare's own address space beyond that proxy list, by registry record: its Realtime TURN relays allocate from it
# (the relay candidates of 2026-10-03 were 104.30.136.195 and 104.30.146.169; ARIN RDAP NET-104-16-0-0-1, CLOUDFLARENET,
# registrant Cloudflare, Inc.). Every relay address a run sees is also looked up and its registrant kept in the verdict.
CLOUDFLARE_REGISTERED = [ipaddress.ip_network('104.16.0.0/12')]
CLOUDFLARE_HOSTS = {'turn.cloudflare.com', 'stun.cloudflare.com'}
CLOUDFLARE_REGISTRANTS = {'cloudflare, inc.', 'cloudflare inc', 'cloudflare, inc', 'cloudflarenet'}
RELAY_SETTINGS = ('NetworkTurnServers', 'NetworkTurnUser', 'NetworkTurnPass', 'NetworkPlayerTurnServers', 'NetworkPlayerTurnUser',
                  'NetworkPlayerTurnPass')
# The game's built-in directory (c_DefaultSessionDirectoryUrl): the hotspot rows that turn the tailnet off meet there.
PUBLIC_DIRECTORY = 'directory.broserver.com'
PUBLIC_DIRECTORY_LOGS = ('Erol-Mac', '/Users/erol/cortex-directory/logs')
HOME = dict(gateway='192.168.50.1', public='68.3.162.151')
# Ports other drivers own (the project policy, the e2e README, edith_cross): never chosen here.
FOREIGN_PORTS = [(48320, 48539), (48630, 48649), (49180, 49199), (49400, 49479), (49860, 49879), (49985, 49986)]
PORT_WINDOW = (48700, 49170)
MATCH_TICKS = 1201
TICK_S = 1 / 60
FIXED_PAIR_TTL = 420  # the Fixed row's hand-entered pair: longer than its match, dead minutes after
DRY_RUN = False
REDACTOR = None  # the run's SecretBook: no booked value reaches a printed line or a saved file

# --- the mandatory checks, once (R-b) --------------------------------------------------------------------------------
BASE = ('identities', 'builds', 'exits', 'full_history', 'hashes_equal', 'holds', 'feel_bars', 'relay', 'route_receipts',
        'rtt_recorded', 'secrets_observed', 'no_secret_in_files', 'sanitizer_clean')
REQUIRED = {
    'cloudflare': BASE + ('offer_fresh', 'endpoint', 'relay_registrant', 'provider_201', 'logins_revoked'),
    'coturn': BASE + ('offer_fresh', 'endpoint', 'logins_short_lived'),
    'automatic': BASE + ('offer_fresh', 'direct_expected', 'provider_201', 'logins_revoked'),
    # The Fixed row's pair is typed into the menus, so the engine writes it down: found, blanked, dead by its TTL.
    'fixed': tuple(check for check in BASE if check != 'no_secret_in_files') + ('offer_fresh', 'endpoint', 'menu_entered',
                                                                               'logins_short_lived', 'pair_blanked', 'pair_ttl',
                                                                               'menu_choice:client'),
    'a-automatic-fallback': BASE + ('offer_fresh', 'endpoint', 'logins_revoked', 'tunnel:client', 'panel:client'),
    'b-hotspot-host': BASE + ('offer_fresh', 'logins_revoked', 'tunnel:host', 'listing'),
    'c-four-players': tuple(check for check in BASE if check != 'holds') + ('offer_fresh', 'logins_revoked', 'tunnel:hotspot', 'seat_holds'),
    'd-credential-expiry': BASE + ('offer_fresh', 'endpoint', 'relay_registrant', 'provider_201', 'logins_revoked', 'renewal'),
    'e-migration-relayed': BASE + ('offer_fresh', 'endpoint', 'relay_registrant', 'provider_201', 'logins_revoked', 'migration'),
    'f-relay-by-hand': BASE + ('offer_fresh', 'endpoint', 'relay_registrant', 'provider_201', 'logins_revoked', 'menu_choice:client'),
}


def stamp() -> str:
    return dt.datetime.now(MST).strftime('%Y-%m-%d %I:%M:%S %p MST')


def parse_stamp(text: str | None) -> float | None:
    for pattern in ('%Y-%m-%d %I:%M:%S %p MST', '%Y-%m-%d %H:%M:%S MST'):
        try:
            return dt.datetime.strptime(str(text), pattern).replace(tzinfo=MST).timestamp()
        except ValueError:
            continue
    return None


def redacted(text):
    return REDACTOR.redact(text) if REDACTOR is not None else text


def say(message: str) -> None:
    print(f'[relay-match] {redacted(message)}', flush=True)


def write_json(path, value) -> None:
    Path(path).write_text(redacted(json.dumps(value, indent=2)) + '\n', encoding='utf-8')


def lane_for(out: Path) -> str:
    """The lane's scratch root name: CC_RELAY_LANE, or the D:/mx/<lane> directory the output sits in."""
    if LANE:
        return LANE
    parts = Path(out).resolve().parts
    for index, part in enumerate(parts[:-1]):
        if part.lower() == 'mx' and index + 1 < len(parts):
            return parts[index + 1]
    raise SystemExit('the output is not under D:/mx/<lane>: set CC_RELAY_LANE')


def boxes_file() -> Path:
    """The box inventory: CC_RELAY_BOXES_JSON, or the boxes.json of the inventory copy this run reads."""
    if os.environ.get('CC_RELAY_BOXES_JSON'):
        return Path(os.environ['CC_RELAY_BOXES_JSON'])
    from inventory_location import inventory_dir
    found = inventory_dir() / 'boxes.json'
    if not found.is_file():
        raise SystemExit(f'no box inventory at {found}: set CC_RELAY_BOXES_JSON or CC_INVENTORY_DIR')
    return found


def read_json(path):
    path = Path(path)
    try:
        return json.loads(path.read_text(encoding='utf-8-sig')) if path.is_file() else {}
    except ValueError:
        return {}


# --- the evidence (pure; tools/relay_cloudflare_test.py and relay_gate_test.py drive every rule) ---------------------

CANDIDATE = re.compile(rb'candidate:\S+ \d+ (?:udp|UDP) \d+ (\S+) (\d+) typ (host|srflx|prflx|relay)')


def cloudflare_address(text: str) -> bool:
    try:
        address = ipaddress.ip_address(str(text).strip('[]'))
    except ValueError:
        return False
    return any(address.version == network.version and address in network for network in CLOUDFLARE_RANGES + CLOUDFLARE_REGISTERED)


def registrant(address: str) -> dict:
    """The address's registry record (RDAP through rdap.org, which redirects to the owning registry): Cloudflare only when the
    record's range holds the address and its registrant entity is Cloudflare, Inc. (a network name alone never counts)."""
    try:
        request = urllib.request.Request(f'https://rdap.org/ip/{address}', headers={'User-Agent': 'cccp-relay-proof/1',
                                                                                    'Accept': 'application/rdap+json'})
        with urllib.request.urlopen(request, timeout=20) as reply:
            record = json.load(reply)
    except (OSError, ValueError) as error:
        return dict(address=address, cloudflare=False, error=f'{type(error).__name__}: {error}'[:200])
    return judge_registrant(address, record)


def judge_registrant(address: str, record: dict) -> dict:
    def entities(items):
        for entity in items or []:
            yield entity
            yield from entities(entity.get('entities'))
    registrants = [item[3] for entity in entities(record.get('entities')) if 'registrant' in entity.get('roles', [])
                   for item in (entity.get('vcardArray') or [None, []])[1] if item[0] == 'fn']
    try:
        inside = ipaddress.ip_address(record['startAddress']) <= ipaddress.ip_address(address) <= ipaddress.ip_address(record['endAddress'])
    except (KeyError, ValueError, TypeError):
        inside = False
    owner = any(str(name).strip().lower() in CLOUDFLARE_REGISTRANTS for name in registrants)
    return dict(address=address, handle=record.get('handle'), name=record.get('name'), start=record.get('startAddress'),
                end=record.get('endAddress'), registrants=registrants, inside=inside, cloudflare=bool(inside and owner))


def host_of(endpoint: str) -> str:
    """'141.101.90.17:40001' or '[2a06::1]:5' -> the address."""
    endpoint = str(endpoint)
    if endpoint.startswith('['):
        return endpoint[1:endpoint.index(']')]
    return endpoint.rsplit(':', 1)[0] if endpoint.count(':') == 1 else endpoint


def signal_candidates(payload_b64: str) -> list[tuple[str, int, str]]:
    try:
        raw = base64.b64decode(payload_b64)
    except ValueError:
        return []
    return [(address.decode('ascii', 'replace'), int(port), kind.decode()) for address, port, kind in CANDIDATE.findall(raw)]


def url_host(url: str) -> str:
    rest = url.split(':', 1)[1].split('?', 1)[0]
    return rest[1:rest.index(']')] if rest.startswith('[') else rest.rsplit(':', 1)[0] if ':' in rest else rest


def route_receipts(log: str, session: str | None, any_session: bool = False) -> dict:
    """The route lines a peer wrote for this session, per connection: its selected candidate and its RouteAllowed line.
    Each line keeps its number in the log, so a phase (after a migration, after a renewal) can be read from it."""
    current, connections = None, {}
    for number, line in enumerate(log.splitlines()):
        if found := re.search(r'\[net-ice\] (?:host )?session (\S+)', line):
            current = found[1]
        if not any_session and (not session or current != session):
            continue
        if found := re.search(r'\[net-ice\] selected candidate=(\S+) connection=(\d+)', line):
            connections.setdefault(found[2], {}).update(candidate=found[1], selected_at=number)
        if found := re.search(r'\[net-route\] RouteAllowed route=(\w+) allowed=(\d+) connection=(\d+)', line):
            connections.setdefault(found[3], {}).update(route=found[1], allowed=found[2] == '1', allowed_at=number, line=line.strip())
    accepted = [dict(connection=key, **value) for key, value in connections.items() if value.get('allowed') and value.get('candidate')
                and (value['route'] == 'relay') == (value['candidate'] == 'relay')]
    other_allowed = [dict(connection=key, **value) for key, value in connections.items() if value.get('allowed') and dict(connection=key, **value) not in accepted]
    refused = [dict(connection=key, **value) for key, value in connections.items() if value.get('route') and not value.get('allowed')]
    incomplete = [key for key, value in connections.items() if 'candidate' not in value or 'route' not in value]
    return dict(accepted=accepted, other_allowed=other_allowed, refused=refused, incomplete=incomplete, connections=connections)


def sender_peers(signals, identities: dict[str, str], peers: list[str]) -> dict[str, str]:
    """Each signal sender's peer: 'host' is the host; a joiner 'client:<nonce>' is the peer whose own report names
    'str:c-<the nonce's prefix>'; with one joiner and no report, that joiner. An unmatched sender stays unmatched."""
    joiners = [peer for peer in peers if peer != 'host']
    mapped = {}
    for sender, _ in signals:
        if sender in mapped:
            continue
        if sender == 'host':
            mapped[sender] = 'host'
            continue
        nonce = sender.split(':', 1)[1] if sender.startswith('client:') else sender
        owners = [peer for peer, identity in identities.items() if identity and identity.startswith('str:c-') and nonce.startswith(identity[6:])]
        mapped[sender] = owners[0] if len(owners) == 1 else joiners[0] if not identities and len(joiners) == 1 else f'unmatched:{sender[:16]}'
    return mapped


def judge_relay(run: dict) -> dict:
    """mode cloudflare: every Relay only peer relayed at a Cloudflare address on a fresh Cloudflare offer for this session;
    mode coturn / fixed: the same at our relay's addresses; mode automatic: every peer's route recorded on a fresh offer of
    the provider the directory minted from, and a peer the row expects on the relay (the hotspot fallback) relayed through
    it. Every joiner's own report describes its own connection to the host and must be open (found) and agree with that
    peer's route; a closed or missing report fails the row (the engine writes one connection per report, its connection to
    peer 1, so the host's own report describes none). The provider is bound by exclusion until the engine names the
    selected endpoint: the inherited relay settings were cleared, so the offer's servers are the only ones a connection can
    hold, and the relay candidates each relayed peer itself sent sit at that provider's addresses."""
    mode, session = run['mode'], run.get('session_id')
    reasons, peers, routes = [], {}, {}
    provider = run.get('provider', {'cloudflare': 'cloudflare', 'coturn': 'coturn', 'fixed': 'fixed', 'automatic': 'cloudflare'}.get(mode))
    coturn = provider in ('coturn', 'fixed')
    relay_ok = (lambda address: address in (run.get('relay_addresses') or [])) if coturn else cloudflare_address
    names = sorted(run['connection'])
    signals = run.get('signals') or []
    owners = sender_peers(signals, run.get('identities') or {}, names)
    sent: dict[str, list] = {}
    for sender, payload in signals:
        sent.setdefault(owners[sender], []).extend(signal_candidates(payload))
    for stray in sorted(owner for owner in sent if owner.startswith('unmatched:')):
        reasons.append(f'{stray}: candidates from a sender no peer report names')
    reports = run.get('reports') or {}
    receipts_complete, bindings, report_states = True, {}, {}
    for peer in names:
        connection = run['connection'][peer]
        receipts = route_receipts(run['logs'].get(peer, ''), session)
        chosen = receipts['accepted'][-1]['route'] if receipts['accepted'] else None
        routes[peer] = chosen
        own = sent.get(peer, [])
        relays = sorted({address for address, _, kind in own if kind == 'relay'})
        entry = dict(connection=connection, route=chosen, accepted=receipts['accepted'], other_allowed=receipts['other_allowed'],
                     refused=receipts['refused'], incomplete=receipts['incomplete'], relay_addresses=relays,
                     candidates=[f'{address}:{port} {kind}' for address, port, kind in own])
        if not receipts['accepted']:
            reasons.append(f'{peer}: no accepted route receipt for session {session!r}')
        if receipts['incomplete']:
            receipts_complete = False
            reasons.append(f'{peer}: connection(s) {receipts["incomplete"]} have no complete route receipt')
        if peer != 'host':
            report = reports.get(peer)
            state = 'missing' if not isinstance(report, dict) or not report else 'closed' if report.get('found') is not True else 'open'
            report_states[peer] = state
            entry['report'] = {key: (report or {}).get(key) for key in ('found', 'state', 'relayed', 'remote_identity', 'remote_address')}
            if state != 'open':
                reasons.append(f'{peer}: its report holds no live connection ({state}): a lost report fails the row')
            elif chosen is not None and bool(report.get('relayed')) != (chosen == 'relay'):
                reasons.append(f'{peer}: its report says relayed={report.get("relayed")} where its route is {chosen}')
        if connection == 'RelayOnly':
            if chosen != 'relay' or receipts['other_allowed'] or any(row['route'] != 'relay' for row in receipts['accepted']):
                reasons.append(f'{peer}: Relay only but routes {[row.get("route") for row in receipts["accepted"] + receipts["other_allowed"]]}')
            if not relays:
                reasons.append(f'{peer}: sent no relay candidate through the directory')
            if any(kind != 'relay' for _, _, kind in own):
                reasons.append(f'{peer}: Relay only but sent {sorted({kind for _, _, kind in own if kind != "relay"})} candidates')
        outside = sorted(address for address in relays if not relay_ok(address))
        if outside and (connection == 'RelayOnly' or chosen == 'relay'):
            reasons.append(f'{peer}: relay candidates outside the {provider} relay: {outside}')
        if chosen == 'relay':
            report = reports.get(peer)
            if relays and not outside and run.get('overrides_cleared'):
                bindings[peer] = 'by exclusion'
            elif report and report.get('found') is True and report.get('relayed') and report.get('remote_address') and \
                    relay_ok(host_of(report['remote_address'])):
                bindings[peer] = 'by its open report'
            else:
                bindings[peer] = None
                why = 'the inherited relay settings were not cleared' if relays and not run.get('overrides_cleared') else 'no candidate or report names it'
                reasons.append(f'{peer}: its relay is not bound to the {provider} relay: {why}')
        entry['binding'] = bindings.get(peer)
        peers[peer] = entry
    for peer, wanted in (run.get('expect_routes') or {}).items():
        if routes.get(peer) != wanted:
            reasons.append(f'{peer}: took the {routes.get(peer)} route where the row needs {wanted}'
                           + ('; the fallback to the relay was not exercised' if wanted == 'relay' else ''))
    offer_fresh = None
    if provider:
        valid = [row for row in run.get('offers') or [] if session and row.get('session_id') == session and row.get('provider') == provider
                 and all(type(row.get(key)) is int and row[key] > 0 for key in ('generation', 'expires_at', 'server_count'))]
        fresh = [row for row in valid if str(row.get('match_id', '')).startswith(f'{session}:')
                 and (run.get('run_ends_at') is None or row['expires_at'] > run['run_ends_at'])]
        offer_fresh = bool(fresh) and run.get('run_ends_at') is not None
        if not valid:
            reasons.append(f'no {provider} relay offer issued for session {session!r}')
        elif not offer_fresh:
            reasons.append(f'the {provider} offer is stale or names another match: '
                           f'{[(row.get("match_id"), row.get("expires_at")) for row in valid]} against the run end {run.get("run_ends_at")}')
        hosts = sorted({url_host(url) for url in run.get('offer_urls') or []})
        allowed = set(run.get('relay_hosts') or run.get('relay_addresses') or []) if coturn else CLOUDFLARE_HOSTS
        if run.get('offer_urls') is not None and (not hosts or set(hosts) - allowed):
            reasons.append(f'the offer names relay servers {hosts}, expected only {sorted(allowed)}')
    endpoint = all(bindings.get(peer) for peer in names if routes.get(peer) == 'relay') and any(routes.get(peer) == 'relay' for peer in names)
    return dict(mode=mode, provider=provider, passed=not reasons, reasons=reasons, peers=peers, routes=routes, reports=report_states,
                receipts_complete=receipts_complete and all(entry['accepted'] for entry in peers.values()),
                offer_fresh=offer_fresh, endpoint=endpoint, bindings=bindings,
                binding_note='by exclusion: the engine names neither the selected endpoint nor the offer generation yet',
                direct_as_expected=all(route == 'direct' for route in routes.values()))


FEEL_BARS = ('item9a_wall_tps', 'item9a_net_wait', 'item9a_steady_stalls', 'item9a_missing_frame_stalls', 'item9a_longest_wait',
             'item9a_confirmed_horizon_lag')


def feel_bars(timing: dict, peer: str) -> dict:
    """The feel driver's own measured pins for one peer (its thresholds, unchanged): every one present and PASS. The pins this
    match cannot measure are listed with their reasons: the harness-cost receipt no current engine prints, and the input
    pins of the feel recorder a lean match does not run."""
    pins = (timing.get('peers', {}).get(peer) or {}).get('pins', {})
    failed = {name: (pins.get(name) or {}).get('status', 'MISS') for name in FEEL_BARS if (pins.get(name) or {}).get('status') != 'PASS'}
    unmeasured = {name: str(pin.get('reason', ''))[:160] for name, pin in pins.items() if name not in FEEL_BARS and pin.get('status') == 'MISS'}
    return dict(passed=not failed, failed=failed, values={name: (pins.get(name) or {}).get('value') for name in FEEL_BARS},
                unmeasured=unmeasured, pass_check=(timing.get('peers', {}).get(peer) or {}).get('pass_check'))


def tunnel_receipt(rows: list[dict], peers: list[str] | None = None, min_seconds: float | None = None) -> dict:
    """The hotspot box's own Tailscale states around its engines, by its own clock: down (Stopped) stamped before the first
    engine started; for every peer one start and one end record, in that order, Stopped at both and at every record between;
    back up (Running) stamped after the last engine ended; with min_seconds, each engine bracket at least that long. A
    missing record, a Running state inside a bracket or an unreadable time fails."""
    reasons = []
    times = [parse_stamp(row.get('at')) for row in rows]
    down = next((index for index, row in enumerate(rows) if row.get('step') == 'down'), None)
    up = next((index for index, row in enumerate(rows) if row.get('step') == 'up'), None)
    if down is None or rows[down].get('exit_code') != 0 or rows[down].get('backend_state') != 'Stopped':
        reasons.append(f'tailscale down did not stop the tunnel: {rows[down] if down is not None else "no down step"}')
    named = peers or sorted({row.get('peer') for row in rows if row.get('step', '').startswith('engine-') and row.get('peer')})
    if not named:
        reasons.append('the receipt names no engine')
    starts_at, ends_at = [], []
    for peer in named:
        starts = [index for index, row in enumerate(rows) if row.get('step') == 'engine-started' and row.get('peer') == peer]
        ends = [index for index, row in enumerate(rows) if row.get('step') == 'engine-ended' and row.get('peer') == peer]
        if len(starts) != 1 or len(ends) != 1 or ends[0] < starts[0]:
            reasons.append(f'{peer}: {len(starts)} start and {len(ends)} end record(s), not one ordered bracket')
            continue
        running = [row.get('step') for row in rows[starts[0]:ends[0] + 1] if row.get('backend_state') != 'Stopped']
        if running:
            reasons.append(f'{peer}: the tunnel was not Stopped inside the bracket at {running}')
        began, ended = times[starts[0]], times[ends[0]]
        if began is None or ended is None or ended < began:
            reasons.append(f'{peer}: the bracket times are unreadable or out of order')
            continue
        starts_at.append(began)
        ends_at.append(ended)
        if min_seconds is not None and ended - began < min_seconds:
            reasons.append(f'{peer}: the engine ran {ended - began:.0f} s inside the bracket, the match needs {min_seconds:.0f} s')
    if down is not None and starts_at:
        if times[down] is None or times[down] > min(starts_at) or any(index < down for index, row in enumerate(rows) if row.get('step') == 'engine-started'):
            reasons.append('an engine started before the tunnel went down')
    if up is None or rows[up].get('exit_code') != 0 or rows[up].get('backend_state') != 'Running':
        reasons.append(f'the tunnel did not come back up: {rows[up] if up is not None else "no up step"}')
    elif ends_at and (times[up] is None or times[up] < max(ends_at)
                      or any(index > up for index, row in enumerate(rows) if row.get('step') == 'engine-ended')):
        reasons.append('the tunnel came back up before the last engine ended')
    return dict(passed=not reasons, reasons=reasons)


def hotspot_preflight(state: str | None, gateway: str | None, mapped: str | None) -> dict:
    """The hotspot box's network, read before a live row. Only well-formed facts count: the tunnel state exactly Running or
    Stopped, a gateway that parses as an address, a STUN-mapped address that parses (address or address:port); anything else
    is REFUSED. A stopped tunnel refuses; the home network refuses."""
    state, gateway, mapped = (str(value).strip() if value is not None else '' for value in (state, gateway, mapped))

    def address(text: str) -> str | None:
        host = text[1:text.index(']')] if text.startswith('[') and ']' in text else text.rsplit(':', 1)[0] if text.count(':') == 1 else text
        try:
            return str(ipaddress.ip_address(host))
        except ValueError:
            return None
    malformed = [name for name, ok in (('tailscale state', state in ('Running', 'Stopped')), ('default gateway', address(gateway) is not None),
                                       ('STUN mapped address', address(mapped) is not None)) if not ok]
    if malformed:
        return dict(verdict='REFUSED', reason=f'unknown or malformed precondition: {", ".join(malformed)}')
    if state != 'Running':
        return dict(verdict='REFUSED', reason=f'the tunnel is {state}, not Running')
    if address(gateway) == HOME['gateway'] or address(mapped) == HOME['public']:
        return dict(verdict='HOME', reason=f'the box is on the home network (gateway {address(gateway)}, mapped {address(mapped)})')
    return dict(verdict='AWAY', reason=f'gateway {address(gateway)}, mapped {address(mapped)}')


def panel_verdict(result) -> dict:
    """The in-match connection panel's probe (CC_TEST_NET_UI_SCRIPT): it ran to its end and every assertion held."""
    ok = isinstance(result, dict) and result.get('pass') is True and result.get('complete') is True
    return dict(passed=ok, reasons=[] if ok else [f'the connection panel probe did not pass: {result!r}'[:300]])


def listing_evidence(lines: list[str], session: str | None, listed: bool) -> dict:
    """A hosting box listed in the directory (its row seen in the listing) and given a Cloudflare relay for that session."""
    offers = []
    for line in lines:
        if 'relay_offer_issued ' in line:
            try:
                row = json.loads(line.split('relay_offer_issued ', 1)[1])
            except ValueError:
                continue
            if session and row.get('session_id') == session and row.get('provider') == 'cloudflare':
                offers.append(row)
    reasons = (([] if listed else [f'session {session!r} was never seen in the directory listing'])
               + ([] if offers else [f'no Cloudflare relay offer was issued for session {session!r}']))
    return dict(passed=not reasons, reasons=reasons, offers=offers)


def seat_holds(peers: list[dict], allowed: set[str], expected: set[str] | None = None) -> dict:
    """The match summary's holds per seat: only the seats the row allows (the hotspot's) may have been held."""
    reasons = []
    names = {peer.get('name') for peer in peers}
    if expected and names != set(expected):
        reasons.append(f'the summary names seats {sorted(map(str, names))}, the row has {sorted(expected)}')
    held = {peer.get('name'): peer.get('holds') for peer in peers if peer.get('holds') and peer.get('name') not in allowed}
    if held:
        reasons.append(f'seats other than {sorted(allowed)} were held: {held}')
    return dict(passed=not reasons, reasons=reasons)


def renewal_evidence(logs: dict[str, str], calls: list[dict], relayed: list[str], ttl_s: float | None = None,
                     line_times: dict[str, list] | None = None, samples: dict[str, list] | None = None, first_expiry: float | None = None,
                     clocks: dict[str, tuple] | None = None, session: str | None = None) -> dict:
    """An expiring relay login renewed on the live connection of every relayed peer, judged by time: the second mint at or
    after the half-life measured from the first mint; every mint answered 201; for each relayed peer the engine's renewal
    line naming at least one live connection, seen on its box AFTER the second mint; that connection's route re-read after
    the renewal (still relay and allowed, no later refusal or change); and the peer's frames continuing past the first
    login's ACTUAL expiry. Box times are moved to this box's clock by the measured offset and judged against its uncertainty."""
    reasons, renewed, routes_after = [], {}, {}
    epochs = [call.get('epoch') if call.get('epoch') is not None else parse_stamp(call.get('at')) for call in calls]
    if len(calls) < 2:
        reasons.append(f'the directory minted {len(calls)} login(s); a renewal needs a second')
    if any(call.get('status') != 201 for call in calls):
        reasons.append(f'a mint was refused: {[call.get("status") for call in calls]}')
    second = epochs[1] if len(epochs) > 1 else None
    if ttl_s is None:
        reasons.append('the row gives no login lifetime to judge the renewal by')
    elif len(calls) >= 2 and (epochs[0] is None or second is None or second < epochs[0] + ttl_s / 2):
        reasons.append(f'the second mint came {None if epochs[0] is None or second is None else round(second - epochs[0], 1)} s after the first, '
                       f'before the half-life of {ttl_s / 2:.0f} s')
    for peer in relayed:
        offset, uncertainty = (clocks or {}).get(peer) or (None, None)
        timed = [(when - offset, line) for when, line in (line_times or {}).get(peer, []) if offset is not None
                 and re.search(r'\[net-relay\] relay login renewed on ([1-9]\d*) live connection', line)]
        after = [when for when, _ in timed if second is not None and when - uncertainty > second]
        renewed[peer] = len(after)
        if not after:
            reasons.append(f'{peer}: no renewal on a live connection seen after the second mint' if timed or offset is not None
                           else f'{peer}: the renewal line carries no time from its box')
        log = logs.get(peer, '').splitlines()
        marks = [index for index, line in enumerate(log) if re.search(r'\[net-relay\] relay login renewed on [1-9]\d* live connection', line)]
        if marks:
            later = route_receipts('\n'.join(log[marks[-1]:]), None, any_session=True)['connections']
            current = {key: row.get('route') for key, row in route_receipts('\n'.join(log), session)['connections'].items() if row.get('allowed')}
            changed = {key: row for key, row in later.items() if 'route' in row and (row.get('route') != 'relay' or not row.get('allowed'))}
            routes_after[peer] = current
            if changed or not any(route == 'relay' for route in current.values()):
                reasons.append(f'{peer}: its route after the renewal is not relay and allowed: {current} {sorted(changed)}')
        peer_samples = sorted((when - offset, tick) for when, tick in (samples or {}).get(peer, []) if offset is not None)
        before = [tick for when, tick in peer_samples if first_expiry is not None and when + uncertainty < first_expiry]
        past = [tick for when, tick in peer_samples if first_expiry is not None and when - uncertainty > first_expiry]
        if first_expiry is None or not past or max(past) <= max(before, default=0):
            reasons.append(f'{peer}: no frames timed past the first login\'s expiry')
    return dict(passed=not reasons, reasons=reasons, renewed=renewed, mints=len(calls), routes_after_renewal=routes_after,
                note='the offer generation is bound by exclusion until the engine names it')


def migration_declarations(logs: dict[str, str], survivors: list[str], session: str | None = None, seats: dict[str, str] | None = None,
                           last_ticks: dict[str, int] | None = None) -> dict:
    """Every survivor declares, once, the same successor, boundary and round after the host is lost; the successor is
    exactly one seat's name; each survivor holds a NEW connection (an id it never used before the declaration) whose
    selected candidate is relay and whose own RouteAllowed route=relay allowed=1 line follows the declaration; and each
    survivor's frames run past the boundary. A repeated line on an old connection proves nothing."""
    found = {peer: re.findall(r'(?m)^\[net-match\] Host left - (.+) is now hosting; boundary=(\d+) round=(\d+)', logs.get(peer, ''))
             for peer in survivors}
    distinct = {entry for rows in found.values() for entry in rows}
    agreed = all(len(rows) == 1 for rows in found.values()) and len(distinct) == 1
    reasons = [] if agreed else [f'the survivors did not declare one successor once each: {found}']
    entry = next(iter(distinct)) if len(distinct) == 1 else (None, None, None)
    boundary = int(entry[1]) if entry[1] else None
    if agreed and (seats is None or list(seats.values()).count(entry[0]) != 1):
        reasons.append(f'the successor {entry[0]!r} is not exactly one seat of {sorted((seats or {}).values())}')
    successors = {}
    if agreed:
        for peer in survivors:
            lines = logs.get(peer, '').splitlines()
            declared = next(index for index, line in enumerate(lines) if 'is now hosting; boundary=' in line)
            seen_before = {match for line in lines[:declared] for match in re.findall(r'connection=(\d+)', line)}
            post = [key for key, row in route_receipts('\n'.join(lines[declared:]), None, any_session=True)['connections'].items()
                    if key not in seen_before and row.get('candidate') == 'relay' and row.get('route') == 'relay' and row.get('allowed')
                    and row.get('selected_at', -1) >= 0 and row.get('allowed_at', -1) >= 0]
            successors[peer] = post
            if not post:
                reasons.append(f'{peer}: no new connection with its own relay receipts after the migration')
    if agreed and (last_ticks is None or any((last_ticks.get(peer) or 0) <= (boundary or 0) for peer in survivors)):
        reasons.append(f'no frames after the loss on every survivor (boundary {boundary}): {last_ticks}')
    return dict(passed=not reasons, reasons=reasons, successor=entry[0], boundary=boundary, round=int(entry[2]) if entry[2] else None,
                successor_connections=successors)


def menu_choice(log: str, value: str, session: str | None = None) -> dict:
    """The player's own choice through Settings > Network > Connection, read back by the menu script BEFORE the match's
    networking starts (its lobby, session, ICE or route lines): a choice confirmed after the connection started proves
    nothing about that connection. With the session given, the relay must also be the candidate a connection of that
    session selected after the choice."""
    lines = log.splitlines()
    confirm = re.compile(rf'^\[menu-script\] assert_label ComboNetworkConnection "{re.escape(value)}" text="[^"]*{re.escape(value)}[^"]*" PASS')
    chosen = next((index for index, line in enumerate(lines) if confirm.match(line)), None)
    network = next((index for index, line in enumerate(lines) if re.match(r'\[(net-ice|net-route|net-lobby|net-session|net-match)\]', line)), None)
    if chosen is None:
        return dict(passed=False, reasons=[f'the menu never confirmed Connection = {value}'])
    if network is not None and network < chosen:
        return dict(passed=False, reasons=[f'the menu confirmed Connection = {value} at line {chosen}, after the network started at line {network}'])
    if session is not None:
        relayed = [row for row in route_receipts(log, session)['connections'].values()
                   if row.get('candidate') == 'relay' and row.get('selected_at', -1) > chosen]
        if not relayed:
            return dict(passed=False, reasons=[f'no connection of session {session!r} selected a relay candidate after the choice'])
    return dict(passed=True, reasons=[])


def transport_rtts(log: str) -> list[dict]:
    return [dict(peer=int(peer), rtt_ms=int(rtt), delay_frames=int(frames)) for peer, rtt, frames in
            re.findall(r'\[net-match\] auto input delay: peer (\d+) rtt (\d+)ms -> (\d+) frames', log)]


def delay_changes(log: str) -> list[dict]:
    return [dict(peer=int(peer), frame=int(frame), delay=int(delay)) for peer, frame, delay in
            re.findall(r'\[net-match\] delay change peer=(\d+) frame=(\d+) delay=(\d+)', log)]


def coturn_rest_login(secret: str, ttl_s: int, now: int | None = None) -> tuple[str, str]:
    """coturn's REST pair (use-auth-secret): username '<expiry>:<tag>', password base64(HMAC-SHA1(secret, username))."""
    username = f'{(now or int(time.time())) + int(ttl_s)}:{secrets.token_hex(12)}'
    return username, base64.b64encode(hmac.new(secret.encode(), username.encode(), hashlib.sha1).digest()).decode()


# --- a TURN Allocate, enough to ask our relay whether a login is alive ------------------------------------------------

def turn_allocate(server: tuple[str, int], username: str, password: str, timeout: float = 4.0) -> dict:
    """RFC 5766 Allocate with the long-term credential: the first answer's realm and nonce, then the signed request.
    Returns the final message class and error code; the login is used for the HMAC only and never returned."""
    def attribute(kind: int, value: bytes) -> bytes:
        return struct.pack('!HH', kind, len(value)) + value + b'\x00' * ((4 - len(value) % 4) % 4)

    def message(attributes: bytes, txn: bytes, integrity_key: bytes | None) -> bytes:
        body = attributes
        if integrity_key is not None:
            header = struct.pack('!HHI', 0x0003, len(body) + 24, 0x2112A442) + txn
            digest = hmac.new(integrity_key, header + body, hashlib.sha1).digest()
            body += attribute(0x0008, digest)
        return struct.pack('!HHI', 0x0003, len(body), 0x2112A442) + txn + body

    def parse(data: bytes) -> tuple[int, dict]:
        kind, length = struct.unpack('!HH', data[:4])
        fields, offset = {}, 20
        while offset + 4 <= 20 + length:
            name, size = struct.unpack('!HH', data[offset:offset + 4])
            fields[name] = data[offset + 4:offset + 4 + size]
            offset += 4 + size + ((4 - size % 4) % 4)
        return kind, fields

    transport = attribute(0x0019, b'\x11\x00\x00\x00')
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(timeout)
        sock.sendto(message(transport, os.urandom(12), None), server)
        kind, fields = parse(sock.recvfrom(4096)[0])
        realm, nonce = fields.get(0x0014, b''), fields.get(0x0015, b'')
        if kind != 0x0113 or not realm or not nonce:
            return dict(first=hex(kind), result='no challenge')
        key = hashlib.md5(username.encode() + b':' + realm + b':' + password.encode()).digest()
        signed = transport + attribute(0x0006, username.encode()) + attribute(0x0014, realm) + attribute(0x0015, nonce)
        sock.sendto(message(signed, os.urandom(12), key), server)
        kind, fields = parse(sock.recvfrom(4096)[0])
    error = fields.get(0x0009)
    code = (error[2] & 0x7) * 100 + error[3] if error and len(error) >= 4 else None
    return dict(result='allocated' if kind == 0x0103 else 'refused', message_class=hex(kind), error_code=code)


# --- the boxes -------------------------------------------------------------------------------------------------------

class Box:
    def __init__(self, name: str, entry: dict, tree: str | None = None, alias: str | None = None) -> None:
        self.name = name
        self.alias = alias or entry['ssh']
        self.task = entry.get('task', 'cortex-session1')
        self.session_script = entry.get('session_script', 'D:/mx/session1/run.ps1')
        self.tree = tree or entry['repo']
        self.path_prepend = list(entry.get('path_prepend') or [])
        self.max_engines = int((entry.get('memory') or {}).get('max_engines') or (entry.get('runner') or {}).get('max_engines') or 2)
        self.computer = entry.get('computer_name') or entry.get('hostname') or name
        from remote_box import RemoteBox
        self.remote = RemoteBox(self.alias, self.task, self.session_script, dry_run=DRY_RUN, say=say)


def load_boxes(trees: dict[str, str], aliases: dict[str, str] | None = None) -> dict[str, Box]:
    entries = {entry['name'].lower(): entry for entry in json.loads(boxes_file().read_text(encoding='utf-8'))['boxes']
               if entry.get('kind') == 'windows-task'}
    return {name: Box(name, entry, trees.get(name), (aliases or {}).get(name)) for name, entry in entries.items()}


def excluded_ranges(box: Box) -> list[tuple[int, int]]:
    text = box.remote.ssh('netsh interface ipv4 show excludedportrange protocol=tcp; netsh interface ipv4 show excludedportrange protocol=udp')
    return [(int(low), int(high)) for low, high in re.findall(r'(?m)^\s*(\d+)\s+(\d+)', text)]


def ports_free(box: Box, ports: list[int]) -> bool:
    probe = ('import socket,sys\nok=True\nfor p in %r:\n for kind in (socket.SOCK_STREAM, socket.SOCK_DGRAM):\n'
             '  s=socket.socket(socket.AF_INET, kind)\n  try: s.bind(("0.0.0.0", p))\n  except OSError: ok=False\n'
             '  finally: s.close()\nprint("free" if ok else "busy")\n') % (ports,)
    encoded = base64.b64encode(probe.encode()).decode()
    return box.remote.ssh(f"python -c \"import base64;exec(base64.b64decode('{encoded}'))\"").strip().endswith('free')


def choose_ports(boxes: list[Box], count: int) -> list[int]:
    """A block free on every game box (their excluded ranges and live binds) and on this box's loopback."""
    if DRY_RUN:
        return list(range(PORT_WINDOW[0], PORT_WINDOW[0] + count))
    ranges = FOREIGN_PORTS + [r for box in boxes for r in excluded_ranges(box)]
    local = subprocess.run(['netsh', 'interface', 'ipv4', 'show', 'excludedportrange', 'protocol=tcp'], capture_output=True, text=True).stdout
    ranges += [(int(low), int(high)) for low, high in re.findall(r'(?m)^\s*(\d+)\s+(\d+)', local)]
    for start in range(PORT_WINDOW[0], PORT_WINDOW[1] - count, count):
        block = list(range(start, start + count))
        if any(not (block[-1] < low or block[0] > high) for low, high in ranges):
            continue
        try:
            for port in block:
                with socket.socket() as sock:
                    sock.bind(('127.0.0.1', port))
        except OSError:
            continue
        if all(ports_free(box, block) for box in boxes):
            return block
    raise RuntimeError(f'no free block of {count} ports in {PORT_WINDOW} on {[box.name for box in boxes]}')


def identity(box: Box, expected_head: str | None = None) -> dict:
    """The box tree's executable bound to its build receipt (tools/acceptance_identity.py's fields)."""
    errors, build = [], {}
    exe = f'{box.tree}/Cortex Command.exe'
    measured = None if DRY_RUN else box.remote.sha256(exe)
    head = '' if DRY_RUN else box.remote.ssh(f"git -C '{box.tree}' rev-parse HEAD").strip()
    text = None if DRY_RUN else box.remote.read_text(f'{box.tree}/tools/cross_peers/build.json')
    try:
        build = json.loads(text) if text else {}
    except ValueError:
        errors.append('the build receipt is not JSON')
    if not DRY_RUN:
        if not build:
            errors.append('no build receipt (tools/cross_peers/build.json)')
        elif build.get('commit') != head or build.get('executable_sha256') != measured:
            errors.append(f'the build receipt names {build.get("commit")} / {build.get("executable_sha256")}, '
                          f'the tree is at {head} and its executable hashes {measured}')
        if expected_head and head != expected_head:
            errors.append(f'the tree is at {head}, the run expects {expected_head}')
    return dict(schema=1, box=box.name.upper(), node=box.computer, platform='Windows', os='Windows', source_sha=head, head=head,
                executable=exe, executable_sha256=measured, build=build, build_receipt=f'{box.tree}/tools/cross_peers/build.json',
                status='INCOMPLETE' if errors else 'PASS', errors=errors, measured=stamp())


def box_lan_address(box: Box) -> str:
    if DRY_RUN:
        return '0.0.0.0'
    text = box.remote.ssh("(Get-NetIPConfiguration | Where-Object { $_.IPv4DefaultGateway } | Select-Object -First 1).IPv4Address.IPAddress")
    address = text.strip().splitlines()[-1].strip() if text.strip() else ''
    ipaddress.ip_address(address)
    return address


# --- the directory on this box ---------------------------------------------------------------------------------------

class Directory:
    """The session directory in this process, observed: the candidates each side sends, the logins it mints or publishes,
    the provider's statuses. Nothing it observes is written except candidates and statuses; at stop every Cloudflare login
    it minted is revoked."""

    def __init__(self, root: Path, port: int, backend: dict | None, ttl_cap: int, book) -> None:
        from session_directory import session_directory as module
        from edith_cross import make_cert
        self.module, self.book, self.backend = module, book, backend
        self.signals, self.provider_calls, self.offers, self.minted, self.revokes, self.published = [], [], [], [], [], []
        self.cert, key, self.pin = make_cert(root)
        self.handlers = set(module.LOGGER.handlers)
        real = self.real_urlopen = module.urlopen

        def urlopen(request, *args, **kwargs):
            try:
                response = real(request, *args, **kwargs)
            except HTTPError as error:
                code = re.search(r'error code:\s*(\d+)', error.read(128).decode('ascii', 'replace'))
                self.provider_calls.append(dict(at=stamp(), epoch=time.time(), status=error.code, provider_error_code=code[1] if code else None))
                raise
            self.provider_calls.append(dict(at=stamp(), epoch=time.time(), status=response.status))
            return response
        if backend and backend.get('backend', 'cloudflare') == 'cloudflare':
            module.urlopen = urlopen
        self.server = module.spawn_server(port=port, cert=self.cert, key=key, insecure_http=False, log_file=root / 'service.log',
                                          turn_config=backend, turn_max_ttl=ttl_cap)
        key.unlink()
        store = self.server.store
        mint, post, register, mint_offer = store.turn_provider.mint, store.post_signal, store.register, store.mint_ice_servers

        def minted(*args, **kwargs):
            offer = mint(*args, **kwargs)
            book.add_offer(offer)
            self.minted += [dict(username=server['username'], expires_at=offer['expires_at'], minted_at=int(time.time()))
                            for server in offer.get('iceServers', []) if server.get('username')]
            return offer

        def posted(session_id, data, now):
            self.signals.append((str(data.get('from')), str(data.get('payload_b64', ''))))
            return post(session_id, data, now)

        def registered(*args, **kwargs):
            row = register(*args, **kwargs)
            book.add('directory-session-token', row.get('token'))
            return row

        def published(session_id, data, install_key, now):
            if isinstance(data, dict):
                book.add('directory-session-token', data.get('token'))
                if 'iceServers' in data:
                    book.add_offer({'iceServers': data['iceServers']}, 'fixed')
                    self.published.append(session_id)
            offer = mint_offer(session_id, data, install_key, now)
            self.offers.append(dict(session_id=session_id, urls=[url for server in offer.get('iceServers', []) for url in server.get('urls', [])],
                                    expires_at=offer.get('expires_at')))
            return offer
        store.turn_provider.mint, store.post_signal, store.register, store.mint_ice_servers = minted, posted, registered, published

    def sessions(self) -> list[str]:
        with self.server.store._lock:
            return list(self.server.store._sessions)

    def stop(self) -> None:
        """Stops the server, revokes every Cloudflare login it minted (a test login never outlives its run, wherever an
        engine may have written it) and detaches this run's log file and provider hook, so the next run logs to its own root."""
        try:
            self.server.stop()
        finally:
            if self.backend and self.backend.get('backend', 'cloudflare') == 'cloudflare':
                self.revokes = revoke_cloudflare(self.backend, [row['username'] for row in self.minted], self.real_urlopen,
                                                 self.module.USER_AGENT)
            self.module.urlopen = self.real_urlopen
            for handler in set(self.module.LOGGER.handlers) - self.handlers:
                self.module.LOGGER.removeHandler(handler)
                handler.close()


def revoke_cloudflare(backend: dict, usernames: list[str], opener=None, agent: str = 'cccp-session-directory/1') -> list[int]:
    opener = opener or urllib.request.urlopen
    statuses = []
    for username in dict.fromkeys(usernames):
        request = urllib.request.Request(
            f'https://rtc.live.cloudflare.com/v1/turn/keys/{backend["turn_key_id"]}/credentials/{username}/revoke', data=b'',
            method='POST', headers={'Authorization': 'Bearer ' + backend['api_token'], 'User-Agent': agent})
        try:
            with opener(request, timeout=15) as response:
                statuses.append(response.status)
        except HTTPError as error:
            statuses.append(error.code)
        except OSError:
            statuses.append(0)
    return statuses


class Tunnel:
    """ssh -R: the game box's 127.0.0.1:<port> reaches the given port on this box's loopback (the rendezvous, the bridge)."""

    def __init__(self, box: Box, ports: list[int], log_path: Path) -> None:
        self.box, self.ports, self.log_path, self.process = box, ports, log_path, None

    def open(self) -> None:
        argv = ['ssh', '-N', '-o', 'ExitOnForwardFailure=yes', '-o', 'ServerAliveInterval=15',
                *[part for port in self.ports for part in ('-R', f'127.0.0.1:{port}:127.0.0.1:{port}')], self.box.alias]
        if DRY_RUN:
            say('dry-run: ' + ' '.join(argv))
            return
        self.process = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=self.log_path.open('w'), stderr=subprocess.STDOUT,
                                        creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        for _ in range(10):
            time.sleep(2)
            probe = self.box.remote.ssh(f"$c = New-Object Net.Sockets.TcpClient; try {{ $c.Connect('127.0.0.1', {self.ports[0]}); 'open' }} "
                                        f"catch {{ 'closed' }} finally {{ $c.Close() }}").strip()
            if probe == 'open':
                return
            if self.process.poll() is not None:
                break
        raise RuntimeError(f'the ssh -R tunnel did not open on {self.box.name} (port {self.ports[0]}); see {self.log_path}')

    def close(self) -> None:
        if self.process and self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=10)


# --- our relay for a run, and the bridge EDITH reaches it through ---------------------------------------------------

# Runs on the Mac: reads the secret from stdin and serves it as coturn's configuration through a named pipe (no storage),
# once for each time coturn opens it, then removes the pipe; coturn runs on in its own session.
COTURN_STARTER = r'''
import json, os, subprocess, sys, tempfile, time
secret = sys.stdin.readline().strip()
binary, args = sys.argv[1], json.loads(sys.argv[2])
folder = tempfile.mkdtemp()
fifo = os.path.join(folder, 'turn.conf')
os.mkfifo(fifo, 0o600)
process = subprocess.Popen([binary, '-c', fifo] + args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, start_new_session=True)
print(process.pid, flush=True)
deadline, served = time.time() + 6, 0
try:
    while time.time() < deadline and process.poll() is None:
        try:
            handle = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
        except OSError:
            time.sleep(0.05)
            continue
        try:
            os.set_blocking(handle, True)
            os.write(handle, ('static-auth-secret=' + secret + '\n').encode())
            served += 1
        except OSError:
            pass
        finally:
            os.close(handle)
        time.sleep(0.05)
finally:
    os.unlink(fifo)
    os.rmdir(folder)
print('served', served, flush=True)
'''


class LaneCoturn:
    """A coturn started for one run on the Mac: its own LAN port and relay range, REST authentication with a per-run secret
    that exists only in this process, in the directory's memory and in that coturn's memory (handed over on stdin and a
    named pipe, never as an argument a shell, a process list or an exception could show); stopped by its PID."""

    def __init__(self, book) -> None:
        self.secret = secrets.token_hex(24)
        book.add('lane-coturn-secret', self.secret)
        self.pid, self.port = None, None
        self.address = LANE_COTURN['address']

    def start(self) -> None:
        if DRY_RUN:
            self.port = LANE_COTURN['ports'][0]
            say(f'dry-run: coturn on {LANE_COTURN["ssh"]} {self.address}:{self.port} relay {LANE_COTURN["relay"]} (REST secret in memory)')
            return
        free = subprocess.run(['ssh', '-o', 'BatchMode=yes', LANE_COTURN['ssh'],
                               '/usr/bin/python3 -c "import socket\nfor p in range(%d,%d):\n s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)\n'
                               ' try:\n  s.bind((\'0.0.0.0\',p)); print(p); break\n except OSError: pass\n finally: s.close()"' % (
                                   LANE_COTURN['ports'][0], LANE_COTURN['ports'][1] + 1)], capture_output=True, text=True, timeout=60)
        self.port = int(free.stdout.split()[0])
        low, high = LANE_COTURN['relay']
        args = [f'--listening-port={self.port}', f'--listening-ip={self.address}', f'--relay-ip={self.address}', f'--min-port={low}',
                f'--max-port={high}', '--realm=relay-proof.lane', '--use-auth-secret', '--fingerprint', '--no-tls', '--no-multicast-peers',
                '--no-stdout-log', '--log-file=/dev/null', '--simple-log']
        starter = base64.b64encode(COTURN_STARTER.encode()).decode()
        command = f"/usr/bin/python3 -c \"import base64;exec(base64.b64decode('{starter}'))\" {LANE_COTURN['binary']} '{json.dumps(args)}'"
        done = subprocess.run(['ssh', '-o', 'BatchMode=yes', LANE_COTURN['ssh'], command], input=f'{self.secret}\n',
                              capture_output=True, text=True, timeout=60)
        lines = [line for line in done.stdout.splitlines() if line.strip()]
        self.pid = int(lines[0]) if lines and lines[0].strip().isdigit() else None
        if not self.pid or not any(line.startswith('served ') and int(line.split()[1]) > 0 for line in lines):
            raise RuntimeError(f'the run coturn did not start or never read its configuration: {lines[-2:]}')
        time.sleep(2)
        try:
            answer = turn_allocate((self.address, self.port), 'probe-not-a-login', 'x')
        except OSError as error:
            raise RuntimeError(f'the run coturn does not answer ({type(error).__name__})') from error
        if answer.get('error_code') != 401:
            raise RuntimeError(f'the run coturn answered an unknown login with {answer}')

    def stop(self) -> None:
        if self.pid and not DRY_RUN:
            subprocess.run(['ssh', '-o', 'BatchMode=yes', LANE_COTURN['ssh'],
                            f'kill {self.pid}; sleep 3; ps -p {self.pid} > /dev/null && kill -9 {self.pid}; true'], capture_output=True, timeout=60)
        self.pid = None


def pump(stream: socket.socket, deliver, counts: dict, key: str) -> None:
    """2-byte-length frames off a TCP stream, each handed on as one datagram."""
    try:
        while True:
            header = b''
            while len(header) < 2:
                chunk = stream.recv(2 - len(header))
                if not chunk:
                    return
                header += chunk
            size, data = struct.unpack('>H', header)[0], b''
            while len(data) < size:
                chunk = stream.recv(size - len(data))
                if not chunk:
                    return
                data += chunk
            deliver(data)
            counts[key] += 1
    except OSError:
        return


def bridge_edith(bind: str, tcp_port: int) -> int:
    """On EDITH, in an ssh session: the engines' TURN sockets talk UDP to <bind> (EDITH's own LAN address: ICE binds
    interface addresses, so a loopback TURN server is never tried); each source gets its own TCP stream through the ssh -R
    forward to the driver's end, which speaks UDP to our relay."""
    host, port = bind.rsplit(':', 1)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((host, int(port)))
    flows, counts = {}, dict(up=0, down=0, flows=0)
    print(f'bridge pid {os.getpid()} udp {bind} -> tcp 127.0.0.1:{tcp_port}', flush=True)
    while True:
        try:
            datagram, source = sock.recvfrom(65535)
        except ConnectionResetError:
            continue
        stream = flows.get(source)
        if stream is None:
            stream = socket.create_connection(('127.0.0.1', tcp_port))
            stream.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            flows[source] = stream
            counts['flows'] += 1
            threading.Thread(target=pump, args=(stream, lambda data, to=source: sock.sendto(data, to), counts, 'down'), daemon=True).start()
        stream.sendall(struct.pack('>H', len(datagram)) + datagram)
        counts['up'] += 1


class Bridge:
    """The driver's end of EDITH's bridge to our relay: each tunnel stream becomes one UDP flow to the run coturn."""

    def __init__(self, box: Box, target: tuple[str, int], tcp_port: int, udp_bind: str, log_path: Path, payload: Path) -> None:
        self.box, self.target, self.tcp_port, self.udp_bind, self.log_path, self.payload = box, target, tcp_port, udp_bind, log_path, payload
        self.counts, self.remote, self.remote_pid, self.listener = dict(up=0, down=0, flows=0), None, None, None

    def serve(self) -> None:
        while True:
            try:
                stream, _ = self.listener.accept()
            except OSError:
                return
            stream.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            udp.connect(self.target)
            self.counts['flows'] += 1

            def back(udp=udp, stream=stream):
                try:
                    while True:
                        datagram = udp.recv(65535)
                        stream.sendall(struct.pack('>H', len(datagram)) + datagram)
                        self.counts['down'] += 1
                except OSError:
                    return
            threading.Thread(target=pump, args=(stream, udp.send, self.counts, 'up'), daemon=True).start()
            threading.Thread(target=back, daemon=True).start()

    def open(self) -> None:
        if DRY_RUN:
            say(f'dry-run: bridge {self.box.name} udp {self.udp_bind} -> tcp {self.tcp_port} -> {self.target[0]}:{self.target[1]}')
            return
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.bind(('127.0.0.1', self.tcp_port))
        self.listener.listen(16)
        threading.Thread(target=self.serve, daemon=True).start()
        argv = ['ssh', '-o', 'BatchMode=yes', self.box.alias,
                f"python '{(self.payload / 'relay_cloudflare_match.py').as_posix()}' --bridge-edith {self.udp_bind} {self.tcp_port}"]
        self.remote = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                       creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        found, seen = None, []
        while self.remote.stdout and not found and len(seen) < 20:  # ssh's own warnings come first
            line = self.remote.stdout.readline()
            if not line:
                break
            seen.append(line)
            found = re.search(r'bridge pid (\d+)', line)
        if not found:
            raise RuntimeError(f'the EDITH end of the bridge did not start: {"".join(seen)[-300:]!r}')
        self.remote_pid = int(found[1])

    def close(self) -> None:
        try:
            if self.remote_pid and not DRY_RUN:
                self.box.remote.ssh(f'Stop-Process -Id {self.remote_pid} -Force', check=False)
            if self.remote and self.remote.poll() is None:
                self.remote.terminate()
                self.remote.wait(timeout=10)
        finally:
            if self.listener:
                self.listener.close()
            with self.log_path.open('a') as stream:
                stream.write(f'{stamp()} bridge {json.dumps(self.counts)} target={self.target[0]}:{self.target[1]}\n')


# --- one run ---------------------------------------------------------------------------------------------------------

def display_name(run: dict, peer: dict) -> str:
    """Each seat's name, given to the engine with -net-player-name: the directory lists the host by it, the summary's
    holds and a migration's successor are read by it."""
    return f'{run.get("tag", "relayproof")}-{peer["name"]}'[:24]


def peer_settings(run: dict, peer: dict, port: int, pin: str, login=None) -> dict:
    """What the player chose for this match, seeded before the engine starts: the directory, the connection, the host's relay
    mode. Every inherited relay setting is cleared (the server lists and the logins, the player's own and the host's), so the
    only TURN list a connection can hold is the directory's offer; a login is never seeded (a Fixed pair is entered by the
    menu automation on the box). A public row keeps the game's own directory and the system's certificate check."""
    public = run.get('directory') == 'public'
    settings = {'SessionDirectoryUrl': PUBLIC_DIRECTORY if public else f'127.0.0.1:{port}', 'SessionDirectoryCertSha256': '' if public else pin,
                'SessionDirectoryInstallKey': f'relay-proof-{peer["box"]}-{peer["name"]}'[:32], 'NetworkIceEnable': '1',
                'NetworkConnectionMode': peer.get('settings_connection', peer['connection']), 'NetworkShowDiagnostics': '1',
                'NetworkMatchStatusMode': 'Always', 'NetworkDisplayName': display_name(run, peer),
                'NetworkHostRelayMode': 'Directory', **{key: '' for key in RELAY_SETTINGS}}
    if not run.get('record_replay', True):
        settings['NetworkRecordReplays'] = '0'
    return settings


def build_specs(h, run: dict, root: Path, ports: dict, boxes: dict, pin: str, login, ticks: int) -> list[dict]:
    """One spec per peer. A service peer plays the feel driver's two-peer arm (-net-match-service-e2e); a menu peer starts
    at the main menu and its menu script makes the player's choices, then hosts or joins through the menus (row f, the
    Fixed row); the trace cap ends a menu match at the declared frame on every peer."""
    from edith_cross import match_spec
    specs = []
    humans = str(len(run['peers']))
    for peer in run['peers']:
        box = boxes[peer['box']]
        name = display_name(run, peer)
        recording = ['-net-replay-out', str(root / 'match.ccreplay')] if run.get('record_replay', True) and peer['name'] == 'host' else []
        role = (['-net-host', *recording, '-net-ice', 'on'] if peer['name'] == 'host' else ['-net-join-session', '{SESSION}', '-net-ice', 'on'])
        spec = match_spec(peer['name'], root, ports[peer['name']], [] if peer.get('menu_script') else role,
                          peer_settings(run, peer, ports['directory'], pin), repo=Path(box.tree), ticks=ticks,
                          timeout=int(run.get('timeout_s', 420)))
        if peer.get('menu_script'):
            out = root / peer['name']
            flags = ['-tick-hashes', '-max-ticks', str(ticks), '-net-match-ticks', str(ticks),
                     '-net-live-tick-hashes', str(root / f'{peer["name"]}-live.jsonl'), '-out', str(root / f'{peer["name"]}_trace.json'),
                     '-input-script', str(root / 'input.txt'), '-feel-render-settings', str(out / 'runtime/Userdata/FeelRender.ini'),
                     '-net-match-report', str(root / f'{peer["name"]}_report.json'), *recording, '-net-player-name', name,
                     '-menu-script', str(root / f'{peer["name"]}.menu.txt')]
        else:
            flags = list(spec['flags'])
            for option in ('-net-match-humans', '-net-match-peers'):
                flags[flags.index(option) + 1] = humans
            flags += ['-net-player-name', name]
        spec['flags'] = flags
        if peer['name'] == 'host' and run.get('kill_host_at_tick'):
            spec['kill_at_tick'] = int(run['kill_host_at_tick'])
        if peer.get('panel_probe'):
            spec['env']['CC_TEST_NET_UI_SCRIPT'] = str(root / f'{peer["name"]}-panel' / 'probe.json')
        spec.update(box=peer['box'], tailscale_down=bool(peer.get('tailscale_down')), panel_probe=peer.get('panel_probe'),
                    panel_route=peer.get('panel_route'), menu_script=peer.get('menu_script'), name=name)
        specs.append(spec)
    return specs


def ship(box, root: Path, specs: list[dict], directory_port: int, payload: Path, public: bool = False, host_name: str = '',
         menu_texts: dict[str, str] | None = None) -> Path:
    """The box's spec (its peers in start order) and the drivers; a menu script is written straight onto the box from
    memory (a Fixed pair inside one never touches this box's disk); returns the local copy of the rendered task payload."""
    spec_path = root / f'{box.name}-peers.json'
    document = dict(root=str(root), lane=LANE, directory=dict(port=directory_port, cert=str(root / 'cert.pem'), public=public,
                    host_name=host_name), peers=specs)
    if DRY_RUN:
        say(f'dry-run: {box.name} runs {[spec["peer"] for spec in specs]} from {box.tree}: ' + json.dumps([spec['settings'] for spec in specs]))
        return root / f'{box.name}-run.ps1'
    box.remote.mkdir(root)
    box.remote.mkdir(payload / 'edith')
    for local, remote in ((HERE / 'relay_cloudflare_match.py', payload / 'relay_cloudflare_match.py'),
                          (HERE / 'relay_secrets.py', payload / 'relay_secrets.py'), (HERE / 'relay_fixtures.json', payload / 'relay_fixtures.json'),
                          (HERE / 'edith_cross.py', payload / 'edith_cross.py'),
                          (HERE / 'edith/remote_box.py', payload / 'edith/remote_box.py'), (root / 'cert.pem', root / 'cert.pem'),
                          (root / 'input.txt', root / 'input.txt'), (root / 'input-schedule.json', root / 'input-schedule.json')):
        if Path(local).is_file():
            box.remote.scp_to(local, remote)
    for spec in specs:
        if spec.get('panel_probe'):
            probe = (SCENARIO_DIR / spec['panel_probe']).read_text(encoding='utf-8').replace('{PANEL_ROUTE}', spec.get('panel_route') or '')
            local = root / f'{spec["peer"]}-panel' / 'probe.json'
            local.parent.mkdir(exist_ok=True)
            local.write_text(probe, encoding='utf-8')
            box.remote.mkdir(local.parent)
            box.remote.scp_to(local, local)
        if spec.get('menu_script'):
            text = (menu_texts or {}).get(spec['peer'])
            if text is None:
                text = (SCENARIO_DIR / spec['menu_script']).read_text(encoding='utf-8')
            put_remote_text(box, root / f'{spec["peer"]}.menu.txt', text)
    write_json(spec_path, document)
    box.remote.scp_to(spec_path, spec_path)
    from remote_box import render_payload
    script = render_payload(box.tree, [str(payload / 'relay_cloudflare_match.py'), '--remote-peers', str(spec_path)],
                            root / f'{box.name}-payload.log', root / f'{box.name}-payload.done',
                            {'CC_RELAY_LANE': LANE, 'CC_EDITH_CROSS_LANE': LANE}, box.path_prepend)
    local = root / f'{box.name}-run.ps1'
    local.write_text(script, encoding='utf-8')
    return local


def put_remote_text(box, path: Path, text: str) -> None:
    """Writes text to a file on the box through ssh's stdin: nothing is staged on this box."""
    if DRY_RUN:
        say(f'dry-run: write {len(text)} characters to {box.name}:{path}')
        return
    command = f"$t = [Console]::In.ReadToEnd(); [IO.File]::WriteAllText('{Path(path).as_posix()}', $t)"
    done = subprocess.run(['ssh', '-o', 'BatchMode=yes', box.alias, command], input=text, capture_output=True, text=True, timeout=120,
                          creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    if done.returncode:
        raise RuntimeError(f'could not write {path.name} on {box.name}')


def stun_legs(box, targets: list[str]) -> list[str]:
    """Each box's own STUN binding round trip to the relay servers: what one leg to the relay costs from there."""
    if DRY_RUN:
        return []
    code = (HERE / 'relay_stun_probe.py').read_text(encoding='utf-8')
    encoded = base64.b64encode(code.encode()).decode()
    args = ', '.join(repr(target) for target in targets)
    text = box.remote.ssh(f"python -c \"import base64,sys;sys.argv=['probe', {args}];exec(base64.b64decode('{encoded}'))\"", timeout=60)
    return [line.strip() for line in text.splitlines() if line.strip()]


def sanitize_box(box, root: Path, book, payload: Path) -> dict:
    """On the box: its run root swept with the book's salted digests (and every login shape), scrubbed, swept again; the
    digest file removed after. Returns the box's receipt (counts, never values); an unreachable box is INCOMPLETE."""
    if DRY_RUN:
        return dict(box=box.name, status='CLEAN', dry_run=True)
    digests = root / f'{box.name}-digests.json'
    receipt_path = root / f'{box.name}-sanitize.json'
    try:
        write_json(digests, book.digests())
        box.remote.scp_to(HERE / 'relay_secrets.py', payload / 'relay_secrets.py')
        box.remote.scp_to(HERE / 'relay_fixtures.json', payload / 'relay_fixtures.json')
        box.remote.scp_to(digests, digests)
        digests.unlink()
        exit_code = None
        try:
            done = subprocess.run(['ssh', '-o', 'BatchMode=yes', box.alias,
                                   f"python '{(payload / 'relay_secrets.py').as_posix()}' sweep --root '{root.as_posix()}' --digests "
                                   f"'{digests.as_posix()}' --fixtures '{(payload / 'relay_fixtures.json').as_posix()}' --scrub --out '{receipt_path.as_posix()}'; "
                                   f"$code = $LASTEXITCODE; "
                                   f"Remove-Item -LiteralPath '{digests.as_posix()}' -Force; exit $code"],
                                  capture_output=True, text=True, timeout=1800, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            exit_code = done.returncode
        finally:
            text = box.remote.read_text(receipt_path)
        receipt = json.loads(text) if text else {}
        first, verify = receipt.get('sweep') or {}, receipt.get('verify') or {}
        return dict(box=box.name, exit_code=exit_code, status=verify.get('status', 'INCOMPLETE') if exit_code in (0, 1, 3) else 'INCOMPLETE',
                    files=verify.get('files_scanned'), hits_before=len(first.get('files_with_secrets', [])),
                    hits_after=len(verify.get('files_with_secrets', [])), incomplete=verify.get('incomplete', []) + first.get('incomplete', []),
                    forms=sorted({form for row in first.get('files_with_secrets', []) for form in row.get('forms', [])}),
                    paths=[row['path'] for row in first.get('files_with_secrets', [])])
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        return dict(box=box.name, status='INCOMPLETE', error=f'{type(error).__name__}: {str(error)[-200:]}')
    finally:
        if digests.exists():
            digests.unlink()


def sanitize_local(root: Path, book) -> dict:
    from relay_secrets import sweep
    first = sweep([root], book.finder(), scrub=True)
    verify = sweep([root], book.finder())
    write_json(root / 'secret-scan.json', dict(sweep=first, verify=verify))
    return dict(box='here', status=verify['status'], files=verify['files_scanned'], hits_before=len(first['files_with_secrets']),
                hits_after=len(verify['files_with_secrets']), incomplete=verify['incomplete'],
                forms=sorted({form for row in first['files_with_secrets'] for form in row.get('forms', [])}),
                paths=[row['path'] for row in first['files_with_secrets']])


def observe_public_offer(session: str, book) -> list[dict]:
    """A public row's minted logins, read as a joiner reads them (GET the session's ICE list) and booked in memory."""
    try:
        request = urllib.request.Request(f'https://{PUBLIC_DIRECTORY}/v1/sessions/{session}/ice-servers',
                                         headers={'X-Install-Key': 'relay-proof-offer-observer', 'User-Agent': 'cccp-relay-proof/1'})
        with urllib.request.urlopen(request, context=ssl.create_default_context(), timeout=15) as reply:
            offer = json.load(reply)
    except (OSError, ValueError):
        return []
    book.add_offer(offer)
    return [dict(username=server['username'], expires_at=offer.get('expires_at')) for server in offer.get('iceServers', []) if server.get('username')]


class PublicObserver:
    """A public row's offer, watched from this box while the match runs: the host's listing found by its seat name, then
    the session's ICE list read as a joiner reads it every 20 s (a renewal mints a new login); every login is booked in
    memory for the sweep and revoked at the run's end. Nothing it reads is written."""

    def __init__(self, host_name: str, book) -> None:
        self.host_name, self.book, self.session, self.minted = host_name, book, None, []
        self.stopping = threading.Event()
        self.thread = threading.Thread(target=self.watch, daemon=True)

    def watch(self) -> None:
        while not self.stopping.is_set():
            if self.session is None:
                row = directory_session(dict(public=True, host_name=self.host_name), 20)
                self.session = row and row.get('session_id')
            if self.session:
                for login in observe_public_offer(self.session, self.book):
                    if login['username'] not in {row['username'] for row in self.minted}:
                        self.minted.append(login)
            self.stopping.wait(20)

    def start(self) -> None:
        if not DRY_RUN:
            self.thread.start()

    def stop(self) -> None:
        self.stopping.set()
        if self.thread.is_alive():
            self.thread.join(timeout=60)


def run_one(scenario: dict, run: dict, out: Path, boxes: dict, ticks: int, book) -> dict:
    import edith_cross
    from relay_secrets import read_turn_config
    root = (out / run['name']).resolve()
    used = [boxes[name] for name in dict.fromkeys(peer['box'] for peer in run['peers'])]
    for box in used:
        engines = sum(peer['box'] == box.name for peer in run['peers'])
        if engines > box.max_engines:
            raise RuntimeError(f'{box.name} holds {box.max_engines} engine(s); the run asks for {engines}')
    say(f'{run["name"]}: {[(peer["name"], peer["box"], peer["connection"]) for peer in run["peers"]]} relay={run["relay"]} ticks={ticks} {stamp()}')
    if not DRY_RUN:
        root.mkdir(parents=True, exist_ok=False)
    identities = {box.name: identity(box) for box in used}
    heads = {value['head'] for value in identities.values()}
    exes = {value['executable_sha256'] for value in identities.values()}
    if not DRY_RUN:
        write_json(root / 'identities.json', identities)
    if not DRY_RUN and (any(value['status'] != 'PASS' for value in identities.values()) or len(heads) != 1 or len(exes) != 1):
        refusal = f'the boxes do not run one receipted build: {[(name, value["head"][:10], str(value["executable_sha256"])[:12], value["errors"]) for name, value in identities.items()]}'
        say(f'{run["name"]}: REFUSED {refusal}')
        return dict(name=run['name'], root=str(root), passed=False, refused=refusal, identities=identities)
    h = edith_cross.harness(HERE)
    public = run.get('directory') == 'public'
    run.setdefault('tag', f'rp{os.urandom(3).hex()}')
    backend = coturn = bridge = directory = observer = None
    fixed_pair = pair_ttl = minted_at = relay_hosts = None
    if run['relay'] in ('cloudflare', 'automatic') and not public:
        backend = read_turn_config(CLOUDFLARE_TURN_CONFIG)
        book.add_turn_config(backend)
    names = [peer['name'] for peer in run['peers']]
    block = choose_ports(used, len(names) + 3)
    ports = dict(directory=block[0], bridge_tcp=block[1], bridge_udp=block[2], **{name: block[index + 3] for index, name in enumerate(names)})
    if not DRY_RUN:
        edith_cross.looped_input(h, root / 'input.txt', ticks)
    tunnels: list[Tunnel] = []
    started, states, payload = stamp(), {}, Path('D:/mx') / lane_for(out) / 'payload'
    sanitize, cleanup, fetched, revokes, menus, clocks = [], [], [], [], [], {}
    failure = interrupt = None
    overrides_cleared, ended_epoch = False, time.time()
    host_name = display_name(run, next(peer for peer in run['peers'] if peer['name'] == 'host'))
    # One boundary for every way out (a failure, a timeout, an interrupt): the finally below closes, blanks, sweeps on
    # each box and here, revokes, and only then may an interrupt carry on.
    try:
        if run['relay'] in ('coturn', 'fixed'):
            coturn = LaneCoturn(book)
            coturn.start()
            edith_box = next((box for box in used if box.name == 'edith'), None)
            bridge_url = None
            if edith_box:
                lan = box_lan_address(edith_box)
                bridge = Bridge(edith_box, (coturn.address, coturn.port), ports['bridge_tcp'], f'{lan}:{ports["bridge_udp"]}',
                                root / 'bridge.log', payload)
                bridge_url = f'turn:{lan}:{ports["bridge_udp"]}?transport=udp'
                relay_hosts = [lan]
            relay_urls = [bridge_url] if bridge_url else [f'turn:{coturn.address}:{coturn.port}?transport=udp']
            relay_hosts = relay_hosts or [coturn.address]
            if run['relay'] == 'coturn':
                backend = dict(backend='coturn', static_auth_secret=coturn.secret, relay_urls=relay_urls)
            else:
                minted_at = int(time.time())
                fixed_pair = coturn_rest_login(coturn.secret, FIXED_PAIR_TTL, minted_at)
                book.add('fixed-row-username', fixed_pair[0])
                book.add('fixed-row-credential', fixed_pair[1])
                run['fixed_url'] = relay_urls[0]
        if not DRY_RUN and not public:
            directory = Directory(root, ports['directory'], backend, int(run.get('relay_ttl_cap', 86400)), book)
        pin = directory.pin if directory else '<pin>'
        for box in used:
            if not public:
                forwards = [ports['directory']] + ([ports['bridge_tcp']] if bridge and box is bridge.box else [])
                tunnels.append(Tunnel(box, forwards, root / f'tunnel-{box.name}.log'))
        for tunnel in tunnels:
            tunnel.open()
        specs = build_specs(h, run, root, ports, boxes, pin, None, ticks)
        overrides_cleared = all(spec['settings'].get(key) == '' for spec in specs for key in RELAY_SETTINGS)
        menu_texts = render_menus(run, specs, fixed_pair, ports)
        menus = [(boxes[spec['box']], root / f'{spec["peer"]}.menu.txt') for spec in specs if spec.get('menu_script')]
        if bridge:
            ship_driver_only(bridge.box, payload)
            bridge.open()
        scripts = {box.name: ship(box, root, [spec for spec in specs if spec['box'] == box.name], ports['directory'], payload, public,
                                  host_name, menu_texts) for box in used}
        if public:
            observer = PublicObserver(host_name, book)
            observer.start()
        clocks = {box.name: dict(before=measure_clock(box)) for box in used}
        host_box = next(peer['box'] for peer in run['peers'] if peer['name'] == 'host')
        for box in sorted(used, key=lambda box: box.name != host_box):
            if not DRY_RUN:  # the host's box first; a joiner's own payload waits for the host's listing
                box.remote.start_task(scripts[box.name], budget_s=1800)
            say(f'{run["name"]}: payload started on {box.name} through {box.task}')
        for box in used:
            states[box.name] = wait_done(box, root / f'{box.name}-payload.done', int(run.get('timeout_s', 420)) + 900)
            say(f'{run["name"]}: {box.name} {states[box.name].strip()}')
        ended_epoch = time.time()
        for box in used:
            clocks[box.name]['after'] = measure_clock(box)
    except BaseException as error:
        failure = redacted(f'{type(error).__name__}: {error}')
        ended_epoch = time.time()
        overrides_cleared = False
        say(f'{run["name"]}: {failure}')
        if not isinstance(error, Exception):
            interrupt = error
    finally:
        steps = [(f'tunnel {tunnel.box.name}', tunnel.close) for tunnel in tunnels]
        steps += [(name, part.close if name == 'bridge' else part.stop) for name, part in
                  (('bridge', bridge), ('directory', directory), ('observer', observer)) if part is not None]
        for step, close in steps:
            try:
                close()
            except Exception as error:
                cleanup.append(redacted(f'{step}: {type(error).__name__}: {error}'))
        for box, path in menus:  # a script the harness expanded is blanked whether or not an engine loaded it
            try:
                put_remote_text(box, path, MENU_BLANK)
            except Exception as error:
                cleanup.append(redacted(f'menu {path.name}: {type(error).__name__}: {error}'))
        if not DRY_RUN:
            for box in used:
                sanitize.append(sanitize_box(box, root, book, payload))
        if coturn:
            try:
                if fixed_pair and failure is None and not DRY_RUN:
                    pair_ttl = pair_ttl_receipt((coturn.address, coturn.port), fixed_pair, coturn.secret, book)
            except Exception as error:
                pair_ttl = dict(passed=False, reasons=[redacted(f'{type(error).__name__}: {error}')])
            finally:
                coturn.stop()
        if not DRY_RUN:
            for box, receipt in zip(used, list(sanitize)):
                if interrupt is not None or receipt['status'] != 'CLEAN':
                    continue
                listing = root / f'{box.name}-evidence.txt'
                try:
                    box.remote.ssh(f"python '{(payload / 'relay_cloudflare_match.py').as_posix()}' --evidence-list '{root.as_posix()}' --out "
                                   f"'{listing.as_posix()}'", timeout=600)
                    box.remote.fetch_list(root, listing, root, f'{box.name}-evidence.tar')
                    fetched.append(box.name)
                except (RuntimeError, subprocess.TimeoutExpired) as error:
                    sanitize.append(dict(box=box.name, status='INCOMPLETE', error=redacted(f'fetch: {type(error).__name__}: {str(error)[-200:]}')))
            sanitize.append(sanitize_local(root, book))
            observed = observer.minted if observer else []
            if observed:
                revokes = revoke_cloudflare(read_turn_config(CLOUDFLARE_TURN_CONFIG), [row['username'] for row in observed])
            elif directory:
                revokes = directory.revokes
    if interrupt is not None:
        raise interrupt
    if DRY_RUN:
        return dict(name=run['name'], dry_run=True, ports=ports, failure=failure)
    observed = observer.minted if observer else []
    minted = (directory.minted if directory else observed) or (
        [dict(username=fixed_pair[0], expires_at=int(fixed_pair[0].split(':', 1)[0]), minted_at=minted_at)] if fixed_pair else [])
    if failure:
        return dict(name=run['name'], root=str(root), passed=False, refused=failure, identities=identities, sanitize=sanitize,
                    revokes=revokes, cleanup=cleanup)
    return judge_run(h, scenario, run, root, dict(started=started, finished=stamp(), states=states, identities=identities,
                     legs={}, ports=ports, sessions=directory.sessions() if directory else [],
                     signals=directory.signals if directory else [], offers_seen=directory.offers if directory else None,
                     provider_calls=directory.provider_calls if directory else [], revokes=revokes, minted=minted, public=public,
                     ticks=ticks, ended_epoch=ended_epoch, overrides_cleared=overrides_cleared, sanitize=sanitize, fetched=fetched,
                     relay_hosts=relay_hosts, coturn_address=coturn.address if coturn else None, fixed_pair=fixed_pair,
                     pair_ttl=pair_ttl, cleanup=cleanup, minted_at=minted_at, clocks=clocks), book)


MENU_BLANK = '# the menu script was removed by the driver at the end of its run\n'


def measure_clock(box) -> dict:
    """The game box's clock against this box's: offset (box minus here) and its uncertainty (half the ssh round trip)."""
    if DRY_RUN:
        return dict(offset_s=0.0, uncertainty_s=0.0)
    sent = time.time()
    text = box.remote.ssh('[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()')
    back = time.time()
    remote = int(text.strip().splitlines()[-1]) / 1000
    return dict(offset_s=remote - (sent + back) / 2, uncertainty_s=(back - sent) / 2)


def ship_driver_only(box, payload: Path) -> None:
    if DRY_RUN:
        return
    box.remote.mkdir(payload / 'edith')
    box.remote.scp_to(HERE / 'relay_cloudflare_match.py', payload / 'relay_cloudflare_match.py')


def render_menus(run: dict, specs: list[dict], fixed_pair, ports: dict) -> dict[str, str]:
    """Each menu peer's script with its tokens filled, in memory: the host's port, the Fixed row's relay and pair."""
    texts = {}
    for spec in specs:
        if not spec.get('menu_script'):
            continue
        text = (SCENARIO_DIR / spec['menu_script']).read_text(encoding='utf-8')
        values = dict(PORT=str(ports.get('host', '')), PLAYER=spec.get('name', ''))
        if fixed_pair:
            values.update(TURN_SERVER=run.get('fixed_url', ''), TURN_USER=fixed_pair[0], TURN_PASS=fixed_pair[1])
        for key, value in values.items():
            text = text.replace('{' + key + '}', value)
        texts[spec['peer']] = text
    return texts


def wait_done(box, done: Path, budget_s: float) -> str:
    """The payload's done line. A box whose only path is its tailnet is unreachable while a row has the tunnel down:
    that is retried, never read as an end."""
    deadline = time.monotonic() + budget_s
    while True:
        try:
            return box.remote.wait_done(done, max(30.0, deadline - time.monotonic()))
        except (RuntimeError, subprocess.TimeoutExpired) as error:
            if time.monotonic() > deadline:
                return f'UNREACHABLE {type(error).__name__}: {str(error)[-200:]}'
            say(f'{box.name} unreachable ({type(error).__name__}); retrying')
            time.sleep(30)


def public_directory_lines(session: str | None) -> list[str]:
    """The public directory's own log lines for one session (read-only, from the Mac that serves it)."""
    if not session or not re.fullmatch(r'[0-9a-fA-F-]{8,64}', session):
        return []
    host, logs = PUBLIC_DIRECTORY_LOGS
    done = subprocess.run(['ssh', '-o', 'BatchMode=yes', host, f"grep -h -F '{session}' {logs}/* 2>/dev/null | tail -400"],
                          capture_output=True, text=True, timeout=120, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    return done.stdout.splitlines()


def judge_run(h, scenario: dict, run: dict, root: Path, facts: dict, book) -> dict:
    """The run's verdict from its own files: exactly the checks its row's table names, each computed from the evidence
    that check reads, a missing one failing."""
    import edith_cross
    names = [peer['name'] for peer in run['peers']]
    box_of = {peer['name']: peer['box'] for peer in run['peers']}
    ticks = int(facts['ticks'])
    required = REQUIRED.get(run['name'], BASE)
    records = {name: read_json(root / f'{name}-record.json') for name in names}
    logs = {name: edith_cross.peer_log(root, name) for name in names}
    reports = {name: read_json(root / f'{name}_report.json') for name in names}
    host_log = logs.get('host', '')
    session = next(iter(re.findall(r'\[net-ice\] host session (\S+)', host_log)), None) or (facts['sessions'][0] if facts['sessions'] else None)
    dropped = {peer['name'] for peer in run['peers'] if peer['name'] == 'host' and run.get('kill_host_at_tick')}
    complete = all((str(row.get('injected_termination', '')).startswith('scenario drop') if name in dropped else
                    row.get('exit_code') == 0 and row.get('evidence_complete')) and not row.get('timed_out') for name, row in records.items())
    hash_peers = [name for name in names if name not in dropped]
    manifest = dict(name=run['name'], mode=f'relay proof {run["relay"]}', ticks=ticks, lag_ms=0, cap_hz=60, instrumentation=False,
                    loss_percent=0, silent_tick=None, live_stalls=None, autosave_seconds=None,
                    per_peer_lag_ms={name: 0 for name in names}, launches_complete=complete,
                    exe={name: row.get('exe_sha256') for name, row in records.items()})
    write_json(root / 'manifest.json', manifest)
    try:
        h.records.compress_case_records(root)
        timing = h.feel.reduce_timing_case(root)
        for name in names:
            timing['peers'].setdefault(name, h.feel.timing_peer(root, name))
    except Exception as error:  # a pair that never matched still gets its verdict
        timing = dict(peers={}, proof={}, off_wire_pass=False, reduction_error=f'{type(error).__name__}: {error}')
    write_json(root / 'feel-report.json', timing)
    live = {}
    for index, left in enumerate(hash_peers):
        for right in hash_peers[index + 1:]:
            try:
                live[f'{left}/{right}'] = h.feel.compare_live_hashes(root / f'{left}-live.jsonl', root / f'{right}-live.jsonl', 1)
            except (OSError, ValueError) as error:
                live[f'{left}/{right}'] = [dict(compared_ticks=0, mismatched_ticks=1, error=f'{type(error).__name__}: {error}')]
    compared = {pair: sum(row['compared_ticks'] for row in rows) for pair, rows in live.items()}
    mismatched = sum(row['mismatched_ticks'] + row.get('mismatched_applied_input_ticks', 0) for rows in live.values() for row in rows)
    trace_pass = bool((timing.get('proof') or {}).get('pass')) if not dropped else None
    peers, seen = {}, {}
    for name in names:
        metrics = (timing.get('peers', {}).get(name) or {}).get('metrics', {})
        try:
            seen[name] = set(edith_cross.live_ticks(root / f'{name}-live.jsonl'))
        except (ValueError, OSError):
            seen[name] = set()
        peers[name] = dict(box=box_of[name], exit_code=records[name].get('exit_code'),
                           timed_out=records[name].get('timed_out'), exe=str(records[name].get('exe_sha256'))[:16], live_ticks=len(seen[name]),
                           last_tick=max(seen[name], default=0), holds=len(re.findall(r'\[net-match\] hold peer=\d+ frame=\d+', logs[name])),
                           wall_tps=metrics.get('steady_wall_tps'), longest_steady_wait_ms=metrics.get('longest_stall_ms'),
                           waiting_percent=(100 * metrics['net_wait_ms'] / metrics['steady_wall_ms']) if metrics.get('steady_wall_ms') else None,
                           input_delays=metrics.get('peer_input_delays'),
                           timing_pass=(timing.get('peers', {}).get(name) or {}).get('pass_check'))
    last_ticks = {name: peers[name]['last_tick'] for name in names}
    # History is contiguous: every frame 1..N on every peer, no gap, and every one of them on every peer.
    contiguous = {name: bool(seen[name]) and seen[name] == set(range(1, max(seen[name]) + 1)) and max(seen[name]) >= ticks for name in hash_peers}
    common = set.intersection(*(seen[name] for name in hash_peers)) if hash_peers else set()
    covered = set(range(1, ticks + 1)) <= common
    joiner = next((name for name in names if name != 'host'), 'client')
    connections = {name: edith_cross.find_key(reports.get(name) or {}, 'connection') for name in names}
    identities = {name: edith_cross.find_key(reports.get(name) or {}, 'local_identity') for name in names if name != 'host'}
    if facts.get('public'):
        directory_lines = public_directory_lines(session)
        (root / 'public-directory.log').write_text('\n'.join(directory_lines) + '\n', encoding='utf-8')
    else:
        directory_lines = (root / 'service.log').read_text(encoding='utf-8', errors='replace').splitlines() if (root / 'service.log').is_file() else []
    offers = []
    for line in directory_lines:
        if 'relay_offer_issued ' in line:
            try:
                offers.append(json.loads(line.split('relay_offer_issued ', 1)[1]))
            except ValueError:
                pass
    offer_urls = None if facts['offers_seen'] is None else next((row['urls'] for row in facts['offers_seen'] if row['session_id'] == session), [])
    mode = run['relay']
    provider = run.get('provider') or {'cloudflare': 'cloudflare', 'coturn': 'coturn', 'fixed': 'fixed', 'automatic': 'cloudflare'}[mode]
    relay = judge_relay(dict(mode=mode, provider=provider, session_id=session, logs=logs, signals=facts['signals'], identities=identities,
                             offers=offers, offer_urls=offer_urls,
                             connection={peer['name']: peer.get('judged_connection', peer['connection']) for peer in run['peers']},
                             reports={name: value if isinstance(value, dict) else {} for name, value in connections.items()},
                             expect_routes=run.get('expect_routes'),
                             relay_addresses=[facts['coturn_address']] if facts.get('coturn_address') else None,
                             relay_hosts=facts.get('relay_hosts'), run_ends_at=int(facts['ended_epoch']),
                             overrides_cleared=facts.get('overrides_cleared')))
    owners = sender_peers(facts['signals'], identities, names)
    write_json(root / 'signals-candidates.json', [dict(peer=owners[sender], sender=sender, candidates=[
        f'{address}:{port} {kind}' for address, port, kind in signal_candidates(payload)],
        payload_sha256=hashlib.sha256(payload.encode()).hexdigest()) for sender, payload in facts['signals']])
    builds = edith_cross.pair_build_evidence(root, dict(source_sha=next(iter({value['head'] for value in facts['identities'].values()}))), records)
    rtts = transport_rtts(host_log)
    judged = run.get('timing_peers') or hash_peers
    sanitize = facts['sanitize']
    minted = facts.get('minted') or []
    clocks = {}
    for box, reading in (facts.get('clocks') or {}).items():
        values = [reading[key] for key in ('before', 'after') if reading.get(key)]
        if values:
            offsets = [value['offset_s'] for value in values]
            clocks[box] = (sum(offsets) / len(offsets), max(value['uncertainty_s'] for value in values) + (max(offsets) - min(offsets)) / 2)
    box_clock = {box: read_json(root / f'{box}-clock.json') for box in set(box_of.values())}
    details, registry = {}, []

    def detail(key, value):
        details[key] = value
        return bool(value.get('passed'))

    def registrants():
        addresses = sorted({address for entry in relay['peers'].values() for address in entry.get('relay_addresses') or []})
        registry.extend(registrant(address) for address in addresses)
        return bool(registry) and all(row.get('cloudflare') for row in registry)

    def peer_check(key):
        kind, name = key.split(':', 1)
        peer = next((peer for peer in run['peers'] if peer['name'] == name), None)
        if peer is None:
            return False
        if kind == 'tunnel':
            rows = read_json(root / f'{peer["box"]}-tailscale.json')
            return detail(key, tunnel_receipt(rows if isinstance(rows, list) else [], [name]))
        if kind == 'panel':
            return detail(key, panel_verdict(read_json(root / f'{name}-panel' / 'net-ui-result.json') or None))
        if kind == 'menu_choice':
            return detail(key, menu_choice(logs.get(name, ''), peer.get('menu_choice', 'Relay only'), session) if session
                          else dict(passed=False, reasons=['no session to bind the choice to']))
        return False

    def renewal():
        relayed = [peer['name'] for peer in run['peers'] if peer.get('judged_connection', peer['connection']) == 'RelayOnly']
        return detail('renewal', renewal_evidence(
            logs, facts['provider_calls'], relayed, ttl_s=int(run['relay_ttl_cap']) if run.get('relay_ttl_cap') else None,
            line_times={name: (box_clock.get(box_of[name]) or {}).get('lines', {}).get(name, []) for name in relayed},
            samples={name: (box_clock.get(box_of[name]) or {}).get('samples', {}).get(name, []) for name in relayed},
            first_expiry=minted[0].get('expires_at') if minted else None,
            clocks={name: clocks.get(box_of[name]) for name in relayed}, session=session))

    def seat_holds_check():
        summary = ((reports.get('host') or {}).get('last_match') or {}).get('peers') or []
        named = [dict(row, name=next((peer['name'] for peer in run['peers'] if display_name(run, peer) == row.get('name')), row.get('name')))
                 for row in summary]
        return detail('seat_holds', seat_holds(named, set(run.get('holds_allowed') or []), set(names)))

    def listing():
        listed = (read_json(root / f'{joiner}-listed.json') or {}).get('session_id') == session
        return detail('listing', listing_evidence(directory_lines, session, listed))

    def short_lived():
        if facts.get('fixed_pair'):
            expiry = int(facts['fixed_pair'][0].split(':', 1)[0])
            return facts.get('minted_at') is not None and 0 < expiry - facts['minted_at'] <= 900
        return bool(minted) and all(row.get('expires_at') and row.get('minted_at') and 0 < row['expires_at'] - row['minted_at'] <= 900
                                    for row in minted)

    compute = {
        'identities': lambda: all(value['status'] == 'PASS' for value in facts['identities'].values()),
        'builds': lambda: builds['passed'],
        'exits': lambda: complete,
        'full_history': lambda: bool(hash_peers) and all(contiguous[name] for name in hash_peers),
        'hashes_equal': lambda: bool(live) and mismatched == 0 and trace_pass is not False and covered and all(
            value >= ticks - int(run.get('kill_host_at_tick') or 0) for value in compared.values()),
        'holds': lambda: all(peers[name]['holds'] == 0 for name in judged),
        'feel_bars': lambda: all(feel_bars(timing, name)['passed'] for name in judged),
        'relay': lambda: relay['passed'],
        'route_receipts': lambda: relay['receipts_complete'],
        'endpoint': lambda: relay['endpoint'],
        'rtt_recorded': lambda: bool(rtts),
        'secrets_observed': lambda: bool(minted),
        'no_secret_in_files': lambda: bool(sanitize) and all(row.get('hits_before') == 0 and row['status'] == 'CLEAN' for row in sanitize),
        'sanitizer_clean': lambda: bool(sanitize) and all(row['status'] == 'CLEAN' and row.get('hits_after', 1) == 0 for row in sanitize)
                                   and len(facts.get('fetched', [])) == len(facts['identities']),
        'pair_blanked': lambda: bool(sanitize) and all(row['status'] == 'CLEAN' and row.get('hits_after') == 0 for row in sanitize),
        'offer_fresh': lambda: bool(relay['offer_fresh']),
        'direct_expected': lambda: bool(relay['direct_as_expected']),
        'provider_201': lambda: bool(facts['provider_calls']) and all(call['status'] == 201 for call in facts['provider_calls']),
        'logins_revoked': lambda: bool(facts['revokes']) and len(facts['revokes']) == len({row['username'] for row in minted})
                                  and all(status == 204 for status in facts['revokes']),
        'logins_short_lived': short_lived,
        'relay_registrant': registrants,
        'menu_entered': lambda: detail('menu_entered', menu_choice(logs.get('host', ''), 'Relay only', session) if session
                                       else dict(passed=False, reasons=['no session to bind the choice to'])),
        'pair_ttl': lambda: detail('pair_ttl', facts.get('pair_ttl') or dict(passed=False, reasons=['the pair was never asked after its TTL'])),
        'renewal': renewal,
        'migration': lambda: detail('migration', migration_declarations(
            logs, hash_peers, session, {peer['name']: display_name(run, peer) for peer in run['peers']}, last_ticks)),
        'listing': listing,
        'seat_holds': seat_holds_check,
    }
    checks = {}
    for name in required:
        producer = compute.get(name) or ((lambda key=name: peer_check(key)) if ':' in name else None)
        checks[name] = bool(producer()) if producer else False  # a check the run cannot produce fails
    if 'pair_blanked' in required:
        details['pair_on_disk'] = dict(passed=True, files=[(row['box'], row.get('hits_before')) for row in sanitize])
    details.update({f'feel:{name}': feel_bars(timing, name) for name in judged})
    details['history'] = dict(contiguous=contiguous, covered=covered, compared=compared)
    passed = all(checks.values())
    verdict = dict(name=run['name'], scenario=scenario['name'], relay=run['relay'], root=str(root), passed=passed, checks=checks,
                   required=list(required), details=details, registry=registry, session_id=session, peers=peers, relay_evidence=relay,
                   rtt=rtts, delay_changes=delay_changes(host_log), compared_ticks=compared, desyncs=mismatched, builds=builds,
                   provider_calls=facts['provider_calls'], revokes=facts.get('revokes'), minted_logins=len(minted),
                   stun_legs=facts['legs'], ports=facts['ports'], states=facts['states'], started=facts['started'], finished=facts['finished'],
                   sanitize=sanitize, ticks=ticks, clocks=clocks, binding_note=relay['binding_note'])
    write_json(root / 'relay-verdict.json', verdict)
    cell = lambda key, fmt='{}': '/'.join('-' if peers[name][key] is None else fmt.format(peers[name][key]) for name in names)
    say(f'RUN {run["name"]} {"PASS" if passed else "FAIL"} {json.dumps({k: v for k, v in checks.items() if not v})} '
        f'routes={relay["routes"]} rtt={[row["rtt_ms"] for row in rtts]} ticks={cell("last_tick")} desyncs={mismatched} '
        f'holds={cell("holds")} tps={cell("wall_tps", "{:.2f}")} longest_ms={cell("longest_steady_wait_ms", "{:.0f}")} '
        f'sanitize={[(row["box"], row["status"], row.get("hits_before"), row.get("hits_after")) for row in sanitize]} '
        f'relay_reasons={relay["reasons"][:3]}')
    return verdict


def pair_ttl_receipt(target, pair, secret: str, book, now=time.time, sleep=time.sleep, allocate=None) -> dict:
    """The Fixed row's pair, asked of our relay once its TTL has passed: refused with 401, while a fresh pair from the same
    secret is allocated by the same server at the same moment (so the refusal is the expiry, never a dead server)."""
    allocate = allocate or turn_allocate
    expiry = int(pair[0].split(':', 1)[0])
    wait = expiry - now() + 5
    if wait > 0:
        say(f'waiting {wait:.0f} s for the Fixed pair to pass its TTL')
        sleep(wait)
    control = coturn_rest_login(secret, 120)
    book.add('ttl-control-username', control[0])
    book.add('ttl-control-credential', control[1])
    try:
        alive = allocate(tuple(target), control[0], control[1])
        expired = allocate(tuple(target), pair[0], pair[1])
    except OSError as error:
        return dict(passed=False, reasons=[f'our relay did not answer: {type(error).__name__}'])
    dead = expired.get('result') == 'refused' and expired.get('error_code') == 401 and alive.get('result') == 'allocated'
    reasons = [] if dead else [f'the expired pair answered {expired} beside a fresh pair answering {alive}']
    return dict(passed=dead, expired_at=expiry, asked_at=int(now()), expired=expired, control=alive, reasons=reasons)


# --- on the box ------------------------------------------------------------------------------------------------------

def directory_session(directory: dict, budget_s: float) -> dict | None:
    """The host's listing row: the lane directory's only row, or the public directory's row carrying the host's name."""
    if directory.get('public'):
        url, context = f'https://{PUBLIC_DIRECTORY}/v1/sessions', ssl.create_default_context()
    else:
        url, context = f'https://127.0.0.1:{directory["port"]}/v1/sessions', ssl.create_default_context(cafile=directory['cert'])
    deadline = time.monotonic() + budget_s
    while time.monotonic() < deadline:
        try:
            request = urllib.request.Request(url, headers={'X-Install-Key': 'relay-proof-session-poll', 'User-Agent': 'cccp-relay-proof/1'})
            with urllib.request.urlopen(request, context=context, timeout=10) as reply:
                rows = json.load(reply).get('sessions') or []
            rows = [row for row in rows if not directory.get('public') or row.get('name') == directory.get('host_name')]
            if rows:
                return {key: rows[0].get(key) for key in ('session_id', 'name', 'state', 'join_mode')}
        except (OSError, ValueError):
            pass
        time.sleep(2)
    return None


def tailscale(command: list[str]) -> dict:
    """The tunnel's own state, for the receipt: 'tailscale status --json' BackendState, and a toggle's exit code."""
    exe = 'C:/Program Files/Tailscale/tailscale.exe'
    row = dict(at=stamp(), command=' '.join(command))
    try:
        if command[0] != 'status':
            done = subprocess.run([exe, *command], capture_output=True, text=True, timeout=60)
            row['exit_code'] = done.returncode
        status = subprocess.run([exe, 'status', '--json'], capture_output=True, text=True, timeout=60)
        row['backend_state'] = json.loads(status.stdout or '{}').get('BackendState')
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        row['error'] = f'{type(error).__name__}: {error}'
    return row


class BoxClock(threading.Thread):
    """Runs on a game box beside its engines: each second, every renewal, route or migration line its peers print and each
    peer's newest live tick, stamped with this box's clock, so the driver can judge them against its own events."""
    PATTERN = re.compile(r'\[net-relay\] relay login renewed|\[net-route\] RouteAllowed|\[net-ice\] selected candidate|is now hosting; boundary=')

    def __init__(self, root: Path, peers: list[str]) -> None:
        super().__init__(daemon=True)
        self.root, self.peers, self.stopping = root, peers, threading.Event()
        self.offsets = {peer: 0 for peer in peers}
        self.lines = {peer: [] for peer in peers}
        self.samples = {peer: [] for peer in peers}

    def run(self) -> None:
        while not self.stopping.is_set():
            self.sample()
            self.stopping.wait(1)
        self.sample()

    def sample(self) -> None:
        now = time.time()
        for peer in self.peers:
            try:
                with (self.root / peer / 'stdout.log').open('rb') as stream:
                    stream.seek(self.offsets[peer])
                    chunk = stream.read()
            except OSError:
                chunk = b''
            cut = chunk.rfind(b'\n') + 1
            self.offsets[peer] += cut
            for line in chunk[:cut].decode('utf-8', 'replace').splitlines():
                if self.PATTERN.search(line):
                    self.lines[peer].append([now, line.strip()[:300]])
            tick = last_live_tick(self.root / f'{peer}-live.jsonl')
            if tick is not None and (not self.samples[peer] or self.samples[peer][-1][1] != tick):
                self.samples[peer].append([now, tick])

    def finish(self, path: Path) -> None:
        self.stopping.set()
        self.join(timeout=10)
        write_json(path, dict(clock='this box, epoch seconds', lines=self.lines, samples=self.samples))


def last_live_tick(path: Path) -> int | None:
    try:
        with path.open('rb') as stream:
            stream.seek(0, 2)
            stream.seek(max(0, stream.tell() - 8192))
            tail = stream.read().rstrip().rsplit(b'\n', 1)[-1]
        return int(json.loads(tail).get('tick'))
    except (OSError, ValueError, TypeError, AttributeError):
        return None


def remote_peers(spec_path: str) -> int:
    """Runs on a game box inside its session task: its peers through the runner, a joiner after the host is listed. The
    tailnet a row took down comes back up in a finally that survives any cleanup error, with its receipt."""
    import edith_cross
    document = json.loads(Path(spec_path).read_text(encoding='utf-8'))
    root = Path(document['root'])
    specs = document['peers']
    toggled = any(spec.get('tailscale_down') for spec in specs)
    receipt: list[dict] = []
    running, results = [], {}
    clock = BoxClock(root, [spec['peer'] for spec in specs])
    clock.start()
    try:
        h = edith_cross.harness(Path(specs[0]['repo']) / 'tools')
        if toggled:
            receipt.append(dict(tailscale(['status']), step='before'))
            receipt.append(dict(tailscale(['down']), step='down'))
        try:
            for spec in specs:
                menu = root / f'{spec["peer"]}.menu.txt'
                needs_menu_session = spec.get('menu_script') and menu.is_file() and '{DIRECTORY_SESSION}' in menu.read_text(encoding='utf-8')
                if '{SESSION}' in spec['flags'] or needs_menu_session:
                    row = directory_session(document['directory'], 360)
                    if not row:
                        print(f'{stamp()} {spec["peer"]}: no listed session within 360 s', flush=True)
                        results[spec['peer']] = 1
                        continue
                    write_json(root / f'{spec["peer"]}-listed.json', row)
                    spec['flags'] = [row['session_id'] if flag == '{SESSION}' else flag for flag in spec['flags']]
                    if needs_menu_session:
                        menu.write_text(menu.read_text(encoding='utf-8').replace('{DIRECTORY_SESSION}', row['session_id']), encoding='utf-8')
                run = edith_cross.prepare_peer(h, spec)
                run.start()
                if spec.get('menu_script') and menu.is_file():
                    forget_menu(menu, root / spec['peer'] / 'stdout.log', run)
                if spec.get('tailscale_down'):
                    receipt.append(dict(tailscale(['status']), step='engine-started', peer=spec['peer']))
                print(f'{stamp()} {spec["peer"]} started (pid {run.record.get("pid")})', flush=True)
                running.append((spec, run))
            for spec, run in running:
                if spec.get('kill_at_tick') and wait_for_tick(root / f'{spec["peer"]}-live.jsonl', spec['kill_at_tick'], run):
                    run.terminate(137, f'scenario drop of the {spec["peer"]} at tick {spec["kill_at_tick"]}')
                run.finish()
                if spec.get('tailscale_down'):
                    receipt.append(dict(tailscale(['status']), step='engine-ended', peer=spec['peer']))
        finally:
            for spec, run in running:
                try:
                    run.close()
                finally:
                    write_json(root / f'{spec["peer"]}-record.json', run.record)
                    results[spec['peer']] = 0 if run.record.get('exit_code') == 0 else 1
                    print(f'{stamp()} {spec["peer"]} exit={run.record.get("exit_code")} timed_out={run.record.get("timed_out")} '
                          f'exe={str(run.record.get("exe_sha256"))[:16]}', flush=True)
            for spec, run in running:
                edith_cross.redact(h, run, spec)
    finally:
        try:
            clock.finish(root / f'{specs[0]["box"]}-clock.json')
        finally:
            if toggled:
                try:
                    receipt.append(dict(tailscale(['up']), step='up'))
                finally:
                    write_json(root / f'{specs[0]["box"]}-tailscale.json', receipt)
    try:
        h.records.compress_case_records(root)
    except Exception as error:
        print(f'{stamp()} records not compressed: {type(error).__name__}', flush=True)
    return 0 if all(value == 0 for value in results.values()) and len(results) == len(specs) else 1


def forget_menu(menu: Path, log: Path, run, budget_s: float = 90) -> None:
    """Overwrites a menu script once its engine has loaded it (a Fixed row's pair sits in it only that long)."""
    deadline = time.monotonic() + budget_s
    while time.monotonic() < deadline and run.poll() is None:
        try:
            if '[menu-script] loaded' in log.read_text(encoding='utf-8', errors='replace'):
                break
        except OSError:
            pass
        time.sleep(0.5)
    menu.write_text('# the menu script was removed once its engine had loaded it\n', encoding='utf-8')
    print(f'{stamp()} {menu.name} removed after the engine loaded it', flush=True)


def evidence_list(root: Path, out: Path) -> int:
    """The small files a run keeps (after its sweep): 8 MiB each at most; the sweep itself read everything."""
    from relay_secrets import walk
    files = [path.relative_to(root).as_posix() for path, reason in walk(root) if reason is None
             and not path.name.lower().endswith(('.exe', '.dll', '.pdb', '.png', '.mp4', '.tar')) and path.stat().st_size <= 8 << 20
             and 'Data' not in path.relative_to(root).parts[:-1]]
    out.write_text('\n'.join(files) + '\n', encoding='utf-8')
    print(json.dumps(dict(files=len(files))))
    return 0


def wait_for_tick(path: Path, tick: int, run, budget_s: float = 600) -> bool:
    """True once the peer's live hash file reaches the tick while its engine still runs."""
    deadline = time.monotonic() + budget_s
    while time.monotonic() < deadline and run.poll() is None:
        try:
            last = path.read_text(encoding='utf-8', errors='replace').rstrip().rsplit('\n', 1)[-1]
            if int(json.loads(last).get('tick', 0)) >= tick:
                return True
        except (OSError, ValueError, AttributeError):
            pass
        time.sleep(0.5)
    return False


# --- the scenario, the review and the table --------------------------------------------------------------------------

def load_scenario(name: str) -> dict:
    scenario = json.loads((SCENARIO_DIR / f'{name}.json').read_text(encoding='utf-8'))
    if scenario.get('driver') != 'tools/relay_cloudflare_match.py':
        raise SystemExit(f'{name}: not a relay scenario')
    return scenario


def resolve_run(scenario: dict, run: dict, placement: dict[str, str]) -> dict:
    resolved = dict(run)
    resolved['peers'] = [dict(peer, box=placement.get(peer['name'], peer['box'])) for peer in run['peers']]
    return resolved


def review(scenario: dict, verdicts: list[dict], out: Path) -> dict:
    """review.json, the plan's product: the scenario's checklist, each item judged from its run's verdict, then every check
    the run's row requires (REQUIRED) and the checklist does not name, as its own item. A run that did not pass fails every
    item of it, whatever the item reads; an absent check fails; a refused run fails all of its items."""
    by_run = {verdict['name']: verdict for verdict in verdicts}
    items = []
    for item in scenario['checklist']:
        verdict = by_run.get(item['run'])
        if verdict is None:
            items.append(dict(item, state='not-run'))
            continue
        if verdict.get('refused'):
            items.append(dict(item, state='FAIL', finding=dict(**{'class': 'harness'}, reason=verdict['refused'])))
            continue
        check = item['check']
        checks = verdict.get('checks') or {}
        if check.startswith('route:') or check.startswith('address:'):
            kind, peer = check.split(':', 1)
            entry = (verdict.get('relay_evidence') or {}).get('peers', {}).get(peer, {})
            reasons = [reason for reason in (verdict.get('relay_evidence') or {}).get('reasons', []) if reason.startswith(f'{peer}:')]
            state = 'PASS' if entry and not reasons else 'FAIL'
            evidence = dict(route=entry.get('route'), receipts=[row.get('line') for row in entry.get('accepted', [])][:2],
                            relay_addresses=entry.get('relay_addresses'), binding=entry.get('binding'), candidates=entry.get('candidates', [])[:6])
        elif check == 'offer':
            reasons = [reason for reason in (verdict.get('relay_evidence') or {}).get('reasons', []) if 'offer' in reason or 'report' in reason]
            state, evidence = ('PASS' if not reasons and checks.get('offer_fresh', False) else 'FAIL'), dict(session=verdict.get('session_id'), reasons=reasons)
        elif check == 'direct_expected':
            state = 'PASS' if checks.get('direct_expected') else 'FAIL'
            evidence = dict(routes=(verdict.get('relay_evidence') or {}).get('routes'))
        else:
            state = 'PASS' if checks.get(check) is True else 'FAIL'
            evidence = {check: checks.get(check, 'absent')}
            if check == 'rtt_recorded':
                evidence['rtt'] = verdict.get('rtt')
            if check == 'hashes_equal':
                evidence.update(compared=verdict.get('compared_ticks'), desyncs=verdict.get('desyncs'))
        if not verdict.get('passed'):
            state = 'FAIL' if state == 'PASS' and item['run'] in by_run else state
            evidence['run_failed'] = sorted(name for name, value in checks.items() if not value) or ['the run did not pass']
        items.append(dict(item, state=state, evidence=evidence))
    named = {(item['run'], item['check']) for item in scenario['checklist']}
    for verdict in verdicts:
        if verdict['name'] not in {run['name'] for run in scenario['runs']}:
            continue
        checks = verdict.get('checks') or {}
        for check in REQUIRED.get(verdict['name'], BASE):
            if (verdict['name'], check) in named:
                continue
            state = 'PASS' if not verdict.get('refused') and checks.get(check) is True else 'FAIL'
            items.append(dict(id=f'{verdict["name"]}-required-{check}', run=verdict['name'], check=check, required=True,
                              what=f'The row requires {check}.', state=state, evidence={check: checks.get(check, 'absent')}))
    passed = bool(items) and all(item['state'] == 'PASS' for item in items) and all(verdict.get('passed') for verdict in verdicts)
    document = dict(schema=1, scenario=scenario['name'], driver=scenario['driver'], verdict='PASS' if passed else 'FAIL', passed=passed,
                    status='PASS' if passed else 'FAIL', checklist=items, runs=[dict(name=v['name'], root=v.get('root'), passed=v.get('passed'),
                    refused=v.get('refused')) for v in verdicts], counts=dict(executed=sum(item['state'] != 'not-run' for item in items),
                    failed=sum(item['state'] == 'FAIL' for item in items)), finished=stamp())
    write_json(out / 'review.json', document)
    return document


def exit_status(document: dict, verdicts: list[dict], final_scan: dict | None = None) -> int:
    """0 only when the review passed, every run passed and was not refused, and the final sweep is clean."""
    ok = document.get('passed') is True and verdicts and all(verdict.get('passed') is True and not verdict.get('refused') for verdict in verdicts)
    if final_scan is not None:
        ok = ok and final_scan.get('status') == 'CLEAN'
    return 0 if ok else 1


def table(roots: list[Path]) -> dict:
    rows = []
    for root in roots:
        verdict = read_json(Path(root) / 'relay-verdict.json')
        rows.append(dict(run=verdict.get('name'), relay=verdict.get('relay'), passed=verdict.get('passed'),
                         routes=(verdict.get('relay_evidence') or {}).get('routes'),
                         rtt_ms=[row['rtt_ms'] for row in verdict.get('rtt', [])],
                         input_delay_frames=[row['delay_frames'] for row in verdict.get('rtt', [])],
                         peers={name: {key: value.get(key) for key in ('box', 'wall_tps', 'longest_steady_wait_ms', 'waiting_percent', 'holds', 'last_tick')}
                                for name, value in (verdict.get('peers') or {}).items()},
                         desyncs=verdict.get('desyncs'), stun_legs=verdict.get('stun_legs')))
    return dict(schema=1, made=stamp(), rows=rows)


def main(argv=None) -> int:
    global DRY_RUN, REDACTOR, LANE
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--scenario')
    parser.add_argument('--run', action='append', default=[])
    parser.add_argument('--out', type=Path)
    parser.add_argument('--box', action='append', default=[], metavar='PEER=BOX', help='place a peer on another box than the scenario names')
    parser.add_argument('--tree', action='append', default=[], metavar='BOX=PATH', help="a box's engine tree instead of boxes.json's")
    parser.add_argument('--alias', action='append', default=[], metavar='BOX=SSH', help="reach a box through another ssh alias (a laptop's tailnet name)")
    parser.add_argument('--ticks', type=int, help="every run's frames instead of each row's own (the rows' declared ticks otherwise)")
    parser.add_argument('--no-replay', action='store_true',
                        help="the host records no replay (no -net-replay-out, NetworkRecordReplays off): an engine that persists the relay "
                             "login in its replay is told apart from every other file it writes")
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--table', type=Path, nargs='+')
    parser.add_argument('--remote-peers')
    parser.add_argument('--evidence-list', type=Path)
    parser.add_argument('--bridge-edith', nargs=2, metavar=('IP:UDP', 'TCP'))
    options = parser.parse_args(argv)
    if options.remote_peers:
        return remote_peers(options.remote_peers)
    if options.evidence_list:
        return evidence_list(options.evidence_list, options.out)
    if options.bridge_edith:
        return bridge_edith(options.bridge_edith[0], int(options.bridge_edith[1]))
    if options.table:
        if not options.out:
            parser.error('--table needs --out')
        result = table(options.table)
        write_json(options.out, result)
        print(json.dumps(result, indent=1))
        return 0
    if not options.scenario or not options.out:
        parser.error('--scenario and --out are required')
    if os.environ.get('CCCP_HEADLESS', '1') != '1':
        parser.error('CCCP_HEADLESS must stay 1')
    DRY_RUN = options.dry_run
    scenario = load_scenario(options.scenario)
    placement = dict(pair.split('=', 1) for pair in options.box)
    trees = dict(pair.split('=', 1) for pair in options.tree)
    aliases = dict(scenario.get('aliases') or {}, **dict(pair.split('=', 1) for pair in options.alias))
    boxes = load_boxes(trees, aliases)
    from relay_secrets import RETIRED_USERNAME_SCOPE, SecretBook
    book = REDACTOR = SecretBook()
    if RETIRED_FIXED_CONF.is_file():
        book.add_fixed_login(RETIRED_FIXED_CONF, RETIRED_USERNAME_SCOPE)  # its leak is still found; no run uses it
    out = options.out.resolve()
    LANE = lane_for(out)
    if not DRY_RUN:
        out.mkdir(parents=True, exist_ok=False)
    verdicts = []
    for run in scenario['runs']:
        if options.run and run['name'] not in options.run:
            continue
        resolved = resolve_run(scenario, run, placement)
        if options.no_replay:
            resolved['record_replay'] = False
        ticks = options.ticks or int(run.get('ticks') or scenario.get('ticks') or MATCH_TICKS)
        try:
            verdicts.append(run_one(scenario, resolved, out, boxes, ticks, book))
        except Exception as error:
            say(f'{run["name"]}: {type(error).__name__}: {error}')
            verdicts.append(dict(name=run['name'], root=str(out / run['name']), passed=False, refused=f'{type(error).__name__}: {error}'))
    if DRY_RUN:
        print(redacted(json.dumps(verdicts, indent=1)))
        return 0
    identities = out / 'identities'
    identities.mkdir(exist_ok=True)
    for verdict in verdicts:
        for name, value in (verdict.get('identities') or read_json(Path(verdict.get('root', out)) / 'identities.json') or {}).items():
            write_json(identities / f'{name.upper()}.json', value)
    selected = dict(scenario, runs=[run for run in scenario['runs'] if not options.run or run['name'] in options.run],
                    checklist=[item for item in scenario['checklist'] if not options.run or item['run'] in options.run])
    document = review(selected, verdicts, out)
    from relay_secrets import sweep
    scan = sweep([out], book.finder())
    write_json(out / 'secret-scan.json', scan)
    status = exit_status(document, verdicts, scan)
    say(f'{scenario["name"]}: {document["verdict"]} {document["counts"]} final sweep {scan["status"]} ({scan["files_scanned"]} files) exit={status}')
    return status


if __name__ == '__main__':
    raise SystemExit(main())
