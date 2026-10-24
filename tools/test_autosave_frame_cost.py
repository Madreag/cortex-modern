"""A save that is not this machine's own overdue one costs its player no frames (one machine, two engines, proof:false).

A hosted two-peer match at the host's default automatic input delay saves every --autosave-seconds. Every per-second
[frame-pace] window of each peer is placed by its ticks: no save running (idle), inside one of this peer's saves whose own
deadline had not yet passed (save), inside a span this peer's saver logged as overdue (overdue: the late machine may pay
for its own save there; on a build that logs none, the part of a save that ran past the next deadline), or, on the host,
from a returning seat's base being asked to its being published or no longer waited for - the returner loaded an image,
dropped or was refused (base). A base window is a base window whatever the saver logged: the base's own write is part of
what the return costs the host.
--client-rejoin TICK drops the client once its game reaches TICK and starts it again with its reconnect ticket, so it
returns through the host's image and the host asks for its base. The restarted client's join waits at its door
(CC_TEST_JOIN_GATE_FILE) until the host holds the dropped seat and has passed the tick of a capture it named after that and
has not yet written; the host then asks for the base while its writer is busy with that save, on any box whose save outlasts
a join's handshake. The return judged is the one the kill caused: the first the host's log records after the kill.

For each peer and kind the result gives the median frame rate, the 95th percentile of the per-window worst frame interval,
the median tick rate and the seat holds whose frame falls there. The verdict: each peer's ordinary saves (in a rejoin run
the host's alone, the client being replaced) and the host's frames while a base waits or is written keep at least 95 % of
the idle frame rate and at most 125 % of the idle worst frame interval, and the host keeps its idle tick rate within
0.5 ticks/s while a base waits; in a rejoin run the returner also catches up with no refusal of its base
(tools/rejoin_stage_check.py) and on the link its return came on, which nothing on one box drops. Every span a peer's saver logs as overdue needs a cause this driver finds in the log on its
own: a deadline that passed while that peer's own capture was unwritten, a periodic save named late for that peer's own
writer (the save written last before its naming is the peer's own and ends at it), or a manual save; a base is no cause.
--base names a base build's result.json (or its run folder), as often as there are base runs; each base run's logs are
placed again by this driver's rules. The judged kinds are then
held to the base instead of to their own idle windows: a kind's frame rate and worst frame are taken as ratios to its own
run's idle windows, and the tip's ratio keeps at least 95 % of the base runs' mean frame-rate ratio and at most 125 % of
their mean worst-frame ratio; while a base waits the host's tick rate stays within 0.5 ticks/s of the base's and its own
seat is held no more often. The verdict against the run's own idle windows is reported beside it as in_run_checks.
--split-cpus puts the host on one half of the logical CPUs and the client on the other, so neither engine's saver or
startup takes the other's time, as on two machines.
--saves-after-return N (with --client-rejoin) plays on until the host has named N periodic captures after the returner
caught up: the returned seat writes every one of them with no failed save, and its game keeps ticking at least 600 ticks
past the last; a returner whose game stops for 60 s while the host plays on ends the run at once.
--judge RUN places and judges a finished run of this driver again by these rules, without starting an engine; the verdict
goes to --out/judged.json.
"""
import argparse
from datetime import datetime, timezone
import json
import os
import platform
from pathlib import Path
import re
import statistics
import subprocess
import sys
import tempfile
import time
import unittest


def fields(line):
    return {key: value for key, value in re.findall(r'(\w+)=(-?[0-9.]+)\b', line)}


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(fraction * len(ordered)))]


