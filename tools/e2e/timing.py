"""Measure handover presentation from the recorder's shared steady clock."""

import json
from pathlib import Path
import re


def intervals(events, name, end_ms):
    spans, start = [], None
    for event in events:
        match = re.search(rf'\b{name}=(\d+)', event['message'])
        if not match:
            continue
        active = bool(int(match[1]))
        if active and start is None:
            start = event['wall_ms']
        elif not active and start is not None:
            spans.append({'start_ms': start, 'end_ms': event['wall_ms'], 'seconds': (event['wall_ms'] - start) / 1000, 'complete': True})
            start = None
    if start is not None:
        spans.append({'start_ms': start, 'end_ms': end_ms, 'seconds': max(0, end_ms - start) / 1000, 'complete': False})
    return spans


def migration_timing(capture, survivors):
    host = next((peer for peer in capture['peers'] if peer['peer'] == 'host'), {})
    game = [frame for frame in host.get('index', []) if frame.get('screen') == 'game']
    last = game[-1] if game else None
    result = {'clock': 'FrameRecorder steady wall_ms, shared by the local peer processes', 'host_last_frame': last, 'survivors': []}
    for peer in capture['peers']:
        if peer['peer'] not in survivors:
            continue
        path = Path(peer.get('video_dir', peer.get('root', ''))) / 'events.jsonl'
        events = [json.loads(line) for line in path.read_text(encoding='utf-8').splitlines()] if path.is_file() else []
        toasts = [event for event in events if event['message'].startswith('handover_toast ')]
        status = [event for event in events if event['message'].startswith('net_status ')]
        end = max([frame['wall_ms'] for frame in peer.get('index', [])] + [event['wall_ms'] for event in events] + [0])
        toast = toasts[0] if toasts else None
        longest = re.search(r'\blongest_ms=(\d+)', toast['message']) if toast else None
        frame = min(peer.get('index', []), key=lambda row: abs(row['wall_ms'] - toast['wall_ms'])) if toast and peer.get('index') else None
        result['survivors'].append({'peer': peer['peer'], 'events': str(path), 'handover_toast': toast,
            'toast_nearest_frame': frame, 'seconds_from_host_last_frame': (toast['wall_ms'] - last['wall_ms']) / 1000 if toast and last else None,
            'overlay_longest_ms_at_toast': int(longest[1]) if longest else None,
            'host_lost_intervals': intervals(status, 'host_lost', end), 'local_slow_intervals': intervals(status, 'local_slow', end),
            'status_samples': len(status), 'timing_complete': bool(last and toast)})
    return result
