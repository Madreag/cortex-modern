"""The relay proved on real boxes: one lockstep match whose route the scenario forces (Cloudflare through the directory,
our own coturn as the fixed pair, or Automatic), judged by the transport's own receipts, never a label.

    python tools/relay_cloudflare_match.py --scenario mp-relay-cloudflare --out D:/mx/<lane>/<dir> [--run NAME ...]
            [--box host=edith --box client=edith] [--tree edith=D:/Projects/inventory-build] [--dry-run]
    python tools/relay_cloudflare_match.py --table <run dir> [<run dir> ...] --out <table.json>
    python tools/relay_cloudflare_match.py --remote-peers <spec.json>        (on a box, inside its session task)

This box (the driver's) launches no engine. It runs the session directory in this process on a loopback port (TLS,
a throwaway certificate both engines pin, the Cloudflare key file read by path) and opens an `ssh -R` from each game
box's loopback to it; the directory only carries the rendezvous, the match's packets take the route ICE selects. Each
box runs its peers through its own session task (tools/edith/remote_box.py's payload) and the private-desktop runner.

The evidence per peer: the same-session `[net-ice] selected candidate=` / `[net-route] RouteAllowed` lines; the ICE
candidates the peer itself sent through the directory (a Relay only peer sends relay candidates only, each at the
provider's address); the directory's relay_offer_issued receipt for the session; the client's match report naming a
relayed connection; equal live hashes; the feel driver's gates; the transport's RTT. The key, the minted logins and a
fixed relay password stay in memory; the scan proves no file of the run holds one.
"""
from __future__ import annotations

import argparse
import base64
import datetime as dt
import hashlib
import ipaddress
import json
import os
import re
import socket
import ssl
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
LANE = os.environ.get('CC_RELAY_LANE', 'opus-relay-cloudflare-20261003')
SCENARIO_DIR = HERE / 'e2e'
BOXES_JSON = Path('D:/Projects/reviews/takeover-20260909/grok-workers/lead-tools/inventory/boxes.json')
CLOUDFLARE_TURN_CONFIG = Path('D:/mx/coturn-20260920/turn-config-cloudflare.json')
COTURN_CONF = Path('D:/mx/coturn-20260920/turnserver-fixed.conf')
# Our relay (the Mac's coturn, BOXES.md): its LAN address from home, the home router's public address from elsewhere;
# coturn reports the relayed address on its own interface, so a relay through it is at the LAN address.
COTURN_URLS = ['turn:68.3.162.151:3479?transport=udp', 'turn:192.168.50.122:3479?transport=udp']
COTURN_ADDRESSES = ['192.168.50.122', '68.3.162.151']
# https://www.cloudflare.com/ips-v4 and /ips-v6, read 2026-10-03; turn.cloudflare.com resolved to 141.101.90.1 that day.
CLOUDFLARE_RANGES = [ipaddress.ip_network(text) for text in (
    '173.245.48.0/20', '103.21.244.0/22', '103.22.200.0/22', '103.31.4.0/22', '141.101.64.0/18', '108.162.192.0/18',
    '190.93.240.0/20', '188.114.96.0/20', '197.234.240.0/22', '198.41.128.0/17', '162.158.0.0/15', '104.16.0.0/13',
    '104.24.0.0/14', '172.64.0.0/13', '131.0.72.0/22', '2400:cb00::/32', '2606:4700::/32', '2803:f800::/32',
    '2405:b500::/32', '2405:8100::/32', '2a06:98c0::/29', '2c0f:f248::/32')]
CLOUDFLARE_HOSTS = {'turn.cloudflare.com', 'stun.cloudflare.com'}
SECRET_SETTINGS = ('NetworkTurnPass', 'NetworkPlayerTurnPass')
# Ports other drivers own (CLAUDE.md, the e2e README, edith_cross): never chosen here.
FOREIGN_PORTS = [(48320, 48539), (48630, 48649), (49180, 49199), (49400, 49479), (49860, 49879), (49985, 49986)]
PORT_WINDOW = (48700, 49170)
MATCH_TICKS = 1201
DRY_RUN = False


def stamp() -> str:
    return dt.datetime.now(MST).strftime('%Y-%m-%d %H:%M:%S MST')


def say(message: str) -> None:
    print(f'[relay-match] {message}', flush=True)


def write_json(path, value) -> None:
    Path(path).write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def read_json(path) -> dict:
    path = Path(path)
    try:
        return json.loads(path.read_text(encoding='utf-8-sig')) if path.is_file() else {}
    except ValueError:
        return {}


# --- the evidence (pure; tools/relay_cloudflare_test.py drives every rule) ------------------------------------------

CANDIDATE = re.compile(rb'candidate:\S+ \d+ (?:udp|UDP) \d+ (\S+) (\d+) typ (host|srflx|prflx|relay)')


