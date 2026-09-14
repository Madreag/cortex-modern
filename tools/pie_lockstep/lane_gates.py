"""Run the lane's gates and arms on one executable, in order, and record each result."""
from __future__ import annotations

import hashlib
import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def run(command, log):
    proc = subprocess.run(command, capture_output=True, text=True)
    Path(log).write_text(proc.stdout + '\n' + proc.stderr, encoding='utf-8')
    return proc.returncode


def main():
    exe = sys.argv[1]
    root = Path(sys.argv[2])
    root.mkdir(parents=True, exist_ok=True)
    results = {'exe_sha256': hashlib.sha256(Path(exe).read_bytes()).hexdigest()}

    results['selftests'] = run([sys.executable, str(REPO / 'tools/run_selftests.py'), '--repo', str(REPO),
                                '--out', str(root / 'selftests'), '--timeout', '300'], root / 'selftests.log')
    results['script_graph'] = run([sys.executable, str(REPO / 'tools/run_sim_test.py'), '--repo', str(REPO),
                                   '--out', str(root / 'script-graph'), '--timeout', '300', '--',
                                   '-script-graph-selftest', '-num-lua-states', '4'], root / 'script-graph.log')

    arms = [('green2-next', 'next', 'net', '47921'), ('green2-actor-cancel', 'actor_cancel', 'net', '47922'),
            ('green2-delivery-cancel', 'delivery_cancel', 'net', '47923'), ('green2-goto', 'goto', 'net', '47924'),
            ('sp2-next', 'next', 'sp', '47921'), ('sp2-actor-cancel', 'actor_cancel', 'sp', '47921'),
            ('sp2-delivery-cancel', 'delivery_cancel', 'sp', '47921')]
    for name, case, kind, port in arms:
        command = [sys.executable, str(HERE / 'run_arm.py'), case, str(root / name), '--exe', exe, '--port', port, '--observe']
        if kind == 'sp':
            command.append('--sp')
        results[name] = run(command, root / (name + '.log'))
    (root / 'gates.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    print(json.dumps(results, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
