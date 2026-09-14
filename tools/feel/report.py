"""Reduce retained engine records without substituting estimates for missing samples."""
from __future__ import annotations

from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path
import re

TICKS = 1200
SIM_MS = 1000 / 60


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, allow_nan=False) + '\n', encoding='utf-8')


def write_jsonl(path, rows):
    with Path(path).open('w', encoding='utf-8') as stream:
        for row in rows:
            stream.write(json.dumps(row, allow_nan=False) + '\n')


def read_jsonl(path):
    with Path(path).open(encoding='utf-8-sig') as stream:
        for number, line in enumerate(stream, 1):
            row = json.loads(line, parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))
            if not isinstance(row, dict):
                raise ValueError(f'{path}:{number}: expected an object')
            row['_line'] = number
            yield row


def distribution(values):
    ordered = sorted(values)
    if not ordered:
        return dict(count=0, p50=None, p95=None, p99=None, max=None)
    if any(not math.isfinite(value) for value in ordered):
        raise ValueError('non-finite measurement')
    return dict(count=len(ordered), **{name: ordered[max(0, math.ceil(q * len(ordered)) - 1)]
                                     for name, q in [('p50', .5), ('p95', .95), ('p99', .99), ('max', 1)]})


def angular_delta(a, b):
    return (a - b + math.pi) % (2 * math.pi) - math.pi


def wrapped_delta(delta, size, wraps):
    return (delta + size / 2) % size - size / 2 if wraps and size else delta


def pin(value, rule, passed, evidence, detail=None):
    return dict(value=value, rule=rule, status='PASS' if passed and value is not None else 'MISS',
                evidence=[str(path) for path in evidence], detail=detail)


def input_latencies(inputs, frames):
    results = []
    by_actor = defaultdict(list)
    for frame in frames:
        for actor in frame['actors']:
            by_actor[actor['uid']].append((frame, actor))
    for index, edge in enumerate(inputs):
        sequence = by_actor[edge['actor']['uid']]
        before = [(frame, actor) for frame, actor in sequence if frame['present_end_ms'] < edge['wall_ms']]
        reference = before[-1][1] if before else edge['actor']
        for change in edge['changes']:
            action, held = change['action'], change['held']
            if action not in ('AIM_VECTOR', 'L_LEFT', 'L_RIGHT'):
                continue
            def reflects(actor):
                if action == 'AIM_VECTOR':
                    if not held:
                        return False
                    target = -math.atan2(change['y'], change['x'])
                    command_angle = -math.atan2(actor['input']['aim_y'], actor['input']['aim_x'])
                    return (abs(angular_delta(actor['aim'], target)) < abs(angular_delta(reference['aim'], target))
                            and abs(angular_delta(command_angle, target)) < abs(angular_delta(reference['aim'], target))
                            and angular_delta(actor['aim'], reference['aim']) != 0)
                command = actor['input']['move_left' if action == 'L_LEFT' else 'move_right']
                if command != held:
                    return False
                if held:
                    sign = -1 if action == 'L_LEFT' else 1
                    return sign * (actor['vx'] - reference['vx']) > 0
                return abs(actor['vx']) < abs(reference['vx'])
            next_change = min((later['wall_ms'] for later in inputs[index + 1:]
                               if later['actor']['uid'] == edge['actor']['uid']
                               and any(item['action'] == action for item in later['changes'])), default=math.inf)
            found = next(((frame, actor) for frame, actor in sequence
                          if edge['wall_ms'] <= frame['draw_begin_ms'] < next_change and reflects(actor)), None)
            row = dict(input_line=edge['_line'], tick=edge['tick'], uid=edge['actor']['uid'], action=action, held=held,
                       input_wall_ms=edge['wall_ms'], last_presented_frame=edge['last_presented_frame'],
                       reference=reference, frame=None, ms=None, frames=None, budget_ms=None, pass_check=False,
                       observed_through_ms=min(next_change, frames[-1]['present_end_ms']), right_censored=found is None)
            if found:
                frame, actor = found
                elapsed = frame['present_end_ms'] - edge['wall_ms']
                frame_period = 1000 / frame['cap_hz'] if frame['cap_hz'] else frame['interval_ms']
                budget = 34.0 if frame['cap_hz'] == 60 else SIM_MS + frame_period
                row.update(frame=frame['frame'], frame_line=frame['_line'], reflected_actor=actor,
                           present_wall_ms=frame['present_end_ms'], ms=elapsed,
                           frames=frame['frame'] - edge['last_presented_frame'], budget_ms=budget,
                           pass_check=elapsed <= budget)
            row['latency_lower_bound_ms'] = row['ms'] if found else max(0, row['observed_through_ms'] - edge['wall_ms'])
            results.append(row)
    return results