def cloudflare_address(text: str) -> bool:
    try:
        address = ipaddress.ip_address(str(text).strip('[]'))
    except ValueError:
        return False
    return any(address.version == network.version and address in network for network in CLOUDFLARE_RANGES)


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


def route_receipts(log: str, session: str | None) -> dict:
    """The route lines a peer wrote for this session: the accepted ones and every other allowed route."""
    current, selected, accepted, other_allowed, refused = None, {}, [], [], []
    for line in log.splitlines():
        if found := re.search(r'\[net-ice\] (?:host )?session (\S+)', line):
            current, selected = found[1], {}
        if found := re.search(r'\[net-ice\] selected candidate=(\S+) connection=(\d+)', line):
            if session and current == session:
                selected[found[2]] = found[1]
        if found := re.search(r'\[net-route\] RouteAllowed route=(\w+) allowed=(\d+) connection=(\d+)', line):
            if not session or current != session:
                continue
            row = dict(route=found[1], connection=found[3], candidate=selected.get(found[3]), line=line.strip())
            if found[2] != '1':
                refused.append(row)
            elif row['candidate'] is not None and (row['route'] == 'relay') == (row['candidate'] == 'relay'):
                accepted.append(row)
            else:
                other_allowed.append(row)
    return dict(accepted=accepted, other_allowed=other_allowed, refused=refused)


def candidates_by_peer(signals) -> dict[str, list[tuple[str, int, str]]]:
    """Every candidate each side sent through the directory: 'host' is the host, any joiner id the client."""
    found: dict[str, list] = {}
    for sender, payload in signals:
        found.setdefault('host' if sender == 'host' else 'client', []).extend(signal_candidates(payload))
    return found


def judge_relay(run: dict) -> dict:
    """mode cloudflare: every Relay only peer relayed at a Cloudflare address on a Cloudflare offer for this session;
    mode coturn: the same at our relay's addresses on a fixed offer; mode automatic: every peer's route recorded."""
    mode, session = run['mode'], run.get('session_id')
    reasons, peers, routes = [], {}, {}
    provider = {'cloudflare': 'cloudflare', 'coturn': 'fixed'}.get(mode)
    relay_ok = cloudflare_address if mode == 'cloudflare' else (lambda address: address in (run.get('relay_addresses') or []))
    sent = candidates_by_peer(run.get('signals') or [])
    for peer, connection in sorted(run['connection'].items()):
        receipts = route_receipts(run['logs'].get(peer, ''), session)
        chosen = receipts['accepted'][-1]['route'] if receipts['accepted'] else None
        routes[peer] = chosen
        own = sent.get(peer, [])
        entry = dict(connection=connection, route=chosen, accepted=receipts['accepted'], other_allowed=receipts['other_allowed'],
                     refused=receipts['refused'], candidates=[f'{address}:{port} {kind}' for address, port, kind in own])
        if not receipts['accepted']:
            reasons.append(f'{peer}: no accepted route receipt for session {session!r}')
        if connection == 'RelayOnly':
            if chosen != 'relay' or receipts['other_allowed'] or any(row['route'] != 'relay' for row in receipts['accepted']):
                reasons.append(f'{peer}: Relay only but routes {[row["route"] for row in receipts["accepted"] + receipts["other_allowed"]]}')
            relays = [(address, port) for address, port, kind in own if kind == 'relay']
            if not relays:
                reasons.append(f'{peer}: sent no relay candidate through the directory')
            if any(kind != 'relay' for _, _, kind in own):
                reasons.append(f'{peer}: Relay only but sent {sorted({kind for _, _, kind in own if kind != "relay"})} candidates')
            outside = sorted({address for address, _ in relays if not relay_ok(address)})
            if outside:
                reasons.append(f'{peer}: relay candidates outside the {mode} relay: {outside}')
            entry['relay_addresses'] = sorted({address for address, _ in relays})
        peers[peer] = entry
    if provider:
        offers = [row for row in run.get('offers') or [] if session and row.get('session_id') == session and row.get('provider') == provider
                  and all(type(row.get(key)) is int and row[key] > 0 for key in ('generation', 'expires_at', 'server_count'))]
        if not offers:
            reasons.append(f'no {provider} relay offer issued for session {session!r}')
        hosts = sorted({url_host(url) for url in run.get('offer_urls') or []})
        allowed = CLOUDFLARE_HOSTS if mode == 'cloudflare' else set(run.get('relay_addresses') or [])
        if not hosts or set(hosts) - allowed:
            reasons.append(f'the offer names relay servers {hosts}, expected only {sorted(allowed)}')
        report = run.get('client_connection') or {}
        if report.get('relayed') is not True:
            reasons.append(f'the client report does not name a relayed connection: {report}')
        elif report.get('remote_address') and not relay_ok(host_of(report['remote_address'])):
            reasons.append(f'the client report names {report["remote_address"]}, outside the {mode} relay')
    return dict(mode=mode, passed=not reasons, reasons=reasons, peers=peers, routes=routes,
                direct_as_expected=all(route == 'direct' for route in routes.values()) if mode == 'automatic' else None)