def timeline(text, host):
    """The peer's frame windows, saves, deadlines, bases, holds, the spans its own saver logged as overdue (None when the log
    has no such line) and the events that can cause one, in its log's order."""
    windows, saves, dues, bases, holds = [], [], [], [], []
    events = {'base_ticks': [], 'namings': [], 'manual': []}
    latest = 0
    asked = None
    saver, raised = None, None
    for line in text.splitlines():
        if match := re.match(r'^\[autosave\] saver overdue=([01]) tick=(\d+)', line):
            saver = saver if saver is not None else []
            if match.group(1) == '1':
                raised = int(match.group(2))
            elif raised is not None:
                saver.append((raised, int(match.group(2))))
                raised = None
            continue
        if line.startswith('[harness-cost-frame] '):
            # The exact frame the game had reached when the next line printed.
            if match := re.search(r'"frame":(\d+)', line):
                latest = max(latest, int(match.group(1)))
            continue
        if line.startswith('[frame-pace] '):
            f = fields(line)
            if 'first_tick' in f and 'tick' in f and float(f.get('wall_ms', 0)) > 0:
                window = {'first': int(f['first_tick']), 'last': int(f['tick']), 'fps': float(f['fps']),
                          'worst_ms': float(f['interval_max_ms']), 'tps': int(f.get('ticks', 0)) * 1000.0 / float(f['wall_ms'])}
                windows.append(window)
                latest = max(latest, window['last'])
        elif match := re.match(r'^\[autosave\] tick=(\d+) capture_ms=', line):
            # Printed when the save is written: its window runs from its boundary to the tick the game had reached.
            saves.append((int(match.group(1)), max(latest, int(match.group(1)))))
        elif host and (match := re.match(r'^\[autosave\] periodic due named at=(\d+) late_ticks=(-?\d+)(?:.*? own_writer=([01]))?', line)):
            dues.append(int(match.group(1)) - int(match.group(2)))
            events['namings'].append((int(match.group(1)), match.group(3) == '1', dues[-1]))
        elif match := re.match(r'^\[autosave\] manual save (?:named|taken|saved|failed) tick=(\d+)', line):
            events['manual'].append(int(match.group(1)))
        elif host and (match := re.search(r'private base asked of the checkpoint schedule at tick (\d+)', line)):
            asked = int(match.group(1))
        elif host and asked is not None and (match := re.search(r'private base taken at the announced tick (\d+)', line)):
            # The base's own write is part of what its returner costs: the span runs on until the base is published.
            events['base_ticks'].append(int(match.group(1)))
        elif host and asked is not None and re.search(r'private base published from the announced capture at tick \d+|'
                                                      r'^\[roster\] rev=\d+ seat=\d+ (?:ImageLoaded|LinkDropped) REJOIN_IMAGE->|^\[net-match\] rejoin refused ', line):
            # Nobody waits on that base any more: it was published, the returner loaded an image, dropped, or was refused.
            bases.append((asked, max(latest, asked)))
            asked = None
        elif match := re.match(r'^\[net-peer-hold-installed\] local=(\d+) peer=(\d+) frame=(\d+)', line):
            holds.append({'local': int(match.group(1)), 'peer': int(match.group(2)), 'frame': int(match.group(3))})
    if asked is not None:
        bases.append((asked, latest))
    if saver is not None and raised is not None:
        saver.append((raised, latest))
    return windows, saves, dues, bases, holds, saver, events


def classify(windows, saves, dues, bases, warmup, saver=None):
    """Places each window: base, overdue, save, idle, or edge (half in a save) and warmup, which are not judged. The overdue
    spans are the ones the peer's saver logged; a build that logs none has them inferred: the part of a save that ran past
    the next deadline."""
    overdue = list(saver) if saver is not None else []
    for start, end in saves if saver is None else []:
        later = [due for due in dues if due > start]
        if later and end > later[0]:
            overdue.append((later[0], end))
    kinds = []
    for w in windows:
        def overlaps(span):
            return w['first'] <= span[1] and w['last'] >= span[0]
        if w['first'] < warmup:
            kind = 'warmup'
        elif any(overlaps(span) for span in bases):
            kind = 'base'
        elif any(overlaps(span) for span in overdue):
            kind = 'overdue'
        elif any(w['first'] >= start and w['last'] <= end for start, end in saves):
            kind = 'save'
        elif any(overlaps(span) for span in saves):
            kind = 'edge'
        else:
            kind = 'idle'
        kinds.append(kind)
    return kinds, overdue


def uncaused(saver, saves, dues, events, interval=0, near=30, slack=120):
    """The spans the peer's saver logged as overdue that no cause found in the log explains: a deadline that passed while the
    peer's own capture was unwritten (a written save's tick is known to within a frame-pace window), a periodic save named
    late for the peer's own writer - the save written last before the naming is the peer's own and ends at it - or a
    manual save. A base is another player's save and causes none, unless the late own naming named it. The deadlines are
    the namings' and, with the interval known, the cadence's: each repeats every interval, and a base starts its own."""
    if saver is None:
        return []
    bases = set(events['base_ticks'])
    own = [(start, end) for start, end in saves if start not in bases]
    deadlines = set(dues)
    if interval > 0:
        last = max([end for _, end in saves] + [end for _, end in saver] + list(dues) + [0])
        for first in list(dues) + [base + interval - 4 for base in bases]:
            deadlines.update(range(first, last + interval, interval))
    found = []
    for start, end in saver:
        at_deadline = any(due - 1 <= start <= due + near and any(s0 <= due + near and e0 + 60 >= due and end <= e0 + slack for s0, e0 in own)
                          for due in deadlines)
        own_late = False
        for named, own_writer, due in events['namings']:
            if not own_writer:
                continue
            # The naming's own-late claim, checked: the save written last before it is this peer's own and ended just before it.
            before = [save for save in saves if save[1] <= named + 60 and save[0] < named]
            last = max(before, key=lambda save: save[1]) if before else None
            if not last or last[0] in bases or named - last[1] > slack:
                continue
            written = [e1 for s1, e1 in saves if named <= s1 <= named + near]
            waited = due - 1 <= start and end <= named + 2
            wrote = named - 2 <= start <= named + near and written and end <= written[0] + slack
            own_late = own_late or waited or bool(wrote)
        manual = any(start - 600 <= tick <= end + 600 for tick in events['manual'])
        if not (at_deadline or own_late or manual):
            found.append((start, end))
    return found


