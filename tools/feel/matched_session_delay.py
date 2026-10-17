"""Compare local match response with predeclared, matched single-player repeats."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from feel import session_delay as driver


def grouped_responses(peer):
    groups = {}
    for edge in peer['input_edges']:
        key = f"{edge['reference']['team']}:{edge['action']}:{int(edge['held'])}"
        groups.setdefault(key, []).append(edge['ms'])
    return {key: statistics.median(values) if all(v is not None for v in values) else None
            for key, values in groups.items()}


def freeze_envelope(cases):
    actions = {}
    aggregate = {0: [], 1: []}
    frames = {0: [], 1: []}
    for case in cases:
        assert case['arm'] == 'sp' and len(case['peers']) == 1
        peer = case['peers'][0]
        if not peer['b3_coverage_pass']:
            raise ValueError('SP response coverage missing; keep the observation RED')
        team = case['baseline_team']
        observed_teams = {edge['reference']['team'] for edge in peer['input_edges']}
        if observed_teams != {team}:
            raise ValueError('the single-player control did not drive the requested actor team')
        aggregate[team].append(peer['latency_ms']['p50'])
        frames[team].append(peer['latency_frames']['p50'])
        for key, value in grouped_responses(peer).items():
            if value is None:
                raise ValueError('SP response missing; keep the observation RED')
            actions.setdefault(key, []).append(value)
    return dict(
        actions={key: dict(min_ms=min(values), max_ms=max(values), observations=values)
                 for key, values in actions.items()},
        aggregate={team: dict(min_ms=min(values), max_ms=max(values), observations=values)
                   for team, values in aggregate.items()},
        frames={team: dict(min_frames=min(values), max_frames=max(values), observations=values)
                for team, values in frames.items()})


def judge_peer(peer, envelope):
    teams = {edge['reference']['team'] for edge in peer['input_edges']}
    if len(teams) != 1:
        raise ValueError('mixed actor teams in a peer observation')
    team = teams.pop()
    band = envelope['aggregate'][team]
    frame_band = envelope['frames'][team]
    measured_actions = grouped_responses(peer)
    expected_actions = {key for key in envelope['actions'] if key.startswith(f'{team}:')}
    actions = []
    for key in sorted(expected_actions | measured_actions.keys()):
        base = envelope['actions'].get(key)
        value = measured_actions.get(key)
        actions.append(dict(key=key, p50_ms=value, single_player=base,
                            pass_check=base is not None and value is not None and
                            base['min_ms'] <= value <= base['max_ms']))
    result = dict(peer=peer['peer'], team=team, p50_ms=peer['latency_ms']['p50'], actions=actions,
                  aggregate_pass=band['min_ms'] <= peer['latency_ms']['p50'] <= band['max_ms'],
                  frame_p50=peer['latency_frames']['p50'],
                  frame_pass=frame_band['min_frames'] <= peer['latency_frames']['p50'] <= frame_band['max_frames'],
                  coverage_pass=peer['b3_coverage_pass'], delay_max=peer.get('delay_max'))
    for key in ('b1_delay_pass', 'b2_depth_pass', 'b4_overlay_pass', 'c_duplicates_pass'):
        result[key] = peer.get(key)
    result['pass_check'] = bool(result['coverage_pass'] and result['aggregate_pass'] and result['frame_pass'] and
                                actions and all(action['pass_check'] for action in actions))
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--single-player-repeats', type=int, default=12, help='repeats per actor team, all before network arms')
    parser.add_argument('--network-repeats', type=int, default=3, help='pairs per RTT arm')
    parser.add_argument('--port', type=int, default=47961)
    args = parser.parse_args(argv)
    if args.single_player_repeats < 3 or args.network_repeats < 3:
        parser.error('each arm needs at least three repeats')
    root = args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    os.environ.pop('CCCP_HEADLESS', None)
    os.environ.update(CC_RUNNER_HEADED='1', DISPLAY=':0', PYTHONDONTWRITEBYTECODE='1')
    binary_sha = driver.sha(driver.engine_executable(driver.REPO))
    plan = dict(single_player_repeats_per_team=args.single_player_repeats,
                network_pairs_per_rtt=args.network_repeats, teams=[0, 1], rtts_ms=[30, 200], ticks=driver.TICKS,
                binary_sha256=binary_sha, script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                envelope='Observed SP repeat p50 minimum and maximum, separately for each actor team and action/held state. Software milliseconds and presented-frame counts are retained. No tolerance is added.',
                order='All SP controls are collected first, alternating actor teams. Then each repeat measures both RTT arms.',
                matching='Same fixture, actor team, model, weapon, script, draw cap and resolution. The SP team1 control swaps player-team assignments so player0 drives and draws that seat.',
                scope='One Linux box, official runners, software input sample through completed UploadFrame; no physical scanout or spread claim.')
    driver.write_json(root / 'plan.json', plan)
    original_stage = driver.stage_baseline
    cases = []

    def stage_team_one(run, *stage_args, **stage_kwargs):
        original_stage(run, *stage_args, **stage_kwargs)
        path = Path(run.cwd) / 'Userdata/UserScenes.rte/Index.ini'
        text = path.read_text()
        if text.count('TeamOfPlayer1 = 0') != 1 or text.count('TeamOfPlayer2 = 1') != 1:
            raise ValueError('the SP fixture must have exactly the two expected player-team assignments')
        path.write_text(text.replace('TeamOfPlayer1 = 0', 'TeamOfPlayer1 = 1').replace('TeamOfPlayer2 = 1', 'TeamOfPlayer2 = 0'))

    def collect(arm, repeat, team=None):
        case = driver.launch_case(root, arm, repeat, args.port, binary_sha, [960, 540])
        if team is not None:
            case['baseline_team'] = team
        cases.append(case)
        driver.write_json(root / 'cases.json', cases)
        if not all(case['manifest'][key] for key in ('complete', 'quiet', 'binary_unchanged')):
            raise ValueError('observation incomplete or loaded; keep it RED')

    try:
        for repeat in range(1, args.single_player_repeats + 1):
            for team in (0, 1):
                driver.stage_baseline = stage_team_one if team else original_stage
                collect('sp', 2 * repeat - 1 + team, team)
    finally:
        driver.stage_baseline = original_stage
    envelope = freeze_envelope(cases)
    driver.write_json(root / 'frozen-sp-envelope.json', envelope)
    for repeat in range(1, args.network_repeats + 1):
        for arm in ('30ms', '200ms'):
            collect(arm, repeat)
    judged = []
    for case in cases:
        if case['arm'] == 'sp':
            continue
        for peer in case['peers']:
            judged.append(dict(arm=case['arm'], repeat=case['repeat'], **judge_peer(peer, envelope)))
    medians = {arm: statistics.median(peer['latency_ms']['p50'] for case in cases if case['arm'] == arm for peer in case['peers'])
               for arm in ('sp', '30ms', '200ms')}
    result = dict(plan=plan, medians_ms=medians, single_player_bands=envelope['aggregate'],
                  single_player_frame_bands=envelope['frames'], rows=judged,
                  pass_check=bool(judged) and all(row['pass_check'] for row in judged))
    driver.write_json(root / 'result.json', result)
    print(json.dumps(dict(medians_ms=medians, aggregate_passes=sum(row['aggregate_pass'] for row in judged),
                          observations=len(judged), pass_check=result['pass_check'])), flush=True)
    return 0 if result['pass_check'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