def transport_rtts(log: str) -> list[dict]:
    return [dict(peer=int(peer), rtt_ms=int(rtt), delay_frames=int(frames)) for peer, rtt, frames in
            re.findall(r'\[net-match\] auto input delay: peer (\d+) rtt (\d+)ms -> (\d+) frames', log)]


def delay_changes(log: str) -> list[dict]:
    return [dict(peer=int(peer), frame=int(frame), delay=int(delay)) for peer, frame, delay in
            re.findall(r'\[net-match\] delay change peer=(\d+) frame=(\d+) delay=(\d+)', log)]


# --- the boxes -------------------------------------------------------------------------------------------------------

class Box:
    def __init__(self, name: str, entry: dict, tree: str | None = None) -> None:
        self.name = name
        self.alias = entry['ssh']
        self.task = entry.get('task', 'cortex-session1')
        self.session_script = entry.get('session_script', 'D:/mx/session1/run.ps1')
        self.tree = tree or entry['repo']
        self.path_prepend = list(entry.get('path_prepend') or [])
        self.max_engines = int((entry.get('memory') or {}).get('max_engines') or (entry.get('runner') or {}).get('max_engines') or 2)
        self.computer = entry.get('computer_name') or entry.get('hostname') or name
        from remote_box import RemoteBox
        self.remote = RemoteBox(self.alias, self.task, self.session_script, dry_run=DRY_RUN, say=say)


def load_boxes(trees: dict[str, str]) -> dict[str, Box]:
    entries = {entry['name'].lower(): entry for entry in json.loads(BOXES_JSON.read_text(encoding='utf-8'))['boxes']
               if entry.get('kind') == 'windows-task'}
    return {name: Box(name, entry, trees.get(name)) for name, entry in entries.items()}


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
        with socket.socket() as sock:
            try:
                sock.bind(('127.0.0.1', block[0]))
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
                status='INCOMPLETE' if errors else 'PASS', errors=errors, measured=dt.datetime.now(MST).strftime('%Y-%m-%d %I:%M:%S %p MST'))


# --- the directory on this box ---------------------------------------------------------------------------------------

class Directory:
    """The session directory in this process, observed: the candidates each side sends, the logins it mints, Cloudflare's
    statuses. Nothing it observes is written except candidates and statuses."""

    def __init__(self, root: Path, port: int, backend: dict | None, ttl_cap: int, book) -> None:
        from session_directory import session_directory as module
        from edith_cross import make_cert
        self.module, self.book = module, book
        self.signals, self.provider_calls, self.offers = [], [], []
        self.cert, key, self.pin = make_cert(root)
        real = module.urlopen

        def urlopen(request, *args, **kwargs):
            try:
                response = real(request, *args, **kwargs)
            except HTTPError as error:
                code = re.search(r'error code:\s*(\d+)', error.read(128).decode('ascii', 'replace'))
                self.provider_calls.append(dict(at=stamp(), status=error.code, provider_error_code=code[1] if code else None))
                raise
            self.provider_calls.append(dict(at=stamp(), status=response.status))
            return response
        module.urlopen = urlopen
        self.server = module.spawn_server(port=port, cert=self.cert, key=key, insecure_http=False, log_file=root / 'service.log',
                                          turn_config=backend, turn_max_ttl=ttl_cap)
        key.unlink()
        store = self.server.store
        mint, post, register, mint_offer = store.turn_provider.mint, store.post_signal, store.register, store.mint_ice_servers

        def minted(*args, **kwargs):
            offer = mint(*args, **kwargs)
            book.add_offer(offer)
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
            offer = mint_offer(session_id, data, install_key, now)
            self.offers.append(dict(session_id=session_id, urls=[url for server in offer.get('iceServers', []) for url in server.get('urls', [])]))
            return offer
        store.turn_provider.mint, store.post_signal, store.register, store.mint_ice_servers = minted, posted, registered, published

    def sessions(self) -> list[str]:
        with self.server.store._lock:
            return list(self.server.store._sessions)

    def stop(self) -> None:
        self.server.stop()


class Tunnel:
    """ssh -R: the game box's 127.0.0.1:<port> reaches the directory on this box's loopback (the rendezvous only)."""

    def __init__(self, box: Box, port: int, log_path: Path) -> None:
        self.box, self.port, self.log_path, self.process = box, port, log_path, None

    def open(self) -> None:
        argv = ['ssh', '-N', '-o', 'ExitOnForwardFailure=yes', '-o', 'ServerAliveInterval=15', '-R',
                f'127.0.0.1:{self.port}:127.0.0.1:{self.port}', self.box.alias]
        if DRY_RUN:
            say('dry-run: ' + ' '.join(argv))
            return
        self.process = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=self.log_path.open('w'), stderr=subprocess.STDOUT,
                                        creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        for _ in range(10):
            time.sleep(2)
            probe = self.box.remote.ssh(f"$c = New-Object Net.Sockets.TcpClient; try {{ $c.Connect('127.0.0.1', {self.port}); 'open' }} "
                                        f"catch {{ 'closed' }} finally {{ $c.Close() }}").strip()
            if probe == 'open':
                return
            if self.process.poll() is not None:
                break
        raise RuntimeError(f'the ssh -R tunnel did not open on {self.box.name} (port {self.port}); see {self.log_path}')

    def close(self) -> None:
        if self.process and self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=10)


