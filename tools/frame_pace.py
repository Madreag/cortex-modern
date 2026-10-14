"""Drawn frames, from an engine's own lines: does a running match draw at the rate its presentation aims at, as evenly as
single player does?

The engine reports, per 60 simulation ticks of a running activity, the frames it presented (`[frame-pace]`: fps on the steady
clock, the present-to-present interval p50/p95/max, and target_hz - the harness cap, else the display's refresh under vsync).
A lockstep round always reports it; single player under CC_TEST_FRAME_PACE=1. An engine older than that line is read from its
pace census instead (`frames_drawn` over `sim_ticks` per census window, at the sim's 60 ticks a second; no intervals).

The check, per window after the round's first SKIP and before its last: fps >= FPS_SHARE x target, and the interval p95 at
most P95_PERIODS frame periods. While the frame recorder runs, the target is its capture rate (`recorder_hz`, or the
`[record-video] ... at N fps` line of an older engine): a recording's frames are the ones that must arrive.

    python tools/frame_pace.py <stdout.log> [--mode lockstep|single] [--expect-hz 60]
"""
from __future__ import annotations

import argparse
import json
import re
import statistics
import sys
from pathlib import Path

FRAME_PACE = re.compile(r'^\[frame-pace\] (.*)$', re.M)
CENSUS = re.compile(r'pace: iterations=\d+ sim_ticks=(\d+) .*? frames_drawn=(\d+) frames_shed=(\d+)')
RECORDING = re.compile(r'^\[record-video\] recording to .* at (\d+) fps', re.M)
SIM_HZ = 60.0
# A window may draw this share of the target and still pass: one or two frames of a 60-frame second.
FPS_SHARE = 0.95
# The interval p95 band in frame periods; single player's measured p95 lies inside it (the row's evidence).
P95_PERIODS = 1.25
# The round's first windows hold its start (loading, the first catch-up); its last may be cut by the round's end.
SKIP = 3


def windows(text: str, mode: str | None = None) -> tuple[str, list[dict]]:
    """('frame-pace', rows) from the engine's own frame lines, else ('census', rows) from its pace census, else ('none', [])."""
    rows = []
    recording = RECORDING.search(text)
    for match in FRAME_PACE.finditer(text):
        row = dict(part.split('=', 1) for part in match.group(1).split() if '=' in part)
        if mode and row.get('mode') != mode:
            continue
        rows.append(dict(source='frame-pace', mode=row.get('mode'), recorder=row.get('recorder') == '1',
                         recorder_hz=float(row.get('recorder_hz') or (recording.group(1) if recording else 0)),
                         target_hz=float(row.get('target_hz', 0) or 0), first_tick=int(row.get('first_tick', row['tick'])),
                         tick=int(row['tick']), frames=int(row['frames']),
                         fps=float(row['fps']), p95_ms=float(row['interval_p95_ms']), max_ms=float(row['interval_max_ms'])))
    if rows:
        return 'frame-pace', rows
    for match in CENSUS.finditer(text):
        ticks, drawn = int(match.group(1)), int(match.group(2))
        if ticks >= 600:
            rows.append(dict(source='census', mode='lockstep', recorder=bool(recording), recorder_hz=float(recording.group(1)) if recording else 0.0,
                             target_hz=0.0, first_tick=None, tick=None, frames=drawn, fps=SIM_HZ * drawn / ticks, p95_ms=None, max_ms=None))
    return ('census' if rows else 'none'), rows


def check(text: str, mode: str | None = 'lockstep', expect_hz: float | None = None, skip: int = SKIP,
          exclude: list[tuple[int, int]] = ()) -> dict:
    """exclude: tick ranges another rule judges; a window overlapping one is left out."""
    source, rows = windows(text, mode)
    rows = [row for row in rows if row['tick'] is None or not any(row['first_tick'] <= last and row['tick'] >= first for first, last in exclude)]
    if not rows:
        return dict(status='NONE', passed=True, source=source, reason='no running activity reported frames', windows=0)
    # A census window is a minute: only the first (the round's start) and the last (its end) are left out.
    edge = skip if source == 'frame-pace' else 1
    judged = rows[edge:-1] if len(rows) > edge + 1 else rows
    presentation = expect_hz or max((row['target_hz'] for row in judged), default=0.0) or 60.0
    recorded = [row['recorder_hz'] for row in judged if row['recorder'] and row['recorder_hz'] > 0]
    target = min(recorded) if recorded and not expect_hz else presentation
    floor = FPS_SHARE * target
    band = P95_PERIODS * 1000.0 / target
    slow = [row for row in judged if row['fps'] < floor]
    uneven = [row for row in judged if row['p95_ms'] is not None and row['p95_ms'] > band]
    result = dict(status='PASS' if not slow and not uneven else 'FAIL', source=source, mode=mode, target_hz=target,
                  recording_hz=min(recorded) if recorded else None,
                  fps_floor=round(floor, 2), p95_band_ms=round(band, 2), windows=len(judged),
                  fps_min=round(min(row['fps'] for row in judged), 2), fps_median=round(statistics.median(row['fps'] for row in judged), 2),
                  p95_max_ms=max((row['p95_ms'] for row in judged if row['p95_ms'] is not None), default=None),
                  slow_windows=len(slow), uneven_windows=len(uneven),
                  first_slow=slow[0] if slow else None, first_uneven=uneven[0] if uneven else None)
    result['passed'] = result['status'] == 'PASS'
    if not result['passed']:
        first = (slow or uneven)[0]
        result['reason'] = (f"{len(slow)} of {len(judged)} windows drew under {floor:.1f} fps (first {first['fps']:.1f}"
                            + (f" at tick {first['tick']}" if first['tick'] is not None else '') + f"), {len(uneven)} with interval p95 over {band:.1f} ms")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('log', type=Path)
    parser.add_argument('--mode', choices=('lockstep', 'single'), default='lockstep')
    parser.add_argument('--expect-hz', type=float)
    args = parser.parse_args()
    result = check(args.log.read_text(encoding='utf-8', errors='replace'), args.mode, args.expect_hz)
    print(json.dumps(result, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
