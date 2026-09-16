"""Record menu-launched matches, browse each peer's own replay, play it and return.

Proves post-match result/duration agreement and distinct before/after captures at
640x360 and 960x540. The retained RED's 960 Guest landed on Landing: the engine's
assert_substate expected=ReplayBrowser actual=Landing FAIL detects that defect.
The default fixture is the retained harness fixture root used by the replay driver;
--fixture overrides it for another checkout or machine. Launches use make_run only.
"""

import argparse
import json
import os
from pathlib import Path
import re
import shutil

import test_post_match_report as report_driver
from test_lobby_chat import read_log
from test_replay_browser import FIXTURE, duration_agreement


def combined_script(base_script, who, port):
    text = base_script(who, port)
    assert text.endswith('wait_ms 3000\nexit\n')
    return text[:-len('exit\n')] + (
        'activate ButtonMultiplayerLeave\nwait 20\nassert_substate Landing\n'
        'activate ButtonMultiplayerReplays\nwait 10\nassert_substate ReplayBrowser\n'
        'assert_control ListReplays\nassert_control ButtonReplayPlay\n'
        'assert_label LabelReplayRow0 01-fixture.ccreplay\n'
        'assert_label LabelReplayRow1 02-match.ccreplay\n'
        'assert_label LabelReplayRow1 P4 Alpha Duel / Grasslands\n'
        'assert_label LabelReplayRow1 2 peers\n'
        'activate LabelReplayRow1\nwait 10\n'
        'assert_label LabelReplaySelected 02-match.ccreplay\n'
        'screenshot report_saved_replay\nactivate ButtonReplayPlay\nwait_ms 5000\n'
        'assert_screen MultiplayerScreen\nassert_substate ReplayBrowser\n'
        'assert_label LabelReplayStatus Playback finished:\n'
        'assert_label LabelReplaySelected 02-match.ccreplay\n'
        'screenshot report_saved_replay_return\nactivate ButtonReplayBack\nwait 10\n'
        'assert_substate Landing\nexit\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--fixture', type=Path, default=FIXTURE)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--port', type=int, default=48215)
    parser.add_argument('--exe-sha256', required=True)
    parser.add_argument('--size', choices=('both', '640x360', '960x540'), default='both')
    options = parser.parse_args()
    if Path('D:/mx/LEAD_FAMILY.lock').exists():
        parser.error('family lock exists; no driver may run')
    sizes = report_driver.SIZES if options.size == 'both' else (tuple(map(int, options.size.split('x'))),)
    if not 1 <= options.port <= 65536 - len(sizes):
        parser.error('one UDP port per size must fit 1-65535')
    repo, output, fixture = options.repo.resolve(), options.out.resolve(), options.fixture.resolve()
    os.environ['CCCP_HEADLESS'] = '1'
    output.mkdir(parents=True, exist_ok=False)
    fixture_hash = report_driver.sha256(fixture)
    base_script, base_run = report_driver.menu_script, report_driver.make_run

    def recording_run(repo, args, out, timeout, **kwargs):
        run = base_run(repo, [*args, '-net-replay-out', 'Userdata/Replays/02-match.ccreplay'], out, timeout, **kwargs)
        directory = run.cwd / 'Userdata/Replays'
        directory.mkdir()
        shutil.copy2(fixture, directory / '01-fixture.ccreplay')
        return run

    results = {}
    report_driver.menu_script = lambda who, port: combined_script(base_script, who, port)
    report_driver.make_run = recording_run
    try:
        for index, size in enumerate(sizes):
            root = output / f'{size[0]}x{size[1]}'
            result = report_driver.run_size(repo, root, size, options.port + index, options.exe_sha256.lower())
            extra = {}
            for who in ('Host', 'Guest'):
                out = root / who
                log = read_log(out)
                completions = re.findall(r'\[net-replay\] playback finished[^\n]*ticks=(\d+)[^\n]*outcome=completed[^\n]*frames=(\d+)[^\n]*end_marker=1', log)
                saved = out / 'runtime/Userdata/Replays/02-match.ccreplay'
                row = next((text for name, text, _ in report_driver.LABELS.findall(log) if name == 'LabelReplayRow1'), '')
                duration = duration_agreement(row, result['details'].get(who, {}).get('summary'))
                checks = {'saved_file': saved.is_file() and saved.stat().st_size > 0,
                          'played_own_match': any('[net-replay] playing back' in line and '02-match.ccreplay' in line for line in log.splitlines()),
                          'completed_playback': len(completions) == 1 and all(int(ticks) >= 60 and int(frames) >= 60 for ticks, frames in completions),
                          'fixture_unchanged': report_driver.sha256(out / 'runtime/Userdata/Replays/01-fixture.ccreplay') == fixture_hash,
                          'back_to_landing': 'assert_substate expected=Landing actual=Landing PASS' in log,
                          'duration_matches_summary': duration['pass']}
                captures = []
                for stem in ('report_saved_replay', 'report_saved_replay_return'):
                    pattern = re.compile(re.escape(stem) + r'_\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2}\.\d+\.png')
                    paths = sorted(path for path in (out / 'runtime/ScreenShots').glob(stem + '_*.png') if pattern.fullmatch(path.name))
                    if len(paths) != 1:
                        checks[stem] = False
                        continue
                    capture = report_driver.capture_geometry(paths[-1], size)
                    captures.append(capture)
                    checks[stem] = capture['inside_viewport'] and capture['dimensions'] == list(size)
                checks['distinct_captures'] = len({capture['path'] for capture in captures}) == 2
                extra[who] = {'checks': checks, 'playback': completions, 'captures': captures, 'duration': duration,
                              'saved_sha256': report_driver.sha256(saved) if saved.exists() else None}
            result['saved_replay'] = extra
            result['pass'] = result['pass'] and all(all(row['checks'].values()) for row in extra.values())
            (root / 'result.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
            results[root.name] = result
    finally:
        report_driver.menu_script, report_driver.make_run = base_script, base_run
    result = {'pass': all(row['pass'] for row in results.values()), 'runs': results,
              'driver_sha256': report_driver.sha256(__file__), 'report_driver_sha256': report_driver.sha256(report_driver.__file__),
              'exe_sha256': options.exe_sha256, 'fixture': str(fixture), 'fixture_sha256': fixture_hash,
              'ports': [options.port + index for index in range(len(sizes))]}
    (output / 'result.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps({'pass': result['pass'], 'out': str(output), 'exe_sha256': options.exe_sha256}), flush=True)
    return 0 if result['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