# --- one run ---------------------------------------------------------------------------------------------------------

def peer_settings(run: dict, peer: dict, port: int, pin: str, login: tuple[str, str] | None) -> dict:
    """What the player chose for this match, seeded before the engine starts: the directory, the connection, the relay."""
    settings = {'SessionDirectoryUrl': f'127.0.0.1:{port}', 'SessionDirectoryCertSha256': pin,
                'SessionDirectoryInstallKey': f'relay-proof-{peer["box"]}-{peer["name"]}'[:32], 'NetworkIceEnable': '1',
                'NetworkConnectionMode': peer['connection'], 'NetworkShowDiagnostics': '1'}
    if peer['name'] == 'host':
        settings['NetworkHostRelayMode'] = run['host_relay_mode']
        if run['host_relay_mode'] == 'Fixed':
            user, secret = login
            settings.update(NetworkTurnServers=','.join(run.get('relay_urls') or COTURN_URLS), NetworkTurnUser=user, NetworkTurnPass=secret)
    return settings


def build_specs(h, run: dict, root: Path, ports: dict, boxes: dict[str, Box], pin: str, login, ticks: int) -> list[dict]:
    from edith_cross import match_spec
    specs = []
    humans = str(len(run['peers']))
    for peer in run['peers']:
        box = boxes[peer['box']]
        role = (['-net-host', '-net-replay-out', str(root / 'match.ccreplay'), '-net-ice', 'on'] if peer['name'] == 'host'
                else ['-net-join-session', '{SESSION}', '-net-ice', 'on'])
        spec = match_spec(peer['name'], root, ports[peer['name']], role, peer_settings(run, peer, ports['directory'], pin, login),
                          repo=box.tree, ticks=ticks, timeout=run.get('timeout_s', 420))
        flags = spec['flags']
        for name in ('-net-match-humans', '-net-match-peers'):
            flags[flags.index(name) + 1] = humans
        if peer['name'] == 'host' and run.get('kill_host_at_tick'):
            spec['kill_at_tick'] = int(run['kill_host_at_tick'])
        spec.update(box=peer['box'], tailscale_down=bool(peer.get('tailscale_down')))
        specs.append(spec)
    return specs


def redacted(spec: dict) -> dict:
    return dict(spec, settings={key: ('redacted' if key in SECRET_SETTINGS else value) for key, value in spec['settings'].items()})


def ship(box: Box, root: Path, specs: list[dict], directory_port: int, payload: Path) -> Path:
    """The box's spec (its peers in start order) and the drivers; returns the local copy of the rendered task payload."""
    spec_path = root / f'{box.name}-peers.json'
    document = dict(root=str(root), lane=LANE, directory=dict(port=directory_port, cert=str(root / 'cert.pem')), peers=specs)
    if DRY_RUN:
        say(f'dry-run: {box.name} runs {[spec["peer"] for spec in specs]} from {box.tree}: '
            + json.dumps([redacted(spec)['settings'] for spec in specs]))
        return root / f'{box.name}-run.ps1'
    box.remote.mkdir(root)
    box.remote.mkdir(payload / 'edith')
    for local, remote in ((HERE / 'relay_cloudflare_match.py', payload / 'relay_cloudflare_match.py'),
                          (HERE / 'relay_secrets.py', payload / 'relay_secrets.py'), (HERE / 'edith_cross.py', payload / 'edith_cross.py'),
                          (HERE / 'edith/remote_box.py', payload / 'edith/remote_box.py'), (root / 'cert.pem', root / 'cert.pem'),
                          (root / 'input.txt', root / 'input.txt'), (root / 'input-schedule.json', root / 'input-schedule.json')):
        if Path(local).is_file():
            box.remote.scp_to(local, remote)
    write_json(spec_path, document)
    box.remote.scp_to(spec_path, spec_path)
    write_json(spec_path, dict(document, peers=[redacted(spec) for spec in specs]))
    from remote_box import render_payload
    script = render_payload(box.tree, [str(payload / 'relay_cloudflare_match.py'), '--remote-peers', str(spec_path)],
                            root / f'{box.name}-payload.log', root / f'{box.name}-payload.done',
                            {'CC_RELAY_LANE': LANE, 'CC_EDITH_CROSS_LANE': LANE}, box.path_prepend)
    local = root / f'{box.name}-run.ps1'
    local.write_text(script, encoding='utf-8')
    return local


