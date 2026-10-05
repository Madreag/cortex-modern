"""A held seat released by the host under lag and jitter: the host and the survivor agree on every tick through the release.

Three engines play a service match with fake lag and jitter on every link. The client's simulation stalls, the host holds its
seat for the AI, and a while later kicks the held seat through the Seats panel's own removal (CCCP_TEST_KICK_HELD_AFTER_MS).
The release lands on one agreed frame: the host and the survivor both name it, and their per-tick hashes agree on every tick
the two of them ran, the release frame included.

    python tools/feel/release_held_seat.py --out D:/mx/<lane>/scene-1 --port <a free port>
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import feel_measure as measure
from feel.retained_resume import compare_live_hashes

KICKED = re.compile(r'\[net-test\] kicked held seat (\d+) after (\d+)ms: (\w+)')
RELEASED = re.compile(r'\[net-match\] seat (\d+) released at frame (\d+): its claims end')
# A peer that finds its state apart from another's says so on one of these lines.
DESYNC = re.compile(r'desync|checksum mismatch|state mismatch', re.IGNORECASE)


def read(path: Path) -> str:
    return path.read_text(encoding='utf-8-sig', errors='replace') if path.is_file() else ''


def judge(run: Path) -> dict:
    """The scene's verdict from one run's files."""
    logs = {peer: read(run / peer / 'stdout.log') for peer in ('host', 'client', 'survivor')}
    kicked = [dict(seat=int(seat), after_ms=int(after), result=result) for seat, after, result in KICKED.findall(logs['host'])]
    released = {peer: [dict(seat=int(seat), frame=int(frame)) for seat, frame in RELEASED.findall(logs[peer])] for peer in ('host', 'survivor')}
    desyncs = {peer: [line for line in logs[peer].splitlines() if DESYNC.search(line)] for peer in ('host', 'survivor')}
    comparison = compare_live_hashes(run / 'host-live.jsonl', run / 'survivor-live.jsonl', 1)
    records = json.loads(read(run / 'run-result.json') or '{}')
    exits = {peer: records.get(peer, {}).get('exit_code') for peer in ('host', 'client', 'survivor')}
    release_frames = {peer: [row['frame'] for row in rows] for peer, rows in released.items()}
    last_compared = max((row['last_tick'] for row in comparison), default=0)
    reasons = []
    if not kicked or kicked[0]['result'] != 'Ok':
        reasons.append(f'the host did not kick the held seat: {kicked}')
    if not release_frames['host'] or release_frames['host'] != release_frames['survivor']:
        reasons.append(f'the release frames differ: host {release_frames["host"]} survivor {release_frames["survivor"]}')
    if not comparison or any(row['mismatched_ticks'] or row['mismatched_applied_input_ticks'] or not row['compared_ticks'] for row in comparison):
        reasons.append(f'the per-tick hashes differ: {comparison}')
    elif release_frames['host'] and last_compared <= release_frames['host'][0]:
        reasons.append(f'the hashes stop at {last_compared}, before the release at {release_frames["host"][0]}')
    if any(desyncs.values()):
        reasons.append(f'desync lines: {desyncs}')
    if exits['host'] != 0 or exits['survivor'] != 0:
        reasons.append(f'the host or the survivor did not exit cleanly: {exits}')
    return dict(passed=not reasons, reasons=reasons, kicked=kicked, release_frames=release_frames, comparison=comparison,
                desync_lines={peer: len(lines) for peer, lines in desyncs.items()}, exits=exits)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--lag', type=int, default=100, help='fake one-way lag on every peer, ms')
    parser.add_argument('--jitter', type=int, default=40, help='fake jitter on every peer, ms')
    parser.add_argument('--stall-tick', type=int, default=600, help='the tick the client stalls at')
    parser.add_argument('--kick-after-ms', type=int, default=2000, help='how long the AI holds the seat before the host kicks it')
    parser.add_argument('--ticks', type=int, default=2400)
    parser.add_argument('--timeout', type=int, default=600)
    parser.add_argument('--judge-only', type=Path, help='judge an earlier run directory instead of launching')
    args = parser.parse_args()
    if args.judge_only:
        verdict = judge(args.judge_only.resolve())
        print(json.dumps(verdict, indent=2))
        return 0 if verdict['passed'] else 1
    root = args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    script = root / 'input.txt'
    measure.input_pattern(script)
    # Every engine inherits the lever; only a host kicks.
    os.environ['CCCP_TEST_KICK_HELD_AFTER_MS'] = str(args.kick_after_ms)
    # The client stays stalled past the match's end, so the kick meets a seat still held.
    stall_ms = int((args.ticks - args.stall_tick) * 1000 / 60) + 10000
    run = measure.launch_case(root, 'release', args.lag, 60, False, args.port, script,
                              measure.file_record(measure.engine_executable(measure.REPO))['sha256'], args.timeout,
                              three_peers=True, live_stalls=[(args.stall_tick, stall_ms)], jitter_ms=args.jitter, window_ticks=args.ticks)
    verdict = judge(run)
    verdict['run'] = str(run)
    measure.write_json(root / 'result.json', verdict)
    print(f"[release-held-seat] {'PASS' if verdict['passed'] else 'FAIL'} release={verdict['release_frames']} "
          f"compared={sum(row['compared_ticks'] for row in verdict['comparison'])} reasons={verdict['reasons']}", flush=True)
    return 0 if verdict['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
