"""Run the F23c local-pie-write fixtures through the existing isolated simulation runner."""
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
sys.path.insert(0, str(REPO / 'tools'))
from run_sim_test import make_run
from win32_test_runner import firewall_allows_inbound

CASES = ('buy_menu', 'form_squad', 'full_inventory')
PORT_LO, PORT_HI = 48171, 48179
TICKS = 320


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('case', choices=CASES)
    parser.add_argument('out', type=Path)
    parser.add_argument('--exe', type=Path, default=REPO / 'Cortex Command.exe')
    parser.add_argument('--port', type=int, default=PORT_LO)
    args = parser.parse_args()
    args.out = args.out.resolve()
    if args.out.exists():
        parser.error('evidence directory already exists')
    if not PORT_LO <= args.port <= PORT_HI:
        parser.error('port outside the assigned lane')
    if firewall_allows_inbound(args.exe) is not True:
        parser.error('executable has no verified inbound firewall rule: ' + str(args.exe))
    fixture = FIXTURES / (args.case + '.txt')
    observer = FIXTURES / 'PieWriteObserver.lua'
    args.out.mkdir(parents=True)
    manifest = dict(case=args.case, exe=str(args.exe), exe_sha256=sha(args.exe),
                    fixture_sha256=sha(fixture), observer_sha256=sha(observer),
                    driver_sha256=sha(Path(__file__)), port=args.port, input_delay=3)
    (args.out / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    runs = []
    records = {}
    try:
        for peer in ('host', 'client'):
            trace = args.out / (peer + '_trace.json')
            flags = ['-seed', '42', '-max-ticks', str(TICKS), '-tick-hashes',
                     '-num-lua-states', '4', '-out', str(trace),
                     '-free-run-sim', '-net-match-service-e2e', '-net-port', str(args.port),
                     '-net-match-ticks', str(TICKS), '-net-match-input-delay', '3',
                     '-net-match-report', str(args.out / (peer + '_report.json')),
                     '-test-script', 'UserScenes.rte/PieWriteObserver.lua']
            flags += ['-net-host'] if peer == 'host' else ['-net-join', '127.0.0.1']
            if peer == 'host':
                flags += ['-net-match-e2e-spawn', 'ACrab:Crab:Base.rte:920:760:20',
                          '-input-script', str(fixture)]
            env = {'CCCP_HEADLESS': '1', 'CC_SIM_DUMP': '27:320',
                   'PATH': str(REPO) + os.pathsep + os.environ.get('PATH', '')}
            run = make_run(REPO, flags, args.out / peer, timeout=180, env=env,
                           expected=[trace, Path(str(trace) + '.simdump.txt')])
            run.argv[0] = str(args.exe.resolve())
            module = Path(run.cwd) / 'Userdata/UserScenes.rte'
            module.mkdir(exist_ok=True)
            (module / 'PieWriteObserver.lua').write_bytes(observer.read_bytes())
            (module / 'PieCase.lua').write_text('return ' + json.dumps(args.case) + '\n', encoding='utf-8')
            (module / 'Index.ini').write_text('DataModule\n\tModuleName = User Scenes\n\tScanFolderContents = 1\n'
                                              '\tIgnoreMissingItems = 1\n', encoding='utf-8')
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