def stun_legs(box: Box, targets: list[str]) -> list[str]:
    """Each box's own STUN binding round trip to the relay servers: what one leg to the relay costs from there."""
    if DRY_RUN:
        return []
    code = (HERE / 'relay_stun_probe.py').read_text(encoding='utf-8')
    encoded = base64.b64encode(code.encode()).decode()
    args = ', '.join(repr(target) for target in targets)
    text = box.remote.ssh(f"python -c \"import base64,sys;sys.argv=['probe', {args}];exec(base64.b64decode('{encoded}'))\"", timeout=60)
    return [line.strip() for line in text.splitlines() if line.strip()]


def run_one(scenario: dict, run: dict, out: Path, boxes: dict[str, Box], ticks: int, book) -> dict:
    import edith_cross
    from relay_secrets import read_fixed_login, read_turn_config
    root = (out / run['name']).resolve()
    used = [boxes[name] for name in dict.fromkeys(peer['box'] for peer in run['peers'])]
    for box in used:
        engines = sum(peer['box'] == box.name for peer in run['peers'])
        if engines > box.max_engines:
            raise RuntimeError(f'{box.name} holds {box.max_engines} engine(s); the run asks for {engines}')
    say(f'{run["name"]}: {[(peer["name"], peer["box"], peer["connection"]) for peer in run["peers"]]} relay={run["relay"]} {stamp()}')
    if not DRY_RUN:
        root.mkdir(parents=True, exist_ok=False)
    identities = {box.name: identity(box) for box in used}
    heads = {value['head'] for value in identities.values()}
    exes = {value['executable_sha256'] for value in identities.values()}
    refusal = None
    if not DRY_RUN and (any(value['status'] != 'PASS' for value in identities.values()) or len(heads) != 1 or len(exes) != 1):
        refusal = f'the boxes do not run one receipted build: {[(name, value["head"][:10], str(value["executable_sha256"])[:12], value["errors"]) for name, value in identities.items()]}'
    if not DRY_RUN:
        write_json(root / 'identities.json', identities)
    if refusal:
        say(f'{run["name"]}: REFUSED {refusal}')
        return dict(name=run['name'], root=str(root), passed=False, refused=refusal, identities=identities)
    h = edith_cross.harness(HERE)
    backend = read_turn_config(CLOUDFLARE_TURN_CONFIG) if run['host_relay_mode'] == 'Directory' else None
    if backend:
        book.add_turn_config(backend)
    login = read_fixed_login(COTURN_CONF) if run['host_relay_mode'] == 'Fixed' else None
    if login:
        book.add('fixed-username', login[0])
        book.add('fixed-password', login[1])
    names = [peer['name'] for peer in run['peers']]
    block = choose_ports(used, len(names) + 1)
    ports = dict(directory=block[0], **{name: block[index + 1] for index, name in enumerate(names)})
    if not DRY_RUN:
        edith_cross.looped_input(h, root / 'input.txt', ticks)
    legs = {box.name: stun_legs(box, ['turn.cloudflare.com:3478', COTURN_URLS[0].split(':', 1)[1].split('?')[0]]) for box in used}
    directory = tunnels = None
    started = stamp()
    states, payload = {}, Path('D:/mx') / LANE / 'payload'
    try:
        if not DRY_RUN:
            directory = Directory(root, ports['directory'], backend, int(run.get('relay_ttl_cap', 86400)), book)
        pin = directory.pin if directory else '<pin>'
        tunnels = [Tunnel(box, ports['directory'], root / f'tunnel-{box.name}.log') for box in used]
        for tunnel in tunnels:
            tunnel.open()
        specs = build_specs(h, run, root, ports, boxes, pin, login, ticks)
        scripts = {box.name: ship(box, root, [spec for spec in specs if spec['box'] == box.name], ports['directory'], payload) for box in used}
        for box in used:  # the host's box first; a joiner's own payload waits for the host's listing
            if not DRY_RUN:
                box.remote.start_task(scripts[box.name], budget_s=1800)
            say(f'{run["name"]}: payload started on {box.name} through {box.task}')
        for box in used:
            states[box.name] = box.remote.wait_done(root / f'{box.name}-payload.done', run.get('timeout_s', 420) + 900)
            say(f'{run["name"]}: {box.name} {states[box.name].strip()}')
    finally:
        for tunnel in tunnels or []:
            tunnel.close()
        if directory:
            directory.stop()
    if DRY_RUN:
        return dict(name=run['name'], dry_run=True, ports=ports)
    for box in used:
        listing = root / f'{box.name}-evidence.txt'
        if box.remote.read_text(listing) is not None:
            box.remote.fetch_list(root, listing, root, f'{box.name}-evidence.tar')
    sessions = directory.sessions() if directory else []
    return judge_run(h, scenario, run, root, dict(started=started, finished=stamp(), states=states, identities=identities, legs=legs,
                     ports=ports, sessions=sessions, signals=directory.signals, offers_seen=directory.offers,
                     provider_calls=directory.provider_calls, ticks=ticks), book)


