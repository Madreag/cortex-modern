"""Score a lead-routed relay match and its deliberate two-joiner leave.
Missing receipts are failures. Deadlines use lockstep frames and local monotonic clocks.
No engine/network access. Measured physics never silently waives a threshold.
"""
from pathlib import Path
import argparse
import importlib.util
import json
import re
import statistics

import sys
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import frame_pace  # noqa: E402
spec = importlib.util.spec_from_file_location('timeline', Path(__file__).with_name('live-timeline.py'))
timeline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(timeline)
ROLES = ('ally', 'linux', 'mac')
MIN_PLAY_TPS = 59.9
SIM_TICK_MS = 1000 / 60
BLOCK_BOUND_MS = 50
BLACKOUT_POLICY = ('The host loses its peer link and the directory together. During blackout a survivor blocks at most once, at most 50 ms, '
                   'and never on two consecutive ticks; a block is a wait for another seat\'s input, as the engine records it '
                   '([net-frame-wait] frame= wait_ms=). A frame interval over 50 ms with no such wait is the box\'s own frame and is kept as '
                   'evidence, as pauses outside blackout are.')
FRAME_WAIT = re.compile(r'^\[net-frame-wait\] frame=(\d+) wait_ms=(\d+)', re.M)
RATE_POLICY = 'Full in-play and each consecutive 60-second window: at least 59.9 tps; median of 60-tick average intervals: within 1 ms of 1000/60 ms. Raw gaps and sliding-window rates remain evidence.'
LEAVE_POLICY = 'An ordinary Leave is noted within 10 seconds and retains its AI seat; only the host End Match ends this round, within 10 seconds of that request. Silent-peer timing remains enabled and bounded.'
BUSY_RATIO = 4
BOX_POLICY = (f'Each box plays alone: in every 600-tick window after the first, the engine\'s fixed per-tick harness work keeps its 95th percentile '
              f'within {BUSY_RATIO} times its median. A box that took the CPU from the engine fails the run as not a proof; its pauses stay failures.')


def round_listing(report, text):
    runner = report.get('runner', {})
    if 'directory_at_end' in runner:
        return runner['directory_at_end'], 'round snapshot'
    directory = report.get('directory', {})
    if directory.get('confirmed_listed') is True:
        return directory, 'retained directory'
    # A replaced coordinator cannot report its predecessor's listing. Replay
    # the client's acknowledged listing transitions up to the recorded end.
    summary = report.get('last_match') or {}
    lock = runner.get('lockstep', {})
    if summary.get('running_ticks', 0) <= 0 or lock.get('next_frame') != 0 or lock.get('completed_simulation_tick') is not None:
        return {}, 'missing round snapshot'
    listed, session_id = False, ''
    for line in text.splitlines():
        if line.startswith('[net-peer-departure] all-remote-ended'):
            end = re.search(r'applied=(\d+)', line)
            if not end or int(end[1]) != summary['running_ticks']:
                return {}, 'listing end differs from completed summary'
            return {'confirmed_listed': listed, 'session_id': session_id}, 'ordered registration log before round end'
        match = re.fullmatch(r'\[net-directory\] registered session_id=(\S+) heartbeat_s=(\d+)', line)
        if match:
            listed, session_id = True, match[1]
        elif line.startswith('[net-directory] state:') and any(state in line for state in ('-> deleting', '-> failed', '-> disabled', '-> superseded')):
            listed = False
    return {}, 'missing listing end marker'


def own_seat_bridges(text, peer):
    """The round's agreed bridges of one seat from its log, as [first frame, return frame) intervals; None marks one still open."""
    bit = 1 << (peer - 1)
    intervals = []
    for line in text.splitlines():
        match = re.match(r'\[net-frame-group\] (bridge|return) frame=(\d+) members=(\d+)', line)
        if not match:
            continue
        kind, frame, members = match[1], int(match[2]), int(match[3])
        if kind == 'bridge' and not members & bit and (not intervals or intervals[-1][1] is not None):
            intervals.append([frame, None])
        elif kind == 'return' and members & bit and intervals and intervals[-1][1] is None:
            intervals[-1][1] = frame
    return intervals


