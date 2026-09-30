"""The draw p99 ratio over repeated matrices: the median of the arm's p99 over the median of the single-player baseline's p99.

At sub-millisecond draw times one p99 is scheduler noise, so the ratio is read from the medians of five whole matrices (the lead's
ruling R2, 2026-09-30); a row with fewer arm or baseline repeats is not judged and fails. The pin's own threshold is unchanged.

    python tools/feel/draw_median.py <matrix root> [<matrix root> ...] [--threshold 1.5]
"""
from __future__ import annotations

import argparse
import json
import statistics
from pathlib import Path

ARMS = ('100ms-60hz', '100ms-uncapped', '200ms-60hz', '200ms-uncapped')
REPEATS = 5  # the ruling's five whole matrices; fewer is not judged


def p99(report: dict, peer: str) -> float | None:
    peers = report.get('peers')
    metrics = (peers[peer] if peers else report).get('metrics', {}) if (not peers or peer in peers) else {}
    draw = metrics.get('draw_ms') or {}
    return draw.get('p99')


def medians(roots: list[Path], threshold: float) -> dict:
    result = {}
    for arm in ARMS:
        cap = arm.split('-', 1)[1]
        baseline = [p99(json.loads((root / f'baseline-{cap}/feel-report.json').read_text(encoding='utf-8')), 'sp')
                    for root in roots if (root / f'baseline-{cap}/feel-report.json').is_file()]
        baseline = [value for value in baseline if value is not None]
        for peer in ('host', 'client'):
            values = [p99(json.loads((root / f'{arm}-on/feel-report.json').read_text(encoding='utf-8')), peer)
                      for root in roots if (root / f'{arm}-on/feel-report.json').is_file()]
            values = [value for value in values if value is not None]
            ratio = statistics.median(values) / statistics.median(baseline) if values and baseline else None
            judged = len(values) >= REPEATS and len(baseline) >= REPEATS
            result[f'{arm}/{peer}'] = dict(repeats=len(values), baseline_repeats=len(baseline),
                                           arm_p99_ms=values, baseline_p99_ms=baseline, median_ratio=ratio,
                                           status='JUDGED' if judged else 'NOT JUDGED',
                                           passed=judged and ratio is not None and ratio <= threshold)
    return result


def self_test() -> int:
    from tempfile import TemporaryDirectory
    with TemporaryDirectory() as folder:
        roots = []
        for index, (arm_value, base_value) in enumerate(((0.78, 0.38), (0.52, 0.50), (0.50, 0.52), (0.55, 0.49), (0.51, 0.51))):
            root = Path(folder) / f'm{index}'
            for cap in ('60hz', 'uncapped'):
                (root / f'baseline-{cap}').mkdir(parents=True)
                (root / f'baseline-{cap}/feel-report.json').write_text(json.dumps(dict(peer='sp', metrics=dict(draw_ms=dict(p99=base_value)))))
            for arm in ARMS:
                (root / f'{arm}-on').mkdir(parents=True)
                (root / f'{arm}-on/feel-report.json').write_text(json.dumps(dict(peers={peer: dict(metrics=dict(draw_ms=dict(p99=arm_value))) for peer in ('host', 'client')})))
            roots.append(root)
        result = medians(roots, 1.5)
        row = result['100ms-60hz/host']
        # One noisy matrix (0.78 against 0.38, a ratio of 2.05) does not decide it: the medians are 0.52 and 0.50.
        ok = row['repeats'] == 5 and abs(row['median_ratio'] - 0.52 / 0.50) < 1e-9 and row['passed']
        # Four matrices are not the ruling's five: the row is not judged, and fails.
        short = medians(roots[:4], 1.5)['100ms-60hz/host']
        short_ok = not short['passed'] and short.get('status') == 'NOT JUDGED'
    print(f"[draw-median self-test] {'PASS' if ok else 'FAIL'} median ratio {row['median_ratio']:.3f} over {row['repeats']} repeats")
    print(f"[draw-median self-test] {'PASS' if short_ok else 'FAIL'} four matrices: passed={short['passed']} status={short.get('status')}")
    return 0 if ok and short_ok else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('roots', nargs='*', type=Path)
    parser.add_argument('--threshold', type=float, default=1.5, help="the pin's own ratio bound (feel/report.py draw_p99)")
    parser.add_argument('--self-test', action='store_true')
    options = parser.parse_args()
    if options.self_test:
        return self_test()
    result = medians(options.roots, options.threshold)
    for name, row in result.items():
        print(f"[draw-median] {'PASS' if row['passed'] else 'FAIL'} {name} {row['status']} median_ratio={row['median_ratio']} repeats={row['repeats']}/{row['baseline_repeats']}")
    return 0 if all(row['passed'] for row in result.values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())