def warp_records(frames):
    previous = {}
    output = []
    for frame in frames:
        visual_tick = frame['tick'] + frame['preview_depth'] - 1 + frame['alpha']
        for actor in frame['actors']:
            old = previous.get(actor['uid'])
            previous[actor['uid']] = (frame, actor, visual_tick)
            if old is None:
                continue
            old_frame, old_actor, old_tick = old
            seconds = (visual_tick - old_tick) / 60
            dx = wrapped_delta(actor['render_x'] - old_actor['render_x'], frame['scene_width'], frame['wraps_x'])
            dy = wrapped_delta(actor['render_y'] - old_actor['render_y'], frame['scene_height'], frame['wraps_y'])
            vx = (actor['vx'] + old_actor['vx']) / 2
            vy = (actor['vy'] + old_actor['vy']) / 2
            rx, ry = dx - vx * 20 * seconds, dy - vy * 20 * seconds
            residual = math.hypot(rx, ry)
            if residual > 0:
                output.append(dict(frame=frame['frame'], raw_line=frame['_line'], previous_raw_line=old_frame['_line'],
                                   tick=frame['tick'], uid=actor['uid'], wall_ms=frame['present_end_ms'],
                                   x=actor['render_x'], y=actor['render_y'], dx=dx, dy=dy,
                                   visual_sim_seconds=seconds, velocity_expected_dx=vx * 20 * seconds,
                                   velocity_expected_dy=vy * 20 * seconds, residual_px=residual,
                                   over_4_px=residual > 4, cause='unclassified kinematic residual'))
    return output


COMMAND = re.compile(r'^\[net-replay-dump\] frame=(\d+) command=(\S+) sender=(\d+)(.*)$')
VOICE = re.compile(r'^\[preview-event\] voice committed=(\d+) tick=(\d+) uid=(\d+) previewed=(\d+) '
                   r'asset=(\d+) preset=(\d+) seq=(\d+) preset_name="(.*?)" path=(.*)$')
POSITION = re.compile(r'^(\d+) actor uid=(\d+) .*? pos=(\S+),(\S+) prev=')


def remote_commands(path, local_peer):
    commands = defaultdict(list)
    if not path.is_file():
        return commands, False
    for line_no, line in enumerate(path.read_text(encoding='utf-8-sig', errors='replace').splitlines(), 1):
        match = COMMAND.match(line)
        if match and int(match[3]) != local_peer:
            commands[int(match[1])].append(dict(command=match[2], sender=int(match[3]), detail=match[4], raw_line=line_no))
    launch_path = path.parent / 'launch.json'
    verify_path = path.parent.parent / 'replay-report.json'
    if not launch_path.is_file() or not verify_path.is_file():
        return commands, False
    launch = json.loads(launch_path.read_text(encoding='utf-8-sig'))
    verify = json.loads(verify_path.read_text(encoding='utf-8-sig'))
    complete = (launch.get('exit_code') == 0 and launch.get('evidence_complete') is True and not launch.get('timed_out')
                and verify.get('ok') is True and verify.get('first_frame') == 1 and verify.get('last_frame', 0) >= TICKS)
    return commands, complete


def canonical_positions(path, wanted):
    actors, ticks = {}, set()
    with path.open(encoding='utf-8-sig') as stream:
        for number, line in enumerate(stream, 1):
            if re.match(r'^\d+ activity ', line):
                ticks.add(int(line.split(' ', 1)[0]))
            match = POSITION.match(line)
            if match:
                key = int(match[1]), int(match[2])
                if key in wanted:
                    if key in actors:
                        raise ValueError(f'{path}:{number}: duplicate committed actor')
                    actors[key] = (dict(pos=[float.fromhex(match[3]), float.fromhex(match[4])]), number)
    if ticks != set(range(1, TICKS + 1)):
        raise ValueError(f'{path}: canonical dump does not cover ticks 1 through {TICKS}')
    return actors