def summary(windows, kinds, holds, local):
    out = {}
    for kind in ('idle', 'save', 'overdue', 'base', 'edge', 'warmup'):
        chosen = [w for w, k in zip(windows, kinds) if k == kind]
        in_kind = [h for h in holds if h['local'] == local and any(w['first'] <= h['frame'] <= w['last'] for w in chosen)]
        out[kind] = {'windows': len(chosen),
                     'fps_median': round(statistics.median([w['fps'] for w in chosen]), 2) if chosen else None,
                     'worst_ms_p95': round(percentile([w['worst_ms'] for w in chosen], 0.95), 2) if chosen else None,
                     'tps_median': round(statistics.median([w['tps'] for w in chosen]), 2) if chosen else None,
                     'holds': len(in_kind), 'own_seat_holds': sum(1 for h in in_kind if h['peer'] == local)}
    return out


def keeps(kind, idle, minimum):
    """The kind's frames against the idle ones: at least 95 % of the frame rate, at most 125 % of the worst frame interval."""
    if kind['windows'] < minimum or idle['windows'] < 10:
        return {'enough_windows': False}
    return {'enough_windows': True, 'keeps_frame_rate': kind['fps_median'] >= 0.95 * idle['fps_median'],
            'keeps_worst_frame': kind['worst_ms_p95'] <= 1.25 * idle['worst_ms_p95']}


def ratios(kind, idle):
    """The kind's frame rate and worst frame interval as ratios to its own run's idle windows."""
    return kind['fps_median'] / idle['fps_median'], kind['worst_ms_p95'] / idle['worst_ms_p95']


def log_text(run_dir, role):
    path = Path(run_dir) / role / 'stdout.log'
    return path.read_text(errors='replace') if path.exists() else ''


def analyze(run_dir, warmup, interval=0):
    """Each peer's windows placed and summarized from a run's logs; the client follows the host's agreed deadlines."""
    host_dues = timeline(log_text(run_dir, 'host'), True)[2]
    peers = {}
    for local, role in ((1, 'host'), (2, 'client')):
        windows, saves, _, bases, holds, saver, events = timeline(log_text(run_dir, role), role == 'host')
        kinds, overdue = classify(windows, saves, host_dues, bases, warmup, saver)
        peers[role] = {'kinds': summary(windows, kinds, holds, local), 'saves': saves, 'overdue_spans': overdue, 'bases': bases,
                       'logs_overdue': saver is not None, 'uncaused_overdue_spans': uncaused(saver, saves, host_dues, events, interval)}
        idle = peers[role]['kinds']['idle']
        if idle['windows']:
            peers[role]['ratios_to_idle'] = {k: [round(r, 3) for r in ratios(v, idle)] for k, v in peers[role]['kinds'].items() if v['windows']}
    return peers


def equals_base(role, name, kind, idle, bases, minimum):
    """The kind against the base runs' same kind: its ratios to idle keep 95 % of their frame-rate ratio and 125 % of their
    worst-frame ratio; while a base waits the tick rate stays within 0.5 ticks/s of theirs and the own seat is held no more."""
    reference = [b['peers'][role]['kinds'] for b in bases if role in b['peers']]
    reference = [k for k in reference if k[name]['windows'] >= minimum and k['idle']['windows'] >= 10]
    if kind['windows'] < minimum or idle['windows'] < 10 or not reference:
        return {'enough_windows': False}
    fps, worst = ratios(kind, idle)
    base_fps = statistics.mean(ratios(k[name], k['idle'])[0] for k in reference)
    base_worst = statistics.mean(ratios(k[name], k['idle'])[1] for k in reference)
    out = {'enough_windows': True, 'frame_rate_equal_to_base': fps >= 0.95 * base_fps, 'worst_frame_equal_to_base': worst <= 1.25 * base_worst}
    if name == 'base':
        out['tick_rate_equal_to_base'] = kind['tps_median'] >= statistics.mean(k[name]['tps_median'] for k in reference) - 0.5
        out['own_seat_holds_equal_to_base'] = kind['own_seat_holds'] <= max(k[name]['own_seat_holds'] for k in reference)
    return out


def named_ticks(host_text):
    return [int(t) for t in re.findall(r'^\[autosave\] named tick=(\d+) at=\d+ writers=\d+', host_text, re.M)]


def written_ticks(text):
    return [int(t) for t in re.findall(r'^\[autosave\] tick=(\d+) capture_ms=', text, re.M)]


def last_tick(text):
    ticks_seen = re.findall(r'^\[frame-pace\] .*? tick=(\d+) ', text, re.M)
    return int(ticks_seen[-1]) if ticks_seen else 0


HELD = re.compile(r'^\[roster\] rev=\d+ seat=\d+ HeldInPlace ', re.M)
RETURNED = re.compile(r'^\[roster\] rev=\d+ seat=\d+ Returned HELD->REJOIN_IMAGE', re.M)
CAUGHT_UP = re.compile(r'^\[roster\] rev=\d+ seat=\d+ CaughtUp REJOIN_CATCHUP->RUNNING', re.M)
REFUSED = re.compile(r'^\[net-match\] rejoin refused connection=', re.M)
LOST_RETURN = re.compile(r'^\[roster\] rev=\d+ seat=\d+ LinkDropped REJOIN_(?:IMAGE|CATCHUP)->', re.M)
NAMED = re.compile(r'^\[autosave\] named tick=(\d+) at=\d+ writers=\d+', re.M)


