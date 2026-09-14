"""Run pie input fixtures through the existing isolated simulation runner."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import time

REPO = Path(__file__).resolve().parents[2]
FIXTURES = Path(__file__).resolve().parent / 'fixtures'
LANE_PORTS = ((47921, 47926), (48291, 48299))
sys.path.insert(0, str(REPO / 'tools'))
from run_sim_test import make_run
from win32_test_runner import firewall_allows_inbound


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('case', choices=['next', 'prev', 'goto', 'actor_cancel', 'delivery_cancel'])
    parser.add_argument('out', type=Path)
    parser.add_argument('--exe', type=Path, default=REPO / 'Cortex Command.exe')
    parser.add_argument('--port', type=int, default=47921)
    parser.add_argument('--sp', action='store_true')
    parser.add_argument('--observe', action='store_true')
    parser.add_argument('--debug', action='store_true')
    parser.add_argument('--stall', help='engine frame stall as TICK:MS, armed on one side only')
    parser.add_argument('--stall-peer', choices=['host', 'client'], default='host')
    args = parser.parse_args()
    args.out = args.out.resolve()
    if args.out.exists():
        parser.error('evidence directory already exists')
    if not any(low <= args.port <= high for low, high in LANE_PORTS):
        parser.error('port outside the assigned lanes')
    stall_peer = 'sp' if args.sp else args.stall_peer
    if args.stall is not None:
        tick, _, ms = args.stall.partition(':')
        if not (tick.isdigit() and ms.isdigit() and int(ms) > 0):
            parser.error('--stall expects TICK:MS')
    if firewall_allows_inbound(args.exe) is not True:
        parser.error('executable has no verified inbound firewall rule: ' + str(args.exe))
    fixture = FIXTURES / (args.case + '.txt')
    observer = FIXTURES / 'PieObserver.lua'
    setup = FIXTURES / 'PieSwitchSP.lua'
    args.out.mkdir(parents=True)
    manifest = dict(case=args.case, sp=args.sp, exe=str(args.exe), exe_sha256=sha(args.exe),
                    fixture_sha256=sha(fixture), observer_sha256=sha(observer),
                    setup_sha256=sha(setup), driver_sha256=sha(Path(__file__)),
                    port=args.port, input_delay=0 if args.sp else 3,
                    observe=args.observe, debug=args.debug,
                    stall=args.stall, stall_peer=stall_peer if args.stall else None)
    (args.out / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    peers = ['sp'] if args.sp else ['host', 'client']
    runs = []
    records = {}
    try:
        for peer in peers:
            trace = args.out / (peer + '_trace.json')
            flags = ['-seed', '42', '-max-ticks', '320', '-tick-hashes',
                     '-num-lua-states', '4', '-out', str(trace)]
            if args.sp:
                flags += ['-scenario', 'PieSwitchSP']
            else:
                flags += ['-free-run-sim', '-net-match-service-e2e', '-net-port', str(args.port),
                          '-net-match-ticks', '320', '-net-match-input-delay', '3',
                          '-net-match-report', str(args.out / (peer + '_report.json'))]
                flags += ['-net-host'] if peer == 'host' else ['-net-join', '127.0.0.1']
                if peer == 'host':
                    flags += ['-net-match-e2e-spawn', 'ACrab:Crab:Base.rte:920:760:20']
            if peer != 'client':
                flags += ['-input-script', str(fixture)]
            if args.stall is not None and peer == stall_peer:
                flags += ['-selftest-frame-stall', args.stall]
            if args.observe or args.case in ('actor_cancel', 'delivery_cancel'):
                flags += ['-test-script', 'UserScenes.rte/PieObserver.lua']
            if args.debug:
                flags += ['-controller-debug-dump', str(args.out / (peer + '_controller.jsonl')),
                          '-controller-debug-ticks', '29-44,199-213']
            env = {'CCCP_HEADLESS': '1', 'CC_SIM_DUMP': '27:320',
                   'PATH': str(REPO) + os.pathsep + os.environ.get('PATH', '')}
            run = make_run(REPO, flags, args.out / peer, timeout=180, env=env,
                           expected=[trace, Path(str(trace) + '.simdump.txt')])
            run.argv[0] = str(args.exe.resolve())
            module = Path(run.cwd) / 'Userdata/UserScenes.rte'
            module.mkdir(exist_ok=True)
            (module / 'PieObserver.lua').write_bytes(observer.read_bytes())
            (module / 'PieCase.lua').write_text('return ' + json.dumps(args.case) + '\n', encoding='utf-8')
            (module / 'PieSwitchSP.lua').write_bytes(setup.read_bytes())
            index = ('DataModule\n\tModuleName = User Scenes\n\tScanFolderContents = 1\n'
                     '\tIgnoreMissingItems = 1\n')
            if args.sp:
                index += ('\tAddActivity = GAScripted\n\t\tPresetName = Determinism PieSwitchSP\n'
                          '\t\tSceneName = Grasslands\n\t\tScriptPath = UserScenes.rte/PieSwitchSP.lua\n'
                          '\t\tLuaClassName = PieSwitchSP\n\t\tMinTeamsRequired = 1\n'
                          '\t\tIsTestActivity = 1\n\t\tDefaultRequireClearPathToOrbit = 0\n'
                          '\t\tDefaultFogOfWar = 0\n\t\tDefaultDeployUnits = 0\n')
            (module / 'Index.ini').write_text(index, encoding='utf-8')
            runs.append((peer, run))
            run.start()
            if peer == 'host':
                time.sleep(0.75)
        for peer, run in runs:
            record = run.finish()
            records[peer] = {key: record.get(key) for key in
                             ('pid', 'exit_code', 'timed_out', 'evidence_complete', 'verdict_lines')}
    finally:
        for _, run in runs:
            run.close()
    (args.out / 'run_result.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
    print(json.dumps(records, indent=2))
    return 0 if all(row['exit_code'] == 0 and row['evidence_complete'] for row in records.values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())
