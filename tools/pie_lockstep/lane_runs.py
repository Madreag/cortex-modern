"""Run a named list of pie_lockstep arms one after another and record each result."""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', required=True)
    parser.add_argument('--root', required=True, type=Path)
    parser.add_argument('--log', required=True, type=Path)
    parser.add_argument('jobs', nargs='+', help='name:case:sp|net:port[:debug]')
    args = parser.parse_args()
    args.root.mkdir(parents=True, exist_ok=True)
    results = []
    for job in args.jobs:
        parts = job.split(':')
        name, case, kind, port = parts[0], parts[1], parts[2], parts[3]
        out = args.root / name
        cmd = [sys.executable, str(HERE / 'run_arm.py'), case, str(out), '--exe', args.exe, '--port', port, '--observe']
        if kind == 'sp':
            cmd.append('--sp')
        if len(parts) > 4 and parts[4] == 'debug':
            cmd.append('--debug')
        started = time.time()
        proc = subprocess.run(cmd, capture_output=True, text=True)
        row = dict(name=name, case=case, kind=kind, port=port, returncode=proc.returncode,
                   seconds=round(time.time() - started, 1), out=str(out), cmd=cmd,
                   stdout=proc.stdout[-4000:], stderr=proc.stderr[-4000:])
        results.append(row)
        print(json.dumps({k: row[k] for k in ('name', 'returncode', 'seconds')}), flush=True)
        args.log.write_text(json.dumps(results, indent=2), encoding='utf-8')
    return 0 if all(row['returncode'] == 0 for row in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