def reached(text):
    # The newest frame the log shows its game reached: its per-frame cost line, else its last frame-pace window.
    frames = re.findall(r'^\[harness-cost-frame\] .*?"frame":(\d+)', text[-65536:], re.M)
    return max(int(frames[-1]) if frames else 0, last_tick(text))


def offset_at_tick(host_text, tick):
    """Where the host's log stood at its first frame-pace window that reached the tick: the kill's place for a run recorded
    before the driver kept the log's offset at the kill."""
    for match in re.finditer(r'^\[frame-pace\] .*? tick=(\d+) ', host_text, re.M):
        if int(match.group(1)) >= tick:
            return match.start()
    return len(host_text)


def kill_return(host_text, at):
    """The return the kill caused: the host's first seat Returned after its log's place at the kill, and the first CaughtUp
    after that. A return that ended before the kill is not it."""
    returned = RETURNED.search(host_text, at)
    return returned, (CAUGHT_UP.search(host_text, returned.end()) if returned else None)


def named_after_return(host_text, at):
    # The captures the host named after the seat the kill dropped was back.
    back = kill_return(host_text, at)[1]
    return named_ticks(host_text[back.end():]) if back else []


def release_save(host_text, at):
    """The capture the restarted client's join goes into: once the host holds the seat the kill dropped, a capture it named after
    that, whose tick its game has passed and which its writer has not yet written; None until there is one."""
    held = HELD.search(host_text, at)
    if not held:
        return None
    written = set(written_ticks(host_text))
    unwritten = [int(match.group(1)) for match in NAMED.finditer(host_text, held.end()) if int(match.group(1)) not in written]
    return unwritten[0] if unwritten and reached(host_text) >= unwritten[0] else None


def judge(out, cfg, references, warmup, failure, exits_ok, kill=None):
    """The run's checks from its logs: the placed windows against idle or the base runs, the logged overdue spans' causes,
    the captures, refusals, the return and the returned seat's saves. `kill` is the host log's offset and tick at the kill: the
    base asked while the writer was busy and the saves after the return are the kill's own return's."""
    rejoin, after_count = cfg.get('client_rejoin', 0), cfg.get('saves_after_return', 0)
    at, kill_tick = (kill['offset'], kill['host_tick']) if kill else (0, 0)
    judged, in_run = {}, {}
    peers = analyze(out, warmup, 60 * cfg.get('autosave_seconds', 0))
    for role in ('host', 'client'):
        idle, save, base = (peers[role]['kinds'][k] for k in ('idle', 'save', 'base'))
        saves, bases = peers[role]['saves'], peers[role]['bases']
        if not rejoin or role == 'host':
            in_run[role] = {f'save_{name}': value for name, value in keeps(save, idle, 5).items()}
            if references:
                judged[role] = {f'save_{name}': value for name, value in equals_base(role, 'save', save, idle, references, 5).items()}
        if rejoin and role == 'host':
            busy = any(any(start <= asked <= end for start, end in saves) for asked, _ in bases if asked >= kill_tick)
            in_run[role].update({'base_asked_while_writer_busy': busy, **{f'base_{name}': value for name, value in keeps(base, idle, 3).items()}})
            if base['windows'] >= 3:
                in_run[role]['base_keeps_tick_rate'] = base['tps_median'] >= idle['tps_median'] - 0.5
            if references:
                judged[role].update({'base_asked_while_writer_busy': busy, **{f'base_{name}': value for name, value in equals_base(role, 'base', base, idle, references, 3).items()}})
        if not references and role in in_run:
            judged[role] = in_run[role]
        if peers[role]['logs_overdue']:
            judged.setdefault(role, {})['overdue_spans_have_causes'] = not peers[role]['uncaused_overdue_spans']
    host_text = log_text(out, 'host')
    refused = {role: re.findall(r'^\[autosave\] (?:named tick=\d+ not taken: .*|failed .*)$', log_text(out, role), re.M) for role in ('host', 'client')}
    captures = named_ticks(host_text)
    checks = {'no_driver_failure': failure is None,
              'all_exit_zero': exits_ok,
              'periodic_captures_taken': len(captures) >= cfg.get('periodic_captures', 0),
              'no_capture_refused': not refused['host'] and (bool(rejoin) or not refused['client']),
              **{f'{role}_{name}': value for role, items in judged.items() for name, value in items.items()}}
    if rejoin:
        # The returner waits for its base and comes back; a base on its way is never refused.
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from rejoin_stage_check import check as stage_check
        stage = stage_check(host_text)
        checks['returner_caught_up_unrefused'] = stage['status'] == 'PASS' and stage['returns_caught_up'] > 0
        # Nothing on one box drops a returning link: one lost while the host made it wait for its base was refused in silence.
        returned, back = kill_return(host_text, at)
        lost = returned and LOST_RETURN.search(host_text, returned.end(), back.start() if back else len(host_text))
        checks['returner_back_on_the_link_it_came_on'] = bool(back) and not lost
    returned = None
    if after_count:
        # The returned seat saves every capture named after it is back, refuses none, and its game goes on past the last.
        returner_text = log_text(out, 'client-rejoined')
        after = named_after_return(host_text, at)[:after_count]
        written = set(written_ticks(returner_text))
        failed = re.findall(r'^\[autosave\] failed .*$', returner_text, re.M)
        returned = {'named_after_return': after, 'written': sorted(written & set(after)), 'failed': failed,
                    'returner_last_tick': last_tick(returner_text), 'host_last_tick': last_tick(host_text)}
        checks['returner_writes_saves_after_return'] = len(after) == after_count and set(after) <= written and not failed
        checks['returner_keeps_ticking'] = bool(after) and returned['returner_last_tick'] >= after[-1] + 600
    in_run_checks = {f'{role}_{name}': value for role, items in in_run.items() for name, value in items.items()} if references else None
    return {'pass': all(checks.values()), 'checks': checks, 'in_run_checks': in_run_checks, 'captures': captures,
            'returned_seat': returned, 'peers': peers, 'refused': refused}


