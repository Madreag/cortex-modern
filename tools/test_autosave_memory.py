"""Keep an autosaving match's memory bounded for an hour (one machine, two engines, proof:false).

A hosted two-peer match saves every --autosave-seconds for --minutes while each engine's private (committed) bytes and
working set are read from its process every --sample-seconds, beside the engine's own memory census lines. After the
--settle-minutes warm-up, each peer's private bytes may rise by at most --slope-mb-per-minute on a least-squares line
through the samples and may not end more than --rise-mb above their lowest settled sample. Every named capture must be
taken, every save written, and both peers must exit.
"""
import argparse
import ctypes
from datetime import datetime, timezone
import hashlib
import json
import os
import platform
from pathlib import Path
import re
import subprocess
import sys
import threading
import time


class MemoryCounters(ctypes.Structure):
    _fields_ = [("cb", ctypes.c_ulong), ("PageFaultCount", ctypes.c_ulong), ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t), ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t), ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t), ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t)]


def process_memory(run):
    """The engine's working set and private bytes, read from its own process; None once it has exited."""
    if sys.platform != "win32":
        pid = (getattr(run, "record", {}) or {}).get("pid")
        if not pid:
            return None
        fields = subprocess.run(["ps", "-o", "rss=,vsz=", "-p", str(pid)], capture_output=True, text=True).stdout.split()
        return {"working_set_mb": int(fields[0]) / 1024, "virtual_mb": int(fields[1]) / 1024} if len(fields) == 2 else None
    handle = getattr(run, "process", None)
    if not handle:
        return None
    counters = MemoryCounters()
    counters.cb = ctypes.sizeof(counters)
    if not ctypes.windll.psapi.GetProcessMemoryInfo(ctypes.c_void_p(handle), ctypes.byref(counters), counters.cb):
        return None
    return {"working_set_mb": counters.WorkingSetSize / 2**20, "private_mb": counters.PagefileUsage / 2**20}