def box_load(text):
    """The engine's fixed per-tick harness work by 600-tick window; the first window holds the round's start and stays evidence only."""
    windows = [dict(zip(('tick', 'p50_us', 'p95_us', 'max_us'), map(int, w))) for w in
               re.findall(r'\[harness-cost\] tick=(\d+) window=600 tick_end_us p50=(\d+) p95=(\d+) max=(\d+)', text)]
    for window in windows:
        window['ratio'] = round(window['p95_us'] / max(window['p50_us'], 1), 2)
    return {'windows': windows, 'busy_windows': [w for w in windows[1:] if w['ratio'] > BUSY_RATIO]}


def score(roots):
    errors, details, commits, applies, indexes, exits, logs, reports = [], {}, {}, {}, {}, {}, {}, {}

    def require(ok, why):
        if not ok: errors.append(why)

    def read_json(path, role):
        try:
            value = json.loads(path.read_text(encoding='utf-8-sig'))
            if not isinstance(value, dict): raise ValueError('expected an object')
            return value
        except (OSError, ValueError) as exc:
            errors.append(f'{role}: {path.name}: {type(exc).__name__}: {exc}')
            return {}

    def read_text(path, role):
        try: return path.read_text(encoding='utf-8-sig', errors='replace')
        except OSError as exc:
            errors.append(f'{role}: {path.name}: {type(exc).__name__}: {exc}')
            return ''

    placement = re.compile(r'brain placement (committed|applied): seat=(\d+).*? frame=(\d+) unix_ms=(\d+)')
    for role in ROLES:
        root = roots[role]
        result = read_json(root / 'participant-result.json', role)
        require(result.get('pass_') is True and result.get('exit_code') == 0 and not result.get('timed_out'), role + ': participant/probe/exit failed')
        probe = read_json(root / 'inputs/net-ui-result.json', role)
        require(probe.get('pass') is True and probe.get('complete') is True, role + ': incomplete probe')
        steps = probe.get('script', {}).get('steps', [])
        require(1 <= len(steps) <= 256, role + ': invalid probe length')
        require(sum(s.get('seat_ready') == 'local' and s.get('brains_ready') == 3 and s.get('within_ms') == 5000 for s in steps) == 1,
                role + ': missing five-second all-brain confirmation oracle')
        require(any(s.get('op') == 'assert' and s.get('equals', {}).get('ai_held_peer_ids') == [] for s in steps), role + ': probe never checks human return')
        report = read_json(root / 'engine/match-report.json', role)
        reports[role] = report
        lock = report.get('runner', {}).get('lockstep', {})
        require(bool(lock), role + ': missing retained round lockstep report')
        if lock:
            require(lock.get('timeouts') == 0, role + ': lockstep timeout')
            require(lock.get('host_peer_id') == 1 and lock.get('election_generation') == 0, role + ': unexpected host change')
            # rejoins counts committed seat reclaims, including automatic returns.
            # Prepared ownership in an older report may be ahead of the display.
            # Both are retained as diagnostics; live ownership is scored below.
        text = read_text(root / 'engine/stdout.log', role)
        logs[role] = text
        require('reason=Connection dropped' not in text, role + ': transport connection dropped during play')
        require('reliable bytes buffered' not in text, role + ': reliable receive buffer overflow')
        # Ended routes disappear from the native connection table. The original
        # measured connection receipts retain their selected routes after close.
        routes = re.findall(r'\[net-ice\] connected connection=(\d+) after_ms=(\d+) route=(\S+)', text)
        require(len({connection for connection, _, _ in routes}) >= 2, role + ': fewer than two established peer routes')
        require(bool(routes) and all(route == 'relay' for _, _, route in routes), role + ': established non-relay or missing route')
        require(all(c.get('route') in ('', 'relay') for c in report.get('connections', [])), role + ': a connection selected a non-relay route')
        if role == 'ally':
            directory, listing_source = round_listing(report, text)
            require(directory.get('confirmed_listed') is True, role + ': no confirmed public listing at round end')
        applies[role] = {}
        for kind, seat, frame, unix in placement.findall(text):
            seat = int(seat)
            table = commits if kind == 'committed' else applies[role]
            require(seat not in table, role + ': duplicate ' + kind + ' placement ' + str(seat))
            table[seat] = {'frame': int(frame), 'unix_ms': int(unix), 'role': role}
        require(set(applies[role]) == {0, 1, 2}, role + ': missing applied brain placements')
        exit_marks = re.findall(r'\[menu-script\] exit unix_ms=(\d+) lockstep_frame=(\d+) peer=(\d+)', text)
        require(len(exit_marks) == 1, role + ': missing unique engine exit request timestamp')
        if len(exit_marks) == 1:
            unix, frame, peer = map(int, exit_marks[0]); exits[role] = {'unix_ms': unix, 'frame': frame, 'peer': peer}
        launch = read_json(root / 'relay-launch.json', role)
        require(launch.get('leave_test', {}).get('tick_cap') == 0, role + ': deliberate leave was replaced by a tick-limited round end')
        require(launch.get('leave_test', {}).get('path') == 'pause-menu-leave', role + ': missing deliberate pause-menu leave plan')
        path_samples = [{k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)', line)} for line in text.splitlines() if line.startswith('[net-peer-path]')]
        physical = {}
        for key in ('transport_rtt_ms', 'receipt_p95_ms', 'input_delay_ticks'):
            values = [p[key] for p in path_samples if p.get(key, -1) >= 0]
            physical[key] = {'count': len(values), 'min': min(values), 'median': statistics.median(values), 'max': max(values)} if values else None
        detail = {'participant': result, 'report_present': bool(report), 'placements': applies[role], 'routes': routes,
                  'path_samples': path_samples, 'path_summary': physical, 'lockstep': lock}
        details[role] = detail
        # The match draws as single player does: every 60-tick window at the presentation's rate, as evenly (frame_pace.py).
        # The host blackout and the ten seconds allowed for the return after it are the blackout policy's to judge.
        drawn = detail['drawn_frames'] = frame_pace.check(text, 'lockstep', exclude=[(600, 1081 + 600 + 60)])
        require(drawn['status'] != 'NONE', role + ': no drawn-frame windows in the match')
        require(drawn['status'] == 'NONE' or drawn['passed'], role + ': ' + str(drawn.get('reason')))
        # Work beside a participant (a build, another engine) takes its CPU, and its pauses then measure the box, not the round.
        load = detail['box_load'] = box_load(text)
        require(not load['busy_windows'], f"{role}: box was not alone during the match (harness tick-end p95 over {BUSY_RATIO}x its median "
                                          f"in {len(load['busy_windows'])} of {max(len(load['windows']) - 1, 0)} windows)")
        if role == 'ally':
            detail.update(directory_at_end=directory, listing_source=listing_source,
                          completed_report_retained='directory_at_end' in report.get('runner', {}))
        try:
            rows, timing = timeline.summarize(root / 'engine/live.jsonl')
            index = {r['tick']: r for r in rows}; indexes[role] = index
            detail.update(peer=rows[0]['peer'], timing=timing)
            require(rows[0]['peer'] in (1, 2, 3) and (role != 'ally' or rows[0]['peer'] == 1), role + ': wrong local peer')
            require(timing['span_s'] + 1 / 60 >= 200, role + ': less than 200 seconds of full-match ticks')
            require(600 in index and index[600]['paused'] is False, role + ': blackout did not begin in play at tick 600')
            tps = timing['in_play_tps']
            truncated = result.get('timed_out') or result.get('exit_code') != 0 or not report
            detail['rate_status'] = 'unscored_truncated' if truncated else 'complete'
            # A killed run already fails participation. Its cut-off endpoint
            # cannot establish a full-match pacing verdict.
            if not truncated:
                require(tps is not None and tps >= MIN_PLAY_TPS, role + ': full-match in-play rate below 59.9 tps')
                windows = timing['consecutive_60s']
                require(bool(windows) and all(window['tps'] >= MIN_PLAY_TPS for window in windows),
                        role + ': consecutive 60-second in-play window below 59.9 tps')
                median = timing['median_60_tick_interval_ms']
                require(median is not None and abs(median - SIM_TICK_MS) <= 1,
                        role + ': median 60-tick average interval differs from the sim tick by more than 1 ms')
            blackout_gaps = [g for g in timing['gaps_over_50ms'] if 600 <= g['ending_tick'] <= 1081 and g['in_play']]
            steady_gaps = [g for g in timing['gaps_over_50ms'] if not 600 <= g['ending_tick'] <= 1081 and g['gap_ms'] > 100 and g['in_play']]
            # A block is the engine's own wait for another seat's input; a long frame with no wait is this box's frame, not the spike.
            blocks = [{'frame': int(frame), 'block_ms': int(waited)} for frame, waited in FRAME_WAIT.findall(text) if 600 <= int(frame) <= 1081]
            waited_frames = {b['frame'] for b in blocks}
            frame_stalls = [g for g in blackout_gaps if g['ending_tick'] not in waited_frames]
            detail.update(blackout_gaps=blackout_gaps, blackout_blocks=blocks, blackout_frame_stalls=frame_stalls, steady_gaps_over_100ms=steady_gaps)
            if role != 'ally':
                # A survivor may block once per spike, up to the bound, before the silent seat is held.
                require(all(b['block_ms'] <= BLOCK_BOUND_MS for b in blocks), role + ': unaffected peer blocked over 50 ms during blackout')
                require(not any(b['frame'] == a['frame'] + 1 for a, b in zip(blocks, blocks[1:])),
                        role + ': unaffected peer blocked on two consecutive ticks during blackout')
                require(len(blocks) <= 1, role + ': unaffected peer blocked more than once during blackout')
            # The absent player's private catch-up remains measured separately.
            first_return_tick = next((r['tick'] for n, r in enumerate(rows) if n and 1 in rows[n-1].get('ai_held_peer_ids', []) and 1 not in r.get('ai_held_peer_ids', [])), None)
            recovery_gaps = [g for g in steady_gaps if role == 'ally' and first_return_tick and 1081 < g['ending_tick'] < first_return_tick]
            steady_gaps = [g for g in steady_gaps if g not in recovery_gaps]
            detail.update(private_recovery_gaps=recovery_gaps, steady_gaps_over_100ms=steady_gaps)
            require(all('ai_held_peer_ids' in r and 'net_clock_ms' in r for r in rows), role + ': missing per-tick ownership or local monotonic evidence')
            require(all('input_accepted_through' in r and 'input_delay_ticks' in r for r in rows), role + ': missing per-tick input progress evidence')
            active = [r for r in rows if not r.get('paused') and r['tick'] < 12000]
            delays = [r.get('input_delay_ticks') for r in active]
            require(all(isinstance(d, int) and 1 <= d <= 36 for d in delays), role + ': input delay outside the 1..36 tick bound')
            require(all(set(r.get('ai_held_peer_ids', [])) <= {1} for r in active), role + ': unintended healthy seat hold')
            detail['input_delay_bound_ticks'] = 36
            first = next((n for n, r in enumerate(rows) if 1 in r.get('ai_held_peer_ids', [])), None)
            require(first is not None, role + ': no observed host AI takeover')
            returned = next((n for n, r in enumerate(rows) if first is not None and n > first and 1 not in r.get('ai_held_peer_ids', [1])), None)
            require(returned is not None, role + ': host never returned from AI')
            if returned is not None:
                detail['return'] = {k: rows[returned].get(k) for k in ('tick', 'wall_ms', 'net_clock_ms')}
                held_after = [r['tick'] for r in rows[returned:] if 1 in r.get('ai_held_peer_ids', [])]
                detail['held_after_return'] = held_after
                require(not held_after, role + ': host became held again after return')
                # A seat the round bridged has its own input replaced until its return, so acceptance resumes there.
                bridges = [b for b in own_seat_bridges(text, rows[0]['peer']) if b[1] is None or b[1] > rows[returned]['tick']]
                detail['own_seat_bridges_after_return'] = bridges
                require(all(b[1] is not None for b in bridges), role + ': own seat bridged after return without a recorded return')
                bridged = lambda tick: any(b[0] <= tick and (b[1] is None or tick < b[1]) for b in bridges)
                human_rows = [r for r in rows[returned:] if r['tick'] < 12000 and not r.get('paused') and not bridged(r['tick'])]
                require(all(isinstance(r.get('input_accepted_through'), int) and r['input_accepted_through'] >= r['tick'] for r in human_rows),
                        role + ': quorum input acceptance stopped advancing after return')
                detail['input_progress_after_return'] = {'rows': len(human_rows),
                    'first': human_rows[0].get('input_accepted_through') if human_rows else None,
                    'last': human_rows[-1].get('input_accepted_through') if human_rows else None}
        except (OSError, ValueError, KeyError, TypeError, IndexError) as exc:
            errors.append(f'{role}: invalid live timeline: {type(exc).__name__}: {exc}')

    matrix = []
    require(set(commits) == {0, 1, 2}, 'missing local placement commits')
    for seat, commit in commits.items():
        for role, placed in applies.items():
            if seat not in placed: continue
            lag = placed[seat]['frame'] - commit['frame']
            matrix.append({'seat': seat, 'observer': role, 'confirmation_frames': lag, 'confirmation_ms': lag * 1000 / 60, 'commit': commit, 'applied': placed[seat]})
            require(0 <= lag <= 300, f'{role}: seat {seat} confirmation outside 0..300 lockstep frames')
    require(len(matrix) == 9, 'missing nine-peer/seat confirmation matrix')
    require('[net-link-blackout] begin peer=1 frame=600 duration_ms=8000' in logs['ally'], 'ally: wrong blackout interval')
    # A host that still reaches the directory loses only a joiner's link from its side; the host blackout takes both.
    require(re.search(r'^\[net-link-blackout\] begin peer=1 frame=600 duration_ms=8000 .* directory=dark$', logs['ally'], re.M) is not None,
            'ally: blackout left the host reaching the directory')
    ended = re.findall(r'\[net-link-blackout\] end peer=1 elapsed_ms=(\d+) now_ms=(\d+) unix_ms=(\d+)', logs['ally'])
    require(len(ended) == 1, 'ally: missing unique blackout end timestamp')
    if len(ended) == 1:
        duration, end_local, _ = map(int, ended[0])
        require(8000 <= duration <= 8050, 'ally: blackout duration outside 8000..8050 ms')
        for role in ROLES:
            returned = details[role].get('return', {})
            # Anchor the remote observer to the shared fault frame, then use
            # only that observer's clock. The host has its exact local end.
            anchor = indexes.get(role, {}).get(600, {}).get('net_clock_ms')
            end = end_local if role == 'ally' else anchor + duration if anchor is not None else None
            delta = returned.get('net_clock_ms') - end if end is not None and returned.get('net_clock_ms') is not None else None
            details[role]['return_after_blackout_ms'] = delta
            require(delta is not None and 0 <= delta <= 10000, role + ': human return not observed within ten seconds after blackout')
    require(len({details[r].get('peer') for r in ROLES}) == 3, 'participants do not name three distinct peers')

    departures = [{'peer': int(peer), 'frame': int(frame), 'now_ms': int(now), 'unix_ms': int(unix), 'source': source}
                  for peer, frame, now, unix, source in re.findall(r'\[net-peer-departure\] peer=(\d+) frame=(\d+) now_ms=(\d+) unix_ms=(\d+) source=(\S+)', logs['ally'])]
    round_ends = [{'frame': int(frame), 'now_ms': int(now), 'unix_ms': int(unix)} for frame, now, unix in
                  re.findall(r'\[net-peer-departure\] all-remote-ended applied=(\d+) now_ms=(\d+) unix_ms=(\d+)', logs['ally'])]
    leaves = {}
    leave_score = {'exits': exits, 'leaves': leaves, 'departures': departures, 'round_ends': round_ends}
    limit = 10000
    round_silence = {role: details[role].get('lockstep', {}).get('peer_silence_leave_ms') for role in ROLES
                     if isinstance(details[role].get('lockstep', {}).get('completed_simulation_tick'), int) and
                     details[role]['lockstep']['completed_simulation_tick'] >= 12000}
    leave_score['silent_peer_timing_ms'] = round_silence
    require(len(round_silence) >= 2 and all(isinstance(value, int) and 0 < value <= limit for value in round_silence.values()),
            'missing or unbounded silent-peer timing in completed round reports')
    host_rows = indexes.get('ally', {})

    def host_anchor(frame):
        # The last common displayed frame is a conservative start for the
        # host's local deadline, including time spent blocked after that tick.
        preceding = [t for t in host_rows if t <= frame]
        if not preceding: return None
        row = host_rows[max(preceding)]
        return {'frame': row['tick'], 'now_ms': row.get('net_clock_ms')}

    for role in ('linux', 'mac'):
        marks = re.findall(r'\[net-match-leave\] peer=(\d+) frame=(\d+) now_ms=(\d+) reason=([^\n]*)', logs[role])
        marks = [m for m in marks if int(m[0]) == details[role].get('peer') and int(m[1]) > 0]
        require(len(marks) == 1, role + ': missing unique in-match Leave request')
        for gesture in ('open_local_pause', 'activate ButtonLeaveMatch', 'activate ButtonLeaveConfirm'):
            require(any(line.startswith('[menu-script] ' + gesture) and line.endswith('PASS') for line in logs[role].splitlines()), role + ': missing completed real gesture: ' + gesture)
        if len(marks) != 1: continue
        peer, frame, now, reason = marks[0]
        leave = leaves[role] = {'peer': int(peer), 'frame': int(frame), 'now_ms': int(now), 'reason': reason}
        require(leave['frame'] >= 12000, role + ': left before the planned full-match frame')
        require(leave['frame'] in indexes.get(role, {}), role + ': Leave has no displayed frame')
        observed = next((d for d in departures if d['peer'] == leave['peer']), None)
        anchor = host_anchor(leave['frame'])
        delta = observed['now_ms'] - anchor['now_ms'] if observed and anchor and anchor['now_ms'] is not None else None
        earliest = host_anchor(max(1, leave['frame'] - 36))
        # The leaver's last input lands up to the delay bound past the leave; the round notes the first frame without it.
        delay = indexes.get(role, {}).get(leave['frame'], {}).get('input_delay_ticks')
        notice = (observed and observed['source'] == 'round-notice' and
                  leave['frame'] <= observed['frame'] <= leave['frame'] + 36 + 1 and
                  earliest and earliest['now_ms'] is not None and observed['now_ms'] >= earliest['now_ms'])
        leave.update(host_anchor=anchor, notice_ms=delta, host_note=observed, input_delay_ticks=delay)
        # A future input stop may arrive before the host displays the matching
        # leave tick. Keep that signed interval and test the causal frame range.
        require(notice and delta is not None and delta <= limit, 'ally: ' + role + ' leave not noted within ten seconds')
        seats = reports['ally'].get('reconnect', {}).get('moderation', [])
        seat = next((seat for seat in seats if seat.get('lockstep_peer_id') == leave['peer']), {})
        retained = (seat.get('committed') is True and seat.get('dropped') is True and seat.get('held_for_reclaim') is True and
                    seat.get('closed') is False and seat.get('cpu') is False and seat.get('holder_generation', 0) > 0)
        leave['retained_seat'] = seat
        require(retained, 'ally: ' + role + ' leave did not retain its AI seat')
        if observed:
            following = [row for tick, row in host_rows.items() if tick >= observed['frame']]
            held = next((row['tick'] for row in following if leave['peer'] in row.get('ai_held_peer_ids', [])), None)
            leave.update(ai_hold_frame=held, end_before_hold_boundary=not following)
            require(not following or (held is not None and held <= observed['frame'] + 3 and
                    all(leave['peer'] in row.get('ai_held_peer_ids', []) for row in following if row['tick'] >= held)),
                    'ally: ' + role + ' departed seat not held within three ticks of its boundary')
    if len(leaves) == 2:
        requests = [{'frame': int(frame), 'from_frame': int(before)} for frame, before in
                    re.findall(r'\[net-match\] the host ends the round at frame (\d+), playing to it from (\d+)', logs['ally'])]
        for gesture in ('open_local_pause', 'activate ButtonEndMatch', 'activate ButtonLeaveConfirm'):
            require(sum(line.startswith('[menu-script] ' + gesture) and line.endswith('PASS')
                        for line in logs['ally'].splitlines()) == 1, 'ally: missing unique host End Match gesture: ' + gesture)
        require(len(requests) == 1, 'ally: missing unique host End Match request')
        request = requests[0] if len(requests) == 1 else {}
        host_lines = logs['ally'].splitlines()
        end_position = next((index for index, line in enumerate(host_lines) if line.startswith('[net-match] the host ends the round')), -1)
        last_notice = max((index for index, line in enumerate(host_lines) if line.startswith('[net-peer-departure] peer=')), default=-1)
        terminal_position = next((index for index, line in enumerate(host_lines) if line.startswith('[net-peer-departure] all-remote-ended')), -1)
        require(last_notice >= 0 and last_notice < end_position < terminal_position,
                'ally: End Match and completion did not follow both leave notices')
        anchor = host_anchor(request.get('from_frame', 0) - 1)
        ended_cleanly = (anchor is not None and anchor['now_ms'] is not None and
                         any(0 <= end['now_ms'] - anchor['now_ms'] <= limit for end in round_ends))
        leave_score.update(end_match=request, end_anchor=anchor, ended_cleanly=bool(ended_cleanly))
        require(ended_cleanly, 'ally: host did not complete within ten seconds of End Match')
        require((reports['ally'].get('last_match') or {}).get('result') == 'Match ended by host',
                'ally: completed summary does not name the host End Match')
        require('ally' in exits and any('[menu-script] round-ended -> OK' in line or '[menu-script] connected:1 -> OK' in line for line in logs['ally'].splitlines()), 'ally: host exit did not follow both joiner departures')
    eligible = {role: {tick: row for tick, row in index.items()
                      if role not in leaves or tick < leaves[role]['frame']}
                for role, index in indexes.items()}
    common = sorted(set.intersection(*(set(eligible[r]) for r in ROLES))) if len(eligible) == 3 else []
    compared, mismatch, coverage = [], [], []
    for tick in sorted(set().union(*(set(index) for index in eligible.values()))):
        observers = [role for role in ROLES if tick in eligible.get(role, {})]
        if len(observers) < 2: continue
        compared.append(tick)
        if len({eligible[role][tick]['total'] for role in observers}) != 1: mismatch.append(tick)
        if coverage and coverage[-1]['roles'] == observers and coverage[-1]['last_tick'] + 1 == tick:
            coverage[-1]['last_tick'] = tick
            coverage[-1]['ticks'] += 1
        else:
            coverage.append({'first_tick': tick, 'last_tick': tick, 'ticks': 1, 'roles': observers})
    exclusions = {}
    for role, leave in leaves.items():
        omitted = sorted(tick for tick in indexes.get(role, {}) if tick >= leave['frame'])
        exclusions[role] = {'leave_frame': leave['frame'], 'rows': len(omitted),
                            'first_tick': omitted[0] if omitted else None, 'last_tick': omitted[-1] if omitted else None}
    require(len(common) >= 12000, 'insufficient common full-match ticks for 200 seconds')
    require(not mismatch, 'total hash mismatch')
    return {'pass': not errors, 'errors': errors, 'placement_confirmations': matrix, 'commits': commits,
            'common_ticks': len(common), 'compared_ticks': len(compared), 'hash_mismatches': mismatch,
            'hash_coverage': coverage, 'hash_exclusions': exclusions, 'leave_test': leave_score, 'details': details,
            'rate_policy': RATE_POLICY,
            'leave_policy': LEAVE_POLICY, 'box_policy': BOX_POLICY,
            'physics_policy': BLACKOUT_POLICY}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for role in ROLES: parser.add_argument('--' + role, type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if args.out.exists(): parser.error('Retain earlier proof receipts; --out must be fresh')
    result = score({r: getattr(args, r) for r in ROLES})
    args.out.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: result[k] for k in ('pass', 'errors', 'common_ticks')}))
    return int(not result['pass'])


if __name__ == '__main__': raise SystemExit(main())