def load_references(paths, warmup):
    references = []
    for path in paths:
        found = sorted(path.resolve().rglob('result.json')) if path.is_dir() else [path.resolve()]
        if not found or not (found[0].parent / 'host' / 'stdout.log').exists():
            raise SystemExit(f'no base run at {path}')
        # The base run's logs are placed again with this driver's own rules, never read from its stored summary.
        references.append({'path': str(found[0]), **json.loads(found[0].read_text()), 'peers': analyze(found[0].parent, warmup)})
    return references


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repo', type=Path)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--autosave-seconds', type=int, default=90)
    p.add_argument('--periodic-captures', type=int, default=5)
    p.add_argument('--client-rejoin', type=int, default=0, help='the client drops once its game reaches this tick and rejoins')
    p.add_argument('--saves-after-return', type=int, default=0, help='periodic captures the returned seat must write after it caught up')
    p.add_argument('--warmup-ticks', type=int, default=1200)
    p.add_argument('--limit-seconds', type=int, default=1500)
    p.add_argument('--port', type=int, default=49492)
    p.add_argument('--base', type=Path, action='append', default=[], help="a base build's result.json or run folder; repeat per base run")
    p.add_argument('--split-cpus', action='store_true',
                   help="the host on one half of the logical CPUs and the client, rejoined too, on the other, as on two machines")
    p.add_argument('--judge', type=Path, help='a finished run folder of this driver to place and judge again, without engines')
    p.add_argument('--engine-env', action='append', default=[], help='NAME=VALUE given to every engine, a test lever')
    a = p.parse_args()
    if a.saves_after_return and not a.client_rejoin:
        raise SystemExit('--saves-after-return needs --client-rejoin')
    references = load_references(a.base, a.warmup_ticks)
    if a.judge:
        run_dir = a.judge.resolve()
        recorded = json.loads((run_dir / 'result.json').read_text())
        cfg = json.loads((run_dir / 'configuration.json').read_text())
        exits_ok = all(code == 0 for role, code in recorded.get('exits', {}).items() if role != 'client-dropped')
        kill = None
        if recorded.get('rejoined_at_host_tick') is not None:
            offset = recorded.get('host_log_offset_at_kill')
            if offset is None:
                offset = offset_at_tick(log_text(run_dir, 'host'), recorded['rejoined_at_host_tick'])
            kill = {'offset': offset, 'host_tick': recorded['rejoined_at_host_tick']}
        verdict = judge(run_dir, cfg, references, a.warmup_ticks, recorded.get('failure'), exits_ok, kill)
        verdict = {'judged_run': str(run_dir), 'recorded_pass': recorded.get('pass'), **verdict,
                   'bases': [{'path': b['path'], 'exe_sha256': b['exe_sha256'], 'source_head': b['source_head']} for b in references]}
        a.out.mkdir(parents=True, exist_ok=True)
        (a.out / 'judged.json').write_text(json.dumps(verdict, indent=2))
        print(json.dumps({'pass': verdict['pass'], 'checks': verdict['checks'],
                          'uncaused_overdue_spans': {role: verdict['peers'][role]['uncaused_overdue_spans'] for role in verdict['peers']}}))
        return 0 if verdict['pass'] else 1
    if a.repo is None:
        raise SystemExit('--repo is needed to run a match')
    repo = a.repo.resolve()
    sys.path.insert(0, str(repo / 'tools'))
    from run_sim_test import make_run, seed_settings, engine_executable, file_sha256
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out / 'driver-as-run.py').write_bytes(Path(__file__).read_bytes())
    quit_path = out / 'quit.json'
    menu = out / 'menu.txt'
    menu.write_text(f'in_play\nwait_file {quit_path.as_posix()} {a.limit_seconds + 120}\nexit\n')
    ticks = str(60 * (a.limit_seconds + 120))
    common = ['-net-port', str(a.port), '-net-match-peers', '2', '-net-match-auto-delay',
              '-net-autosave-seconds', str(a.autosave_seconds), '-net-match-ticks', ticks, '-max-ticks', ticks,
              '-menu-script', str(menu), '-input-script', str(repo / 'tools/e2e/play-input.txt'),
              '-net-match-service-preset', 'Determinism FeelBaseline', '-net-match-service-module', 'UserScenes.rte']
    argv = {'host': ['-net-match-service-e2e', '-net-host', '-net-match-report', str(out / 'host-report.json'),
                     '-net-reconnect-ticket', str(out / 'host.ticket')] + common,
            'client': ['-net-match-service-e2e', '-net-join', '127.0.0.1', '-net-match-report', str(out / 'client-report.json'),
                       '-net-reconnect-ticket', str(out / 'client.ticket')] + common}
    env = {'CCCP_HEADLESS': '1', 'CC_RUNNER_IGNORE_FULLSCREEN': '1', 'CC_TEST_NET_MATCH_E2E_WAIT_PEERS': '1', **dict(item.split('=', 1) for item in a.engine_env)}
    half = (os.cpu_count() or 2) // 2
    masks = {'host': (1 << half) - 1, 'client': ((1 << half) - 1) << half} if a.split_cpus else {}
    settings = {'AutosaveSeconds': '0', 'NetworkHostDelayPolicy': 'Auto', 'NetworkInputDelayFrames': '0', 'NetworkIceEnable': '0',
                'NetworkPortMapEnable': '0', 'NetworkHostVisibility': 'Unlisted', 'EnableLuaDebugging': '0',
                'NetworkAutoReconnect': '1', 'NetworkOfferStoredRejoin': '1',
                'ResolutionX': '960', 'ResolutionY': '540', 'EnableVSync': '0'}
    def probe_env(name):
        # Each engine leaves when the driver has seen its captures; every engine keeps its probe's results apart.
        probe_dir = out / f'{name}-probe'
        probe_dir.mkdir()
        (probe_dir / 'probe.json').write_text(json.dumps({'schema': 1, 'timeout_ms': (a.limit_seconds + 300) * 1000,
                                                          'steps': [{'op': 'wait_file', 'path': str(quit_path)}, {'op': 'finish', 'quit': True}]}, indent=2))
        mask = masks.get('host' if name == 'host' else 'client')
        return {**env, 'CC_TEST_NET_UI_SCRIPT': str(probe_dir / 'probe.json'), **({'CC_RUNNER_AFFINITY_MASK': f'0x{mask:x}'} if mask else {})}

    runs, records, started = {}, {}, []
    for role in ('host', 'client'):
        runs[role] = make_run(repo, argv[role], out / role, a.limit_seconds + 600, env=probe_env(role))
        seed_settings(runs[role], settings)
        fixture = Path(runs[role].cwd) / 'Userdata/UserScenes.rte'
        fixture.mkdir()
        source = (repo / 'tools/feel/FeelBaseline.lua').read_text()
        source, count = re.subn(r'^local WINDOW_TICKS = \d+;$', f'local WINDOW_TICKS = {ticks};', source, flags=re.M)
        assert count == 1
        (fixture / 'FeelBaseline.lua').write_text(source)
        (fixture / 'Index.ini').write_text('DataModule\n\tModuleName = User Scenes\n\tScanFolderContents = 1\n\tIgnoreMissingItems = 1\n'
            '\tAddActivity = GAScripted\n\t\tCopyOf = P4 Alpha Duel\n\t\tPresetName = Determinism FeelBaseline\n'
            '\t\tScriptPath = UserScenes.rte/FeelBaseline.lua\n\t\tLuaClassName = FeelBaseline\n'
            '\t\tTeamOfPlayer1 = 0\n\t\tPlayer1IsHuman = 1\n\t\tTeamOfPlayer2 = 1\n\t\tPlayer2IsHuman = 1\n')
    config = {'topology': 'single-box', 'proof': False, 'box': platform.node(), 'autosave_seconds': a.autosave_seconds,
              'periodic_captures': a.periodic_captures, 'client_rejoin': a.client_rejoin, 'saves_after_return': a.saves_after_return,
              'input_delay': 'auto', 'settings': settings,
              'environment': env, 'affinity_masks': {role: f'0x{mask:x}' for role, mask in masks.items()}, 'source_head': subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip(),
              'exe_sha256': file_sha256(engine_executable(repo)), 'started_utc': datetime.now(timezone.utc).isoformat(),
              'bases': [{'path': b['path'], 'exe_sha256': b['exe_sha256'], 'source_head': b['source_head']} for b in references]}
    (out / 'configuration.json').write_text(json.dumps(config, indent=2))

    def log(role):
        return log_text(out, role)

    def written(role):
        return len(written_ticks(log(role)))

    def returner_done(host):
        after = named_after_return(host, kill['offset'])[:a.saves_after_return]
        return (len(after) == a.saves_after_return and set(after) <= set(written_ticks(log('client-rejoined'))) and
                last_tick(log('client-rejoined')) >= after[-1] + 600)

    failure = None
    rejoined_at = None
    # The host's log offset and tick at the kill, and the capture the returner's join was released into.
    kill, released = None, None
    gate = out / 'client-rejoined-join.gate'
    returner_progress = (0, time.monotonic())
    began = time.monotonic()
    try:
        for role in ('host', 'client'):
            runs[role].start()
            started.append(role)
        until = time.monotonic() + a.limit_seconds
        while True:
            if a.client_rejoin and rejoined_at is None and last_tick(log('client')) >= a.client_rejoin:
                offset = len(log('host'))
                runs['client'].terminate(reason=f'the client drops at tick {a.client_rejoin}')
                records['client-dropped'] = runs['client'].finish()
                runs['client-dropped'] = runs.pop('client')
                rejoined_at = last_tick(log('host'))
                kill = {'offset': offset, 'host_tick': rejoined_at}
                # The returner loads at once and waits at its door until its join is released into a save.
                runs['client'] = make_run(repo, argv['client'], out / 'client-rejoined', a.limit_seconds + 600,
                                          env={**probe_env('client-rejoined'), 'CC_TEST_JOIN_GATE_FILE': str(gate)},
                                          runtime=Path(runs['client-dropped'].cwd))
                runs['client'].start()
            host = log('host')
            if kill and released is None and (save := release_save(host, kill['offset'])) is not None:
                released = {'save': save, 'host_frame': reached(host)}
                gate.write_text(json.dumps(released))
            back = kill_return(host, kill['offset'])[1] if kill else None
            # A rejoin run lasts until its base is published and the returner is back or refused.
            rejoin_pending = bool(a.client_rejoin) and (kill is None or not re.compile('private base taken').search(host, kill['offset']) or
                                                        not (back or REFUSED.search(host, kill['offset'])))
            if a.saves_after_return and back:
                # A returned seat whose game stops while the host plays on is the defect this mode looks for.
                tick = last_tick(log('client-rejoined'))
                if tick > returner_progress[0]:
                    returner_progress = (tick, time.monotonic())
                elif tick and time.monotonic() - returner_progress[1] > 60 and last_tick(host) > tick + 600:
                    raise RuntimeError(f'the returned seat stopped ticking at tick {tick} while the host reached {last_tick(host)}')
            after_pending = bool(a.saves_after_return) and not (kill and returner_done(host))
            captures = named_ticks(host)
            if len(captures) >= a.periodic_captures and len(written_ticks(host)) >= len(captures) and not rejoin_pending and not after_pending:
                break
            if time.monotonic() > until:
                raise RuntimeError(f'{len(captures)} captures named, {written("host")} written by the host in {a.limit_seconds} s')
            if any(runs[role].poll() is not None for role in ('host', 'client')):
                raise RuntimeError('an engine ended before the captures')
            # The release into a save is timed to its frames; everything else to the second.
            time.sleep(0.1 if kill and released is None else 1)
    except Exception as error:
        failure = repr(error)
    finally:
        quit_path.write_text(json.dumps({'requested_utc': datetime.now(timezone.utc).isoformat()}))
        for role in ('host', 'client'):
            if role in runs and role not in records:
                records[role] = runs[role].finish()
        for run in runs.values():
            run.close()
    exits_ok = all(r.get('exit_code') == 0 and not r.get('timed_out') for role, r in records.items() if role != 'client-dropped')
    verdict = judge(out, config, references, a.warmup_ticks, failure, exits_ok, kill)
    result = {**config, **verdict, 'failure': failure, 'wall_seconds': round(time.monotonic() - began, 1), 'rejoined_at_host_tick': rejoined_at,
              'host_log_offset_at_kill': kill['offset'] if kill else None, 'returner_released_into': released,
              'exits': {role: r.get('exit_code') for role, r in records.items()}}
    (out / 'result.json').write_text(json.dumps(result, indent=2))
    print(json.dumps({'pass': result['pass'], 'checks': result['checks'], 'failure': failure,
                      'kinds': {role: verdict['peers'][role]['kinds'] for role in verdict['peers']}}))
    return 0 if result['pass'] else 1