def slope(points):
    if len(points) < 2:
        return 0.0
    n = len(points)
    mx = sum(x for x, _ in points) / n
    my = sum(y for _, y in points) / n
    den = sum((x - mx) ** 2 for x, _ in points)
    return sum((x - mx) * (y - my) for x, y in points) / den if den else 0.0


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repo', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--minutes', type=float, default=60)
    p.add_argument('--autosave-seconds', type=int, default=60)
    p.add_argument('--sample-seconds', type=float, default=60)
    p.add_argument('--settle-minutes', type=float, default=10)
    p.add_argument('--slope-mb-per-minute', type=float, default=1.0)
    p.add_argument('--rise-mb', type=float, default=100)
    p.add_argument('--census-histogram', action='store_true')
    p.add_argument('--split-cpus', action='store_true',
                   help='each engine on its own half of the logical CPUs, so one peer cannot take the other\'s time')
    p.add_argument('--port', type=int, default=49488)
    a = p.parse_args()
    repo = a.repo.resolve()
    sys.path.insert(0, str(repo / 'tools'))
    from run_sim_test import make_run, seed_settings
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out / 'driver-as-run.py').write_bytes(Path(__file__).read_bytes())
    quit_path = out / 'quit.json'
    seconds = int(a.minutes * 60)
    menu = out / 'menu.txt'
    menu.write_text(f'in_play\nwait_file {quit_path.as_posix()} {seconds + 600}\nexit\n')
    # The match ends at its own tick count: a probe that could end it waits at most half an hour.
    ticks = str(60 * seconds)
    common = ['-net-port', str(a.port), '-net-match-peers', '2', '-net-match-input-delay', '1',
              '-net-autosave-seconds', str(a.autosave_seconds), '-net-match-ticks', ticks, '-max-ticks', ticks,
              '-menu-script', str(menu), '-memory-census-ticks', '3600'] + (['-memory-census-histogram'] if a.census_histogram else [])
    argv = {'host': ['-net-match-service-e2e', '-net-host'] + common,
            'client': ['-net-match-service-e2e', '-net-join', '127.0.0.1'] + common}
    env = {'CCCP_HEADLESS': '1', 'CC_RUNNER_IGNORE_FULLSCREEN': '1', 'CC_TEST_NET_MATCH_E2E_WAIT_PEERS': '1'}
    half = (os.cpu_count() or 2) // 2
    masks = {'host': (1 << half) - 1, 'client': ((1 << half) - 1) << half} if a.split_cpus else {}
    settings = {'AutosaveSeconds': '0', 'NetworkHostDelayPolicy': 'Fixed', 'NetworkIceEnable': '0',
                'NetworkPortMapEnable': '0', 'NetworkHostVisibility': 'Unlisted', 'EnableLuaDebugging': '0',
                'ResolutionX': '960', 'ResolutionY': '540', 'EnableVSync': '0'}
    runs, records, started = {}, {}, []
    for role in ('host', 'client'):
        args = argv[role] + ['-net-match-report', str(out / f'{role}-report.json'),
                             '-input-script', str(repo / 'tools/e2e/play-input.txt'),
                             '-net-match-service-preset', 'Determinism FeelBaseline', '-net-match-service-module', 'UserScenes.rte']
        role_env = dict(env)
        if role in masks:
            role_env['CC_RUNNER_AFFINITY_MASK'] = f'0x{masks[role]:x}'
        runs[role] = make_run(repo, args, out / role, seconds + 1800, env=role_env)
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
    config = {'topology': 'single-box', 'proof': False, 'box': platform.node(), 'minutes': a.minutes, 'autosave_seconds': a.autosave_seconds,
              'settle_minutes': a.settle_minutes, 'slope_mb_per_minute': a.slope_mb_per_minute, 'rise_mb': a.rise_mb,
              'settings': settings, 'environment': env, 'affinity_masks': {role: f'0x{mask:x}' for role, mask in masks.items()},
              'source_head': subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip(),
              'exe_sha256': hashlib.sha256((repo / 'Cortex Command.exe').read_bytes()).hexdigest(),
              'started_utc': datetime.now(timezone.utc).isoformat()}
    (out / 'configuration.json').write_text(json.dumps(config, indent=2))

    def log(role):
        path = out / role / 'stdout.log'
        return path.read_text(errors='replace') if path.exists() else ''

    samples = []
    stop = threading.Event()
    began = time.monotonic()

    def sample():
        with open(out / 'memory.jsonl', 'a', encoding='utf-8') as handle:
            while True:
                row = {'minute': round((time.monotonic() - began) / 60, 3), **{role: process_memory(run) for role, run in runs.items()}}
                samples.append(row)
                handle.write(json.dumps(row) + '\n')
                handle.flush()
                if stop.wait(a.sample_seconds):
                    return

    failure = None
    sampler = threading.Thread(target=sample, daemon=True)
    try:
        for role in ('host', 'client'):
            runs[role].start()
            started.append(role)
        sampler.start()
        until = time.monotonic() + seconds + 1500
        while any(runs[role].poll() is None for role in started):
            if time.monotonic() > until:
                raise RuntimeError('the match did not end within its time')
            time.sleep(1)
    except Exception as error:
        failure = repr(error)
    finally:
        stop.set()
        if sampler.is_alive():
            sampler.join()
        quit_path.write_text(json.dumps({'requested_utc': datetime.now(timezone.utc).isoformat()}))
        for role in started:
            records[role] = runs[role].finish()
        for run in runs.values():
            run.close()
    bounds = {}
    for role in ('host', 'client'):
        settled = [(row['minute'], row[role]['private_mb']) for row in samples
                   if row.get(role) and 'private_mb' in row[role] and row['minute'] >= a.settle_minutes]
        values = [value for _, value in settled]
        bounds[role] = {'samples': len(settled), 'slope_mb_per_minute': round(slope(settled), 3),
                        'lowest_mb': round(min(values), 1) if values else None, 'last_mb': round(values[-1], 1) if values else None,
                        'rise_mb': round(values[-1] - min(values), 1) if values else None,
                        'highest_mb': round(max(values), 1) if values else None}
    captures = [int(t) for t in re.findall(r'^\[autosave\] named tick=(\d+) at=\d+ writers=\d+', log('host'), re.M)]
    refused = {role: re.findall(r'^\[autosave\] (?:named tick=\d+ not taken: .*|failed .*)$', log(role), re.M) for role in started}
    expected = int(a.minutes * 60 / a.autosave_seconds) - 2
    checks = {'no_driver_failure': failure is None,
              'all_exit_zero': len(records) == 2 and all(r.get('exit_code') == 0 and not r.get('timed_out') for r in records.values()),
              'periodic_captures_taken': len(captures) >= expected,
              'no_capture_refused': all(not refused[role] for role in started),
              'sampled_every_minute': all(bounds[role]['samples'] >= int(a.minutes - a.settle_minutes) for role in bounds),
              'private_slope_bounded': all(bounds[role]['slope_mb_per_minute'] <= a.slope_mb_per_minute for role in bounds),
              'private_rise_bounded': all(bounds[role]['rise_mb'] is not None and bounds[role]['rise_mb'] <= a.rise_mb for role in bounds)}
    result = {**config, 'pass': all(checks.values()), 'checks': checks, 'failure': failure, 'bounds': bounds, 'captures': len(captures),
              'expected_captures': expected, 'refused': refused, 'exits': {role: r.get('exit_code') for role, r in records.items()},
              'wall_minutes': round((time.monotonic() - began) / 60, 1)}
    (out / 'result.json').write_text(json.dumps(result, indent=2))
    print(json.dumps({k: result[k] for k in ('pass', 'checks', 'failure', 'bounds', 'captures', 'expected_captures')}))
    return 0 if result['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
