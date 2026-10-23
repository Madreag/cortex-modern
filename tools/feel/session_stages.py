"""Reduce retained input-to-presentation stages without changing the B3 oracle."""
from pathlib import Path
import argparse
import bisect
from collections import defaultdict
import json
import statistics
import sys

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'tools'))
from feel import report


def analyze(root):
    cases = json.loads((root / 'cases.json').read_text())
    observations = []
    for case in cases:
        for peer in case['peers']:
            frames = []
            committed = defaultdict(list)
            for row in report.read_jsonl(peer['raw']):
                if row['type'] == 'frame' and row['active'] and 1 <= row['tick'] <= report.TICKS:
                    frames.append(row)
                elif row['type'] == 'committed':
                    committed[row['tick']].append({'wall_ms': row['wall_ms']})
            drawn = {r['frame']: r for r in frames}
            draw_times = [r['draw_begin_ms'] for r in frames]
            edges = []
            for edge in peer['input_edges']:
                if edge['frame'] is None:
                    edges.append(dict(tick=edge['tick'], right_censored=True))
                    continue
                response = drawn[edge['frame']]
                first = frames[bisect.bisect_left(draw_times, edge['input_wall_ms'])]
                previous = drawn.get(edge['last_presented_frame'])
                commit = next((r for r in committed[edge['tick']] if r['wall_ms'] >= edge['input_wall_ms']), None)
                components = dict(
                    press_to_first_draw_ms=first['draw_begin_ms'] - edge['input_wall_ms'],
                    first_to_response_draw_ms=response['draw_begin_ms'] - first['draw_begin_ms'],
                    response_draw_ms=response['present_begin_ms'] - response['draw_begin_ms'],
                    response_upload_ms=response['present_end_ms'] - response['present_begin_ms'],
                )
                total = sum(components.values())
                if abs(total - edge['ms']) > 1e-6:
                    raise ValueError('presentation stages do not reconstruct the measured response')
                edges.append(dict(tick=edge['tick'], action=edge['action'], held=edge['held'],
                                  team=edge['reference']['team'], latency_ms=edge['ms'],
                                  latency_frames=edge['frames'], response_tick=response['tick'],
                                  preview_depth=response['preview_depth'], carried_ms=edge['carried_ms'],
                                  carried_to_response_ms=None if edge['carried_ms'] is None else edge['ms'] - edge['carried_ms'],
                                  sample_after_previous_present_ms=None if previous is None else edge['input_wall_ms'] - previous['present_end_ms'],
                                  input_to_post_sim_hook_ms=None if commit is None else commit['wall_ms'] - edge['input_wall_ms'],
                                  **components))
            groups = defaultdict(list)
            for edge in edges:
                if not edge.get('right_censored'):
                    groups[f"{edge['team']}:{edge['action']}:{int(edge['held'])}"].append(edge)
            keys = ('latency_ms', 'latency_frames', 'press_to_first_draw_ms', 'first_to_response_draw_ms',
                    'response_draw_ms', 'response_upload_ms', 'sample_after_previous_present_ms', 'input_to_post_sim_hook_ms',
                    'carried_ms', 'carried_to_response_ms')
            def medians(rows):
                return {key: statistics.median(row[key] for row in rows if row.get(key) is not None)
                        for key in keys if any(row.get(key) is not None for row in rows)}
            observations.append(dict(arm=case['arm'], repeat=case['repeat'], peer=peer['peer'],
                                     baseline_team=case.get('baseline_team'), quiet=case['manifest']['quiet'],
                                     medians=medians(edges), actions={key: medians(rows) for key, rows in groups.items()},
                                     frame_interval_ms=report.distribution([r['interval_ms'] for r in frames]),
                                     edges=edges))
            print(f"[session-stages] {case['arm']} {case['repeat']} {peer['peer']}", flush=True)
    result = dict(binary_sha256=cases[0]['manifest']['binary_sha256'],
                  scope='Software input sample to completed UploadFrame, one headed box. No oracle tolerance added.',
                  observations=observations)
    (root / 'stages.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps([dict(arm=r['arm'], repeat=r['repeat'], peer=r['peer'], team=r['baseline_team'],
                           medians=r['medians']) for r in observations], indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    analyze(parser.parse_args().root.resolve())