def write_return_after_return(run_dir, lost_link=False):
    """A rejoin run's logs where an earlier return caught up before the kill: its base was asked inside a save, two captures
    were named after it, and the kill's own return then came back with its base asked outside any save; with `lost_link` that
    return's first link drops while it waits and a second one brings it back. Returns the result the driver records (the
    kill's host tick and the host log's offset at it)."""
    def pace(first, last):
        return f'[frame-pace] mode=lockstep first_tick={first} tick={last} frames=60 wall_ms=1000.0 fps=60.00 interval_max_ms=17.0 ticks=60\n'
    host = [pace(tick - 59, tick) for tick in range(60, 601, 60)]
    host += ['[roster] rev=5 seat=2 Returned HELD->REJOIN_IMAGE: Rejoining - waiting for the host recovery boundary\n',
             '[autosave] named tick=640 at=637 writers=1\n', pace(601, 660),
             '[net-match] private base asked of the checkpoint schedule at tick 650\n', pace(661, 720),
             '[autosave] tick=640 capture_ms=1.0 bytes=1\n',
             '[net-match] private base taken at the announced tick 700; its archive follows from the writer\n',
             '[net-match] private base published from the announced capture at tick 700 bytes=1\n',
             '[roster] rev=7 seat=2 CaughtUp REJOIN_CATCHUP->RUNNING: the player is back\n']
    for tick in (1000, 2000):
        host += [pace(tick - 59, tick), f'[autosave] named tick={tick} at={tick - 3} writers=2\n', f'[autosave] tick={tick} capture_ms=1.0 bytes=1\n']
    host += [pace(2441, 2500)]
    offset = len(''.join(host))
    host += ['[roster] rev=8 seat=2 LinkDropped RUNNING->RUNNING: Connection lost\n', pace(2501, 2560),
             '[roster] rev=9 seat=2 HeldInPlace RUNNING->HELD: Connection lost\n', pace(2561, 2620),
             '[roster] rev=10 seat=2 Returned HELD->REJOIN_IMAGE: Rejoining - waiting for the host recovery boundary\n']
    if lost_link:
        host += ['[roster] rev=11 seat=2 LinkDropped REJOIN_IMAGE->HELD: Connection lost\n',
                 '[roster] rev=12 seat=2 Returned HELD->REJOIN_IMAGE: Rejoining - waiting for the host recovery boundary\n']
    host += ['[net-match] private base asked of the checkpoint schedule at tick 2630\n',
             '[autosave] named tick=2634 at=2631 writers=1\n', pace(2621, 2680),
             '[net-match] private base taken at the announced tick 2634; its archive follows from the writer\n',
             '[autosave] tick=2634 capture_ms=1.0 bytes=1\n',
             '[net-match] private base published from the announced capture at tick 2634 bytes=1\n',
             '[roster] rev=13 seat=2 CaughtUp REJOIN_CATCHUP->RUNNING: the player is back\n']
    for tick in (3000, 4000, 5000):
        host += [pace(tick - 59, tick), f'[autosave] named tick={tick} at={tick - 3} writers=2\n', f'[autosave] tick={tick} capture_ms=1.0 bytes=1\n']
    host += [pace(5641, 5700)]
    returner = [pace(2700, 2759)] + [f'[autosave] tick={tick} capture_ms=1.0 bytes=1\n' for tick in (3000, 4000, 5000)] + [pace(5641, 5700)]
    for role, lines in (('host', host), ('client', [pace(1, 60)]), ('client-rejoined', returner)):
        (run_dir / role).mkdir(parents=True)
        (run_dir / role / 'stdout.log').write_text(''.join(lines))
    cfg = {'client_rejoin': 2480, 'saves_after_return': 3, 'periodic_captures': 3, 'autosave_seconds': 0}
    result = {'rejoined_at_host_tick': 2500, 'host_log_offset_at_kill': offset, 'exits': {'host': 0, 'client': 0}, 'failure': None, 'pass': None}
    (run_dir / 'configuration.json').write_text(json.dumps(cfg))
    (run_dir / 'result.json').write_text(json.dumps(result))
    return cfg, result


