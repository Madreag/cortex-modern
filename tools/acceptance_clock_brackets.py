"""Check native probe ordering around a crawl without equating machine clocks."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re

SEATED = ('seated-one', 'seated-two', 'seated-three')
PLACEMENT = {'host': 'Z13', 'seated-one': 'Z13', 'seated-two': 'EDITH',
             'seated-three': 'EDITH', 'spectator': 'Linux'}


def file_digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def collect_brackets(root, plan, cost):
    root = Path(root)
    events = json.loads((root/'clock-order.json').read_text(encoding='utf-8'))
    placement = plan['placement']
    if placement != PLACEMENT:
        raise ValueError('native clock bracket placement differs from NOTE 11')
    facts = dict(method='native-probe-brackets-v1', placement=placement, events=events,
                 native_cost=cost, peers={})
    for event in events:
        peer = event['peer']
        box = placement[peer]
        mark = {'crawl-start': 'crawl-start', 'crawl-end': 'crawl-end',
                'crawl-armed': 'crawl-armed', 'crawl-closed': 'crawl-complete'}[event['kind']]
        path = root/'boxes'/box/(peer+'-stage')/'probe'/(mark+'.ownership.json')
        if file_digest(path) != event['sha256']:
            raise ValueError(peer+': native clock bracket file differs from its ordered receipt')
        document = json.loads(path.read_text(encoding='utf-8'))
        if event['kind'] == 'crawl-armed':
            facts['armed_dump'] = document
        elif event['kind'] == 'crawl-closed':
            signal = path.with_name('crawl-complete.json')
            if file_digest(signal) != event['signal_sha256'] or document.get('sim_cost') != cost:
                raise ValueError('native cost window or completion signal differs')
            facts['cost_dump'] = document
        else:
            key = 'begin' if event['kind'] == 'crawl-start' else 'end'
            facts['peers'].setdefault(peer, {})[key] = document
    return facts


def errors(facts):
    proof = facts.get('clock_brackets', {})
    peers, cost = facts.get('peers', {}), facts.get('throttle', {})
    problems = []
    def require(value, reason):
        if not value:
            problems.append('throttle: '+reason)
    require(proof.get('method') == 'native-probe-brackets-v1', 'native cross-box clock proof is missing')
    require(proof.get('placement') == PLACEMENT, 'native cross-box placement differs from NOTE 11')
    require(proof.get('native_cost') == cost and cost.get('closed') is True,
            'native spectator cost did not close with the retained receipt')
    require(proof.get('cost_dump', {}).get('process') == 'spectator' and
            proof.get('cost_dump', {}).get('sim_cost') == cost,
            'native closed-window dump is not bound to the watcher')
    armed = proof.get('armed_dump', {})
    require(armed.get('process') == 'spectator' and type(armed.get('pid')) is int and armed['pid'] > 0 and
            armed['pid'] == proof.get('cost_dump', {}).get('pid') == cost.get('pid') and type(armed.get('sim_tick')) is int and
            type(cost.get('first_tick')) is int and armed['sim_tick'] < cost['first_tick'],
            'seated clock probes were not acknowledged before the native crawl began')
    events = proof.get('events', [])
    expected = {('crawl-armed', 'spectator'), ('crawl-closed', 'spectator')} | \
               {(kind, peer) for peer in SEATED for kind in ('crawl-start', 'crawl-end')}
    if not isinstance(events, list) or any(not isinstance(event, dict) for event in events):
        return problems+['throttle: ordered native probe receipts missing']
    require(len(events) == 8 and {(event.get('kind'), event.get('peer')) for event in events} == expected,
            'ordered native probe receipts are missing or duplicated')
    require([event.get('sequence') for event in events] == list(range(1, len(events)+1)),
            'native probe order has gaps or was reordered')
    index = {(event.get('kind'), event.get('peer')): event for event in events}
    go = index.get(('crawl-armed', 'spectator'), {}).get('sequence')
    closed = index.get(('crawl-closed', 'spectator'), {}).get('sequence')
    require(type(go) is int and type(closed) is int and go < closed,
            'native clock acknowledgement did not precede the closed window')
    for event in events:
        require(re.fullmatch('[0-9a-f]{64}', str(event.get('sha256'))) is not None,
                'native probe digest is missing')
    require(re.fullmatch('[0-9a-f]{64}', str(index.get(('crawl-closed', 'spectator'), {}).get('signal_sha256'))) is not None,
            'native crawl-completion signal digest is missing')
    require(peers.get('spectator', {}).get('box') == 'linux', 'watcher native clock box differs')
    for peer in SEATED:
        value = peers.get(peer, {})
        bracket = proof.get('peers', {}).get(peer, {})
        begin, end = bracket.get('begin', {}), bracket.get('end', {})
        first = index.get(('crawl-start', peer), {}).get('sequence')
        last = index.get(('crawl-end', peer), {}).get('sequence')
        require(type(first) is int and type(last) is int and type(go) is int and type(closed) is int and
                first < go < closed < last, peer+' native timing does not bracket the crawl')
        require(value.get('box') == PLACEMENT[peer].lower(), peer+' timing has the wrong native clock box')
        require(begin.get('process') == end.get('process') == peer and type(begin.get('pid')) is int and
                begin['pid'] > 0 and begin['pid'] == end.get('pid'), peer+' native probe process changed')
        low, high = begin.get('sim_tick'), end.get('sim_tick')
        require(type(low) is int and type(high) is int and low > 1 and high > low and
                value.get('first') == low-1 and value.get('last') == high+1,
                peer+' timing omits the enclosing native probe ticks')
        elapsed = value.get('last_wall_ms', 0)-value.get('first_wall_ms', 0) \
            if all(type(value.get(key)) in (int, float) for key in ('first_wall_ms', 'last_wall_ms')) else -1
        duration = cost.get('end_ms', 0)-cost.get('start_ms', 0) \
            if all(type(cost.get(key)) in (int, float) for key in ('start_ms', 'end_ms')) else 0
        require(duration >= 30000 and elapsed >= duration, peer+' native bracket is shorter than the crawl')
    return problems