def corrections(previews, committed, canonical_path, command_path, local_peer):
    forecasts = {}
    for row in previews:
        key = (row['target_tick'], row['actor']['uid'])
        if key not in forecasts or row['committed_tick'] > forecasts[key]['committed_tick']:
            forecasts[key] = row
    canonical = canonical_positions(canonical_path, forecasts)
    commands, commands_complete = remote_commands(command_path, local_peer)
    result, missing = [], []
    for (tick, uid), forecast in sorted(forecasts.items()):
        if tick > TICKS:
            continue
        if (tick, uid) not in canonical:
            missing.append(dict(tick=tick, uid=uid, preview_line=forecast['_line']))
            continue
        actor, line = canonical[tick, uid]
        observed = next((row for row in committed if row['tick'] >= tick), None)
        dx, dy = actor['pos'][0] - forecast['actor']['x'], actor['pos'][1] - forecast['actor']['y']
        if observed:
            dx = wrapped_delta(dx, observed['scene_width'], observed['wraps_x'])
            dy = wrapped_delta(dy, observed['scene_height'], observed['wraps_y'])
        distance = math.hypot(dx, dy)
        wall = observed['wall_ms'] if observed else None
        result.append(dict(tick=tick, uid=uid, displacement_px=distance, over_4_px=distance > 4,
                           predicted_x=forecast['actor']['x'], predicted_y=forecast['actor']['y'],
                           committed_x=actor['pos'][0], committed_y=actor['pos'][1],
                           preview_line=forecast['_line'], canonical_dump_line=line, commit_observed_wall_ms=wall,
                           remote_commands_same_frame=commands[tick], remote_cause_proven=False,
                           cause='remote command in same frame; causality unproven' if commands[tick] else 'no same-frame remote command recorded'))
    return result, missing, commands_complete


def max_in_window(values, width):
    values = sorted(values)
    left, maximum = 0, 0
    for right, value in enumerate(values):
        while value - values[left] >= width:
            left += 1
        maximum = max(maximum, right - left + 1)
    return maximum


def firing_records(inputs, previews, frames, stdout_path):
    voices = []
    for number, text in enumerate(stdout_path.read_text(encoding='utf-8-sig', errors='replace').splitlines(), 1):
        match = VOICE.match(text)
        if match:
            values = [int(match[i]) for i in range(1, 8)]
            voices.append(dict(zip(('committed', 'tick', 'uid', 'previewed', 'asset', 'preset', 'seq'), values),
                               name=match[8], path=match[9], line=number))
    presses = [edge for edge in inputs if any(change['action'] == 'FIRE' and change['held'] for change in edge['changes'])]
    result = []
    for index, press in enumerate(presses):
        end_ms = presses[index + 1]['wall_ms'] if index + 1 < len(presses) else math.inf
        candidates = [row for row in previews if row['actor']['uid'] == press['actor']['uid']
                      and row['actor']['fired'] and press['wall_ms'] <= row['wall_upper_ms'] < end_ms]
        first = min(candidates, key=lambda row: row['wall_upper_ms'], default=None)
        row = dict(input_line=press['_line'], tick=press['tick'], uid=press['actor']['uid'],
                   firing_ms_upper=None, audio_ms_upper=None, presented_firing_ms=None,
                   preview_tick=False, once=False, voices=[], duplicate_voices=[],
                   observation_end_ms=min(end_ms, frames[-1]['present_end_ms']) if frames else press['wall_ms'],
                   right_censored=first is None)
        if first:
            matches = [voice for voice in voices if voice['uid'] in (first['actor']['uid'], first['actor'].get('gun_uid'))
                       and abs(voice['tick'] - first['target_tick']) <= 1]
            early = [voice for voice in matches if voice['committed'] == first['committed_tick']]
            identities = Counter((voice['preset'], voice['seq'], voice['path']) for voice in matches)
            duplicate = [list(key) for key, count in identities.items() if count > 1]
            visible = next((frame for frame in frames if frame['draw_begin_ms'] >= press['wall_ms']
                            and frame['draw_begin_ms'] < end_ms
                            and any(actor['uid'] == press['actor']['uid'] and actor['fired'] for actor in frame['actors'])), None)
            row.update(preview_line=first['_line'], event_tick=first['target_tick'],
                       firing_ms_upper=first['wall_upper_ms'] - press['wall_ms'],
                       firing_ms_lower=max(0, first['wall_lower_ms'] - press['wall_ms']),
                       audio_ms_upper=first['wall_upper_ms'] - press['wall_ms'] if early else None,
                       preview_tick=first['committed_tick'] == press['tick'] and bool(early),
                       once=bool(early) and not duplicate, voices=matches, duplicate_voices=duplicate,
                       presented_firing_ms=visible['present_end_ms'] - press['wall_ms'] if visible else None)
        result.append(row)
    return result