class KillReturnTests(unittest.TestCase):
    """The return judged is the one the kill caused, and the restarted client's join is released into a save the host's writer
    has not yet written."""

    def judged(self, recorded_offset=True, lost_link=False):
        with tempfile.TemporaryDirectory() as directory:
            run_dir = Path(directory)
            cfg, result = write_return_after_return(run_dir, lost_link)
            offset = result['host_log_offset_at_kill'] if recorded_offset else offset_at_tick(log_text(run_dir, 'host'), result['rejoined_at_host_tick'])
            return judge(run_dir, cfg, [], 0, None, True, {'offset': offset, 'host_tick': result['rejoined_at_host_tick']})

    def test_the_saves_after_return_are_those_after_the_kills_own_return(self):
        for recorded in (True, False):
            with self.subTest(recorded_offset=recorded):
                verdict = self.judged(recorded)
                self.assertEqual(verdict['returned_seat']['named_after_return'], [3000, 4000, 5000])
                self.assertTrue(verdict['checks']['returner_writes_saves_after_return'], verdict['returned_seat'])

    def test_a_return_whose_link_dropped_while_it_waited_is_red(self):
        self.assertTrue(self.judged()['checks']['returner_back_on_the_link_it_came_on'])
        lost = self.judged(lost_link=True)['checks']
        self.assertFalse(lost['returner_back_on_the_link_it_came_on'])
        self.assertTrue(lost['returner_writes_saves_after_return'], 'the second link still brought the seat back')

    def test_a_busy_writer_counts_only_for_the_kills_own_base(self):
        # The earlier return's base was asked inside save 640; the kill's own base was asked with the writer idle.
        self.assertFalse(self.judged()['checks']['host_base_asked_while_writer_busy'])

    def test_the_join_is_released_into_a_save_its_writer_has_not_written(self):
        held = '[roster] rev=9 seat=2 HeldInPlace RUNNING->HELD: Connection lost\n'
        frame = '[harness-cost-frame] {{"costs_ms":{{}},"frame":{}}}\n'
        named = '[autosave] named tick=6000 at=5997 writers=1\n'
        written = '[autosave] tick=6000 capture_ms=1.0 bytes=1\n'
        self.assertIsNone(release_save(named + frame.format(6001), 0), 'released before the seat was held')
        self.assertIsNone(release_save(held + named + frame.format(5998), 0), 'released before the capture tick')
        self.assertEqual(release_save(held + named + frame.format(6000), 0), 6000)
        self.assertIsNone(release_save(held + named + frame.format(6100) + written, 0), 'released into a save already written')
        self.assertEqual(release_save(held + named + written + named.replace('6000', '11400').replace('5997', '11397') + frame.format(11401), 0), 11400)


if __name__ == '__main__':
    raise SystemExit(main())
