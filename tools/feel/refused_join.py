"""Require both processes to name a deliberate module-set refusal."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from run_sim_test import make_run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--port', type=int, default=49494)
    args = parser.parse_args()
    if not 49470 <= args.port <= 49499:
        parser.error('the port must stay in 49470..49499')
    root, repo = args.out.resolve(), Path(__file__).resolve().parents[2]
    root.mkdir(parents=True, exist_ok=False)
    runs, records = {}, {}
    try:
        for peer in ('host', 'client'):
            flags = ['-net-allow-userdata', '-net-port', str(args.port), '-net-session-report', str(root / f'{peer}.json'),
                     '-num-lua-states', '4']
            flags += ['-net-host'] if peer == 'host' else ['-net-join', '127.0.0.1']
            run = make_run(repo, flags, root / peer, 30, env={'CCCP_HEADLESS': '1'})
            runs[peer] = run
            if peer == 'host':
                module = Path(run.cwd) / 'Mods/AdmissionMismatch.rte'
                module.mkdir(parents=True)
                (module / 'Index.ini').write_text('DataModule\n\tModuleName = Admission mismatch detector\n\tSupportedGameVersion = 7.0.0\n')
            run.start()
            if peer == 'host':
                time.sleep(1)
        for peer in ('client', 'host'):
            records[peer] = runs[peer].finish()
    finally:
        for run in runs.values():
            run.close()
    lines = {peer: [line for line in (root / peer / 'stdout.log').read_text(encoding='utf-8-sig', errors='replace').splitlines()
                    if '[net-session] admission refused' in line] for peer in runs}
    passed = all(any('ModuleManifestMismatch' in line and 'AdmissionMismatch.rte' in line for line in lines[peer]) for peer in runs)
    result = dict(passed=passed, lines=lines, records=records, port=args.port)
    (root / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f"[refused-join] {'PASS' if passed else 'FAIL'} " + str(root / 'result.json'))
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