def judge_run(h, scenario: dict, run: dict, root: Path, facts: dict, book) -> dict:
    import edith_cross
    names = [peer['name'] for peer in run['peers']]
    ticks = int(facts['ticks'])
    records = {name: read_json(root / f'{name}-record.json') for name in names}
    logs = {name: edith_cross.peer_log(root, name) for name in names}
    host_log = logs.get('host', '')
    session = next(iter(re.findall(r'\[net-ice\] host session (\S+)', host_log)), None) or (facts['sessions'][0] if facts['sessions'] else None)
    # A peer the run drops on purpose ends with the runner's injected exit; every other peer ends clean with its evidence.
    dropped = {peer['name'] for peer in run['peers'] if peer['name'] == 'host' and run.get('kill_host_at_tick')}
    complete = all((str(row.get('injected_termination', '')).startswith('scenario drop') if name in dropped else
                    row.get('exit_code') == 0 and row.get('evidence_complete')) and not row.get('timed_out') for name, row in records.items())
    hash_peers = [name for name in names if name not in dropped]
    manifest = dict(name=run['name'], mode=f'relay proof {run["relay"]}', ticks=ticks, lag_ms=0, cap_hz=60, instrumentation=False,
                    loss_percent=0, silent_tick=None, live_stalls=None, autosave_seconds=None,
                    per_peer_lag_ms={name: 0 for name in names}, launches_complete=complete,
                    exe={name: row.get('exe_sha256') for name, row in records.items()})
    write_json(root / 'manifest.json', manifest)
    h.records.compress_case_records(root)
    try:
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
    peers = {}
    for name in names:
        metrics = (timing.get('peers', {}).get(name) or {}).get('metrics', {})
        log = logs[name]
        try:
            seen = edith_cross.live_ticks(root / f'{name}-live.jsonl')
        except (ValueError, OSError):
            seen = set()
        peers[name] = dict(box=next(peer['box'] for peer in run['peers'] if peer['name'] == name), exit_code=records[name].get('exit_code'),
                           timed_out=records[name].get('timed_out'), exe=str(records[name].get('exe_sha256'))[:16], live_ticks=len(seen),
                           last_tick=max(seen, default=None), holds=len(re.findall(r'\[net-match\] hold peer=\d+ frame=\d+', log)),
                           wall_tps=metrics.get('steady_wall_tps'), longest_steady_wait_ms=metrics.get('longest_stall_ms'),
                           waiting_percent=(100 * metrics['net_wait_ms'] / metrics['steady_wall_ms']) if metrics.get('steady_wall_ms') else None,
                           input_delays=metrics.get('peer_input_delays'),
                           timing_pass=(timing.get('peers', {}).get(name) or {}).get('pass_check'))
    report = read_json(root / 'client_report.json')
    connection = edith_cross.find_key(report, 'connection') if report else None
    offers = [json.loads(line.split('relay_offer_issued ', 1)[1]) for line in
              ((root / 'service.log').read_text(encoding='utf-8', errors='replace').splitlines() if (root / 'service.log').is_file() else [])
              if 'relay_offer_issued ' in line]
    offer_urls = next((row['urls'] for row in facts['offers_seen'] if row['session_id'] == session), [])
    relay = judge_relay(dict(mode=run['relay'], session_id=session, connection={peer['name']: peer['connection'] for peer in run['peers']},
                             logs=logs, signals=facts['signals'], offers=offers, offer_urls=offer_urls,
                             client_connection=connection if isinstance(connection, dict) else {},
                             relay_addresses=COTURN_ADDRESSES if run['relay'] == 'coturn' else None))
    builds = edith_cross.pair_build_evidence(root, dict(source_sha=next(iter({value['head'] for value in facts['identities'].values()}))), records)
    rtts = transport_rtts(host_log)
    scan = book.scan([root])
    write_json(root / 'secret-scan.json', scan)
    judged = run.get('timing_peers') or names
    checks = {
        'identities': all(value['status'] == 'PASS' for value in facts['identities'].values()),
        'builds': builds['passed'],
        'exits': complete,
        'full_history': all(peers[name]['live_ticks'] >= ticks - 1 for name in hash_peers),
        'hashes_equal': bool(live) and mismatched == 0 and all(value >= ticks - 1 - int(run.get('kill_host_at_tick') or 0) for value in compared.values())
                        and trace_pass is not False,
        'holds': all(peers[name]['holds'] == 0 for name in judged),
        'feel_gates': all(peers[name]['timing_pass'] is True for name in judged),
        'relay': relay['passed'],
        'rtt_recorded': bool(rtts),
        'no_secret_in_files': scan['clean'],
    }
    if run['relay'] in ('cloudflare',) and facts['provider_calls']:
        checks['provider_201'] = all(call['status'] == 201 for call in facts['provider_calls'])
    verdict = dict(name=run['name'], scenario=scenario['name'], relay=run['relay'], root=str(root), passed=all(checks.values()), checks=checks,
                   session_id=session, peers=peers, relay_evidence=relay, rtt=rtts, delay_changes=delay_changes(host_log),
                   compared_ticks=compared, desyncs=mismatched, builds=builds, provider_calls=facts['provider_calls'],
                   stun_legs=facts['legs'], ports=facts['ports'], states=facts['states'], started=facts['started'], finished=facts['finished'],
                   secret_scan=dict(clean=scan['clean'], secrets=scan['secrets'], kinds=scan['kinds'], files_scanned=scan['files_scanned'],
                                    files_with_secrets=scan['files_with_secrets']))
    write_json(root / 'relay-verdict.json', verdict)
    cell = lambda key, fmt='{}': '/'.join('-' if peers[name][key] is None else fmt.format(peers[name][key]) for name in names)
    say(f'RUN {run["name"]} {"PASS" if verdict["passed"] else "FAIL"} {json.dumps({k: v for k, v in checks.items() if not v})} '
        f'routes={relay["routes"]} rtt={[row["rtt_ms"] for row in rtts]} ticks={cell("live_ticks")} desyncs={mismatched} '
        f'holds={cell("holds")} tps={cell("wall_tps", "{:.2f}")} longest_ms={cell("longest_steady_wait_ms", "{:.0f}")} '
        f'waiting%={cell("waiting_percent", "{:.2f}")} relay_reasons={relay["reasons"][:3]}')
    return verdict