def reduce_peer(run, peer, baseline=None):
    run = Path(run)
    raw = run / peer / 'feel/raw.jsonl'
    rows = list(read_jsonl(raw))
    if len([row for row in rows if row['type'] == 'schema' and row['version'] == 1]) != 1:
        raise ValueError(f'{raw}: missing or invalid schema')
    all_frames = [row for row in rows if row['type'] == 'frame']
    if [row['frame'] for row in all_frames] != list(range(1, len(all_frames) + 1)):
        raise ValueError(f'{raw}: missing or reordered frame')
    frames = [row for row in all_frames if row['active'] and 0 < row['tick'] <= TICKS]
    inputs = [row for row in rows if row['type'] == 'input' and 0 < row['tick'] <= TICKS]
    previews = [row for row in rows if row['type'] == 'preview' and 0 < row['committed_tick'] <= TICKS]
    committed = [row for row in rows if row['type'] == 'committed']
    all_iterations = [row for row in rows if row['type'] == 'iteration']
    iterations = [row for row in all_iterations if row['active'] and 0 < row['tick'] <= TICKS]
    if not frames or not iterations:
        raise ValueError(f'{raw}: no measured match frames or iterations')
    destination = run / peer / 'analysis'
    destination.mkdir(exist_ok=False)
    latency = input_latencies(inputs, frames)
    warps = warp_records(frames)
    controller = run / f'{peer}_controller.jsonl'
    canonical_dump = run / f'{peer}_trace.json.simdump.txt'
    command_log = run / 'replay-inspect/stdout.log'
    correction_rows, correction_missing, commands_complete = corrections(previews, committed, canonical_dump, command_log, frames[-1]['peer'])
    firing = firing_records(inputs, previews, frames, run / peer / 'stdout.log')
    paths = {name: destination / (name + '.jsonl') for name in ('latencies', 'warps', 'corrections', 'correction-missing', 'firing')}
    for name, values in [('latencies', latency), ('warps', warps), ('corrections', correction_rows),
                         ('correction-missing', correction_missing), ('firing', firing)]:
        write_jsonl(paths[name], values)
    trace = run / f'{peer}_trace.json'
    trace_document = json.loads(trace.read_text(encoding='utf-8-sig'))
    trace_ticks = trace_document['runs'][0]['tick_hashes']
    coverage = [row['tick'] for row in trace_ticks] == list(range(1, TICKS + 1))
    ended = any(row['type'] == 'end' for row in rows)
    draw = distribution([frame['draw_ms'] for frame in frames])
    present = distribution([frame['present_ms'] for frame in frames])
    interval = distribution([frame['interval_ms'] for frame in frames if frame['interval_ms'] is not None])
    for values, field in ((draw, 'draw_ms'), (present, 'present_ms'), (interval, 'interval_ms')):
        values['count_over_50_ms'] = sum(frame[field] is not None and frame[field] > 50 for frame in frames)
    end_records = [row for row in rows if row['type'] == 'end']
    cpu_ms = end_records[-1]['cpu_ms'] - all_iterations[0]['cpu_begin_ms'] if end_records else None
    if all_iterations[0]['cpu_begin_ms'] < 0 or cpu_ms is None or cpu_ms <= 0:
        cpu_ms = None
    pace_last = all_iterations[-1]
    wall_us = pace_last['pace_update_us'] + pace_last['pace_draw_us']
    pace_tps = pace_last['pace_sim_ticks'] * 1e6 / wall_us if wall_us else None
    sim_cost = pace_last['pace_sim_us'] / 1000 / pace_last['pace_sim_ticks'] if pace_last['pace_sim_ticks'] else None
    lp = frames[-1]['local_prediction']
    lp_cost = lp['ms_total'] / lp['previews'] if lp['previews'] else None
    latency_ms = distribution([edge['ms'] for edge in latency if edge['ms'] is not None])
    latency_frames = distribution([edge['frames'] for edge in latency if edge['frames'] is not None])
    delays = sorted({frame['delay'] for frame in frames})
    large = [row for row in correction_rows if row['over_4_px']]
    window_sim = max_in_window([row['tick'] for row in large], 600)
    window_wall = max_in_window([row['commit_observed_wall_ms'] for row in large if row['commit_observed_wall_ms'] is not None], 10000)
    cap = frames[-1]['cap_hz']
    cpu_overhead = (cpu_ms / baseline['cpu_ms'] - 1) * 100 if cpu_ms and baseline and baseline['cpu_ms'] else None
    draw_ratio = draw['p99'] / baseline['draw_ms']['p99'] if baseline and baseline['draw_ms']['p99'] else None
    manifest = json.loads((run / 'manifest.json').read_text(encoding='utf-8'))
    for source in ('input_script', 'input_schedule'):
        if file_record(manifest[source]['path'])['sha256'] != manifest[source]['sha256']:
            raise ValueError(f'{run}: {source} changed after launch')
    lag = manifest['lag_ms']
    schedule = json.loads(Path(manifest['input_schedule']['path']).read_text(encoding='utf-8'))
    expected_inputs = {(row['tick'], row['action'], row['held']) for row in schedule['probes']}
    actual_inputs = {(row['tick'], change['action'], change['held']) for row in inputs for change in row['changes']}
    missing_inputs = sorted(expected_inputs - actual_inputs)
    expected_delay = {0: 0, 100: 4, 200: 7}[lag]
    over_50 = [frame['frame'] for frame in frames if max(frame['draw_ms'], frame['present_ms'], frame['interval_ms'] or 0) > 50]
    captures = [row for row in rows if row['type'] == 'capture']
    capture_missing = sorted(set(range(60, TICKS + 1, 60)) - {row['requested_tick'] for row in captures if row['saved'] and Path(row['path']).is_file()})
    rtts = [row for frame in frames for row in frame['rtt']]
    metrics = dict(cpu_ms=cpu_ms, draw_ms=draw, present_ms=present, frame_interval_ms=interval,
                   cpu_window=dict(first_iteration_tick=all_iterations[0]['tick'], last_iteration_tick=all_iterations[-1]['tick'],
                                   includes_match_stop_drain=True),
                   latency_ms=latency_ms, latency_frames=latency_frames,
                   latency_lower_bounds_ms=distribution([row['latency_lower_bound_ms'] for row in latency]),
                   missing_input_stamps=missing_inputs,
                   latency_edges=len(latency), latency_unreflected=sum(row['ms'] is None for row in latency),
                   firing_presses=len(firing), local_prediction=dict(lp, avg_ms=lp_cost),
                   delays=delays, input_delay_text=frames[-1]['input_delay_text'],
                   rtt_ms=distribution([row['ping_ms'] for row in rtts if row['ping_ms'] > 0]),
                   pace_wall_tps=pace_tps, sim_ms_per_tick=sim_cost,
                   correction_over_4_px=len(large), correction_max_in_10_sim_seconds=window_sim,
                   correction_max_in_10_wall_seconds=window_wall, correction_missing=len(correction_missing),
                   cpu_overhead_percent=cpu_overhead, draw_p99_ratio=draw_ratio,
                   frames_over_50_ms=over_50, warp_frames=len(warps), warp_frames_over_4_px=sum(row['over_4_px'] for row in warps),
                   capture_missing=capture_missing, frame_count=len(frames), cap_hz=cap,
                   effective_hz=(len(frames) - 1) * 1000 / (frames[-1]['present_end_ms'] - frames[0]['present_end_ms']) if len(frames) > 1 else None)
    measured = bool(ended and coverage and latency and not missing_inputs and len(firing) == schedule['fire_presses'] and cpu_ms is not None
                    and not correction_missing and not capture_missing and (commands_complete or peer == 'sp'))
    pins = {}
    pins['wall_tps'] = pin(pace_tps, '>= 59.0', pace_tps is not None and pace_tps >= 59, [raw])
    pins['sim_ms_per_tick'] = pin(sim_cost, '<= 8 ms', sim_cost is not None and sim_cost <= 8, [raw])
    pins['auto_delay'] = pin(delays, f'D = {expected_delay}', delays == [expected_delay], [raw])
    latency_value = dict(observed_ms=latency_ms, observed_frames=latency_frames,
                         lower_bounds_ms=metrics['latency_lower_bounds_ms'], unreflected=metrics['latency_unreflected'])
    pins['input_to_photon'] = pin(latency_value, '<= 34 ms at 60 Hz; <= one 60 Hz sim tick + one actual frame when uncapped',
                                  bool(latency) and all(row['pass_check'] for row in latency), [raw, paths['latencies']],
                                  'First submitted render copy with aim closer to the scripted aim, or velocity changed in the requested direction; swap-return boundary.')
    pins['preview_ms'] = pin(lp_cost, 'ms_total / previews <= 2 ms at D <= 7', lp_cost is not None and max(delays) <= 7 and lp_cost <= 2, [raw])
    pins['violations'] = pin(max(frame['local_prediction']['violations'] for frame in frames), '= 0 always',
                             all(frame['local_prediction']['violations'] == 0 for frame in frames), [raw])
    pins['correction_remote_only'] = pin(len(large), '> 4 px only on a remote-caused event',
                                         not correction_missing and bool(correction_rows) and not large,
                                         [raw, canonical_dump, controller, command_log, paths['corrections']],
                                         'Same-frame remote commands are listed; coincidence alone never proves causality.')
    pins['correction_rate'] = pin(dict(per_10_sim_seconds=window_sim, per_10_wall_seconds=window_wall), '<= 1 in every 10 s window',
                                  bool(correction_rows) and not correction_missing and max(window_sim, window_wall) <= 1, [raw, paths['corrections']])
    response = distribution([max(row['audio_ms_upper'], row['firing_ms_upper']) for row in firing if row['audio_ms_upper'] is not None])
    response['unobserved'] = sum(row['audio_ms_upper'] is None for row in firing)
    response['unobserved_lower_bounds_ms'] = [max(0, row['observation_end_ms'] - inputs_by_line['wall_ms'])
                                             for row in firing if row['audio_ms_upper'] is None
                                             for inputs_by_line in inputs if inputs_by_line['_line'] == row['input_line']]
    pins['firing_audio_ms'] = pin(response, '<= 34 ms after each press', bool(firing) and all(
        row['audio_ms_upper'] is not None and max(row['firing_ms_upper'], row['audio_ms_upper']) <= 34 for row in firing),
        [raw, run / peer / 'stdout.log', paths['firing']], 'Conservative preview-step wall interval; playback is muted on the private desktop.')
    pins['firing_audio_once'] = pin(sum(row['once'] for row in firing), f'exactly once for each of {len(firing)} presses',
                                    bool(firing) and all(row['once'] for row in firing), [run / peer / 'stdout.log', paths['firing']])
    pins['firing_preview_tick'] = pin(sum(row['preview_tick'] for row in firing), f'at the input preview tick for all {len(firing)} presses',
                                      bool(firing) and all(row['preview_tick'] for row in firing), [raw, paths['firing']])
    pins['draw_p99'] = pin(draw_ratio, '<= 1.5 x local SP D=0 p99', draw_ratio is not None and draw_ratio <= 1.5,
                           [raw, baseline['raw_path']] if baseline else [raw])
    pins['frame_max'] = pin(dict(max_ms=max(draw['max'], present['max'], interval['max'] or 0), count_over_50_ms=len(over_50)),
                            'no frame > 50 ms', not over_50, [raw])
    pins['cpu'] = pin(cpu_overhead, '<= 15 percent process CPU time over the same-cap D=0 SP match',
                      cpu_overhead is not None and cpu_overhead <= 15, [raw, baseline['raw_path']] if baseline else [raw])
    if peer == 'sp':
        measured = bool(ended and coverage and cpu_ms is not None and not capture_missing)
        pins = {name: pins[name] for name in ('auto_delay', 'violations', 'frame_max')}
    result = dict(peer=peer, raw_path=str(raw), metrics=metrics, pins=pins, measurement_complete=measured,
                  trace_1200_ticks=coverage, orderly_end_record=ended, analysis={key: str(value) for key, value in paths.items()})
    write_json(destination / 'metrics.json', result)
    return result


def file_record(path):
    path = Path(path)
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    return dict(path=str(path), bytes=path.stat().st_size, sha256=digest.hexdigest())
