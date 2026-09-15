"""Compare every SP dump byte, trace row and Lua pie observation."""
import argparse
import json
from pathlib import Path
import sys

# A driver that runs this file in its own process does not put this directory on the path.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import verify_pie_close as verify


def compare(case, control, tip):
    left = verify.check(control, case, sp=True)
    right = verify.check(tip, case, sp=True)
    failures = ['control ' + value for value in left['failures']] + ['tip ' + value for value in right['failures']]
    equal = {}
    equal['dumps'] = (control / 'sp_trace.json.simdump.txt').read_bytes() == (tip / 'sp_trace.json.simdump.txt').read_bytes()
    equal['traces'] = verify.load_trace(control / 'sp_trace.json') == verify.load_trace(tip / 'sp_trace.json')
    observations = []
    for run in (control, tip):
        text = (run / 'sp/runtime/LogConsole.txt').read_text(encoding='utf-8-sig')
        observations.append(verify.OBSERVE.findall(text))
    equal['lua_observations'] = observations[0] == observations[1] and bool(observations[0])
    for name, match in equal.items():
        if not match:
            failures.append('SP ' + name + ' differ or are absent')
    return dict(pass_check=not failures, failures=failures, equal=equal,
                control=left, tip=right, script_rows=[len(rows) for rows in observations])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('case')
    parser.add_argument('control', type=Path)
    parser.add_argument('tip', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    result = compare(args.case, args.control, args.tip)
    args.out.write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(dict(pass_check=result['pass_check'], equal=result['equal'], script_rows=result['script_rows'],
                          failure_count=len(result['failures']), first_failures=result['failures'][:8]), indent=2))
    raise SystemExit(0 if result['pass_check'] else 1)
