"""Audit held-world progress from the host's receipts and both lobby counters."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re


def counters(value):
    if isinstance(value, dict):
        if 'catch_up_reports_received' in value:
            yield {key: count for key, count in value.items() if key.startswith('catch_up_reports_')}
        for child in value.values():
            yield from counters(child)
    elif isinstance(value, list):
        for child in value:
            yield from counters(child)


def audit(root):
    root = Path(root)
    rounds = {}
    for name in ('boot1', 'boot2', 'fresh'):
        path = root / 'world-restart' / name
        log_path = path / 'host/stdout.log'
        log = log_path.read_text(encoding='utf-8-sig', errors='replace') if log_path.is_file() else ''
        acknowledged = [int(value) for value in re.findall(r'\[net-world\] (?:tail|catch-up gate) [^\n]*\backnowledged=(\d+)', log)]
        rtt = [float(value) for value in re.findall(r'\[net-world\] (?:tail|catch-up gate) [^\n]*\brtt_ms=([\d.]+)', log)]
        peer_counters = {}
        named_drops = {}
        for peer in ('host', 'client'):
            report = path / f'{peer}_report.json'
            peer_counters[peer] = list(counters(json.loads(report.read_text(encoding='utf-8-sig')))) if report.is_file() else []
            peer_log = path / peer / 'stdout.log'
            text = peer_log.read_text(encoding='utf-8-sig', errors='replace') if peer_log.is_file() else ''
            named_drops[peer] = [line for line in text.splitlines() if '[net-lobby] catch-up report dropped' in line or
                                 '[net-match] catch-up report refused by the wire:' in line]
            for sent, received, refused, dropped in re.findall(r' reports_sent=(\d+) reports_received=(\d+) reports_refused=(\d+) reports_dropped=(\d+)', text):
                peer_counters[peer].append(dict(zip(('catch_up_reports_sent', 'catch_up_reports_received', 'catch_up_reports_refused', 'catch_up_reports_dropped'),
                                                    map(int, (sent, received, refused, dropped)))))
        drops = [row for peer in peer_counters.values() for row in peer if row.get('catch_up_reports_dropped', 0) or row.get('catch_up_reports_refused', 0)]
        rounds[name] = dict(acknowledged=acknowledged, rtt_ms=rtt, counters=peer_counters,
                            progress=bool(acknowledged and max(acknowledged) > min(acknowledged)),
                            measured_rtt=any(value > 0 for value in rtt), drops=drops, named_drops=named_drops, evidence=str(log_path))
    reasons = []
    if not all(row['progress'] for row in rounds.values()):
        reasons.append('a world round has no advancing host acknowledgement')
    if not any(row['measured_rtt'] for row in rounds.values()):
        reasons.append('no host tail has a positive measured RTT')
    if not all(all(row['counters'].values()) for row in rounds.values()):
        reasons.append('a peer has no retained catch-up counters')
    if any(row['drops'] or any(row['named_drops'].values()) for row in rounds.values()):
        reasons.append('a catch-up report was refused or dropped; inspect counters and the named route')
    return dict(passed=not reasons, reasons=reasons, rounds=rounds)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    args = parser.parse_args()
    result = audit(args.root)
    (args.root / 'catch-up-audit.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(dict(passed=result['passed'], reasons=result['reasons'])))
    raise SystemExit(0 if result['passed'] else 1)