# --- on the box ------------------------------------------------------------------------------------------------------

def directory_session(port: int, cert: str, budget_s: float) -> str | None:
    context = ssl.create_default_context(cafile=cert)
    deadline = time.monotonic() + budget_s
    while time.monotonic() < deadline:
        try:
            request = urllib.request.Request(f'https://127.0.0.1:{port}/v1/sessions', headers={'X-Install-Key': 'relay-proof-session-poll'})
            with urllib.request.urlopen(request, context=context, timeout=5) as reply:
                rows = json.load(reply).get('sessions') or []
            if rows:
                return rows[0]['session_id']
        except (OSError, ValueError):
            pass
        time.sleep(1)
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


def remote_peers(spec_path: str) -> int:
    """Runs on a game box inside its session task: its peers through the runner, a joiner after the host is listed."""
    import edith_cross
    document = json.loads(Path(spec_path).read_text(encoding='utf-8'))
    root = Path(document['root'])
    specs = document['peers']
    h = edith_cross.harness(Path(specs[0]['repo']) / 'tools')
    receipt = []
    if any(spec.get('tailscale_down') for spec in specs):
        receipt.append(dict(tailscale(['status']), step='before'))
        receipt.append(dict(tailscale(['down']), step='down'))
    running, results = [], {}
    try:
        for spec in specs:
            if '{SESSION}' in spec['flags']:
                session = directory_session(document['directory']['port'], document['directory']['cert'], 180)
                if not session:
                    print(f'{stamp()} {spec["peer"]}: no listed session within 180 s', flush=True)
                    results[spec['peer']] = 1
                    continue
                spec['flags'] = [session if flag == '{SESSION}' else flag for flag in spec['flags']]
            run = edith_cross.prepare_peer(h, spec)
            run.start()
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
            run.close()
            edith_cross.redact(h, run, spec)
            write_json(root / f'{spec["peer"]}-record.json', run.record)
            results[spec['peer']] = 0 if run.record.get('exit_code') == 0 else 1
            print(f'{stamp()} {spec["peer"]} exit={run.record.get("exit_code")} timed_out={run.record.get("timed_out")} '
                  f'exe={str(run.record.get("exe_sha256"))[:16]}', flush=True)
        if receipt:
            receipt.append(dict(tailscale(['up']), step='up'))
            write_json(root / f'{specs[0]["box"]}-tailscale.json', receipt)
        document['peers'] = [redacted(spec) for spec in specs]
        write_json(spec_path, document)
    h.records.compress_case_records(root)
    from relay_secrets import walk_files
    skipped = ('.exe', '.dll', '.pdb', '.png', '.mp4')
    files = [path.relative_to(root).as_posix() for path in walk_files(root)
             if not path.name.lower().endswith(skipped) and path.stat().st_size <= 8 << 20 and not path.name.endswith('.tar')]
    (root / f'{specs[0]["box"]}-evidence.txt').write_text('\n'.join(files) + '\n', encoding='utf-8')
    return 0 if all(value == 0 for value in results.values()) and len(results) == len(specs) else 1


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
    """review.json, the plan's product: the scenario's checklist, each item judged from its run's verdict."""
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
        if check.startswith('route:') or check.startswith('address:'):
            kind, peer = check.split(':', 1)
            entry = verdict['relay_evidence']['peers'].get(peer, {})
            reasons = [reason for reason in verdict['relay_evidence']['reasons'] if reason.startswith(f'{peer}:')]
            reasons = [reason for reason in reasons if ('relay candidate' in reason or 'sent' in reason) == (kind == 'address')]
            state = 'PASS' if entry and not reasons else 'FAIL'
            evidence = dict(route=entry.get('route'), receipts=[row['line'] for row in entry.get('accepted', [])][:2],
                            relay_addresses=entry.get('relay_addresses'), candidates=entry.get('candidates', [])[:6])
        elif check == 'offer':
            reasons = [reason for reason in verdict['relay_evidence']['reasons'] if 'offer' in reason or 'client report' in reason]
            state, evidence = ('FAIL' if reasons else 'PASS'), dict(session=verdict['session_id'], reasons=reasons)
        elif check == 'direct_expected':
            state = 'PASS' if verdict['relay_evidence'].get('direct_as_expected') else 'FAIL'
            evidence = dict(routes=verdict['relay_evidence']['routes'])
        else:
            state = 'PASS' if verdict['checks'].get(check) else 'FAIL'
            evidence = {check: verdict['checks'].get(check)}
            if check == 'rtt_recorded':
                evidence['rtt'] = verdict['rtt']
            if check == 'hashes_equal':
                evidence.update(compared=verdict['compared_ticks'], desyncs=verdict['desyncs'])
        items.append(dict(item, state=state, evidence=evidence))
    passed = bool(items) and all(item['state'] == 'PASS' for item in items)
    document = dict(schema=1, scenario=scenario['name'], driver=scenario['driver'], verdict='PASS' if passed else 'FAIL', passed=passed,
                    status='PASS' if passed else 'FAIL', checklist=items, runs=[dict(name=v['name'], root=v.get('root'), passed=v.get('passed'))
                    for v in verdicts], counts=dict(executed=sum(item['state'] != 'not-run' for item in items),
                                                    failed=sum(item['state'] == 'FAIL' for item in items)), finished=stamp())
    write_json(out / 'review.json', document)
    return document


def table(roots: list[Path]) -> dict:
    rows = []
    for root in roots:
        verdict = read_json(Path(root) / 'relay-verdict.json')
        rows.append(dict(run=verdict.get('name'), relay=verdict.get('relay'), passed=verdict.get('passed'),
                         routes=(verdict.get('relay_evidence') or {}).get('routes'),
                         rtt_ms=[row['rtt_ms'] for row in verdict.get('rtt', [])],
                         input_delay_frames=[row['delay_frames'] for row in verdict.get('rtt', [])],
                         peers={name: {key: value[key] for key in ('box', 'wall_tps', 'longest_steady_wait_ms', 'waiting_percent', 'holds', 'live_ticks')}
                                for name, value in (verdict.get('peers') or {}).items()},
                         desyncs=verdict.get('desyncs'), stun_legs=verdict.get('stun_legs')))
    return dict(schema=1, made=stamp(), rows=rows)


def main(argv=None) -> int:
    global DRY_RUN
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--scenario')
    parser.add_argument('--run', action='append', default=[])
    parser.add_argument('--out', type=Path)
    parser.add_argument('--box', action='append', default=[], metavar='PEER=BOX', help='place a peer on another box than the scenario names')
    parser.add_argument('--tree', action='append', default=[], metavar='BOX=PATH', help="a box's engine tree instead of boxes.json's")
    parser.add_argument('--ticks', type=int, default=MATCH_TICKS)
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--table', type=Path, nargs='+')
    parser.add_argument('--remote-peers')
    options = parser.parse_args(argv)
    if options.remote_peers:
        return remote_peers(options.remote_peers)
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
    boxes = load_boxes(trees)
    from relay_secrets import SecretBook
    book = SecretBook()
    out = options.out.resolve()
    if not DRY_RUN:
        out.mkdir(parents=True, exist_ok=False)
    verdicts = []
    for run in scenario['runs']:
        if options.run and run['name'] not in options.run:
            continue
        resolved = resolve_run(scenario, run, placement)
        try:
            verdicts.append(run_one(scenario, resolved, out, boxes, options.ticks, book))
        except Exception as error:
            say(f'{run["name"]}: {type(error).__name__}: {error}')
            verdicts.append(dict(name=run['name'], root=str(out / run['name']), passed=False, refused=f'{type(error).__name__}: {error}'))
    if DRY_RUN:
        print(json.dumps(verdicts, indent=1))
        return 0
    identities = out / 'identities'
    identities.mkdir(exist_ok=True)
    for verdict in verdicts:
        for name, value in (verdict.get('identities') or read_json(Path(verdict.get('root', out)) / 'identities.json')).items():
            write_json(identities / f'{name.upper()}.json', value)
    document = review(scenario, verdicts, out)
    scan = book.scan([out])
    write_json(out / 'secret-scan.json', scan)
    say(f'{scenario["name"]}: {document["verdict"]} {document["counts"]} secret scan clean={scan["clean"]} ({scan["files_scanned"]} files)')
    return 0 if document['passed'] and scan['clean'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
