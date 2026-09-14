"""Compare the shared pie state and Lua observations with the SP reference."""
import argparse
import json
from pathlib import Path

import verify_pie_close as verify


def compare(case, reference, run):
    result = verify.check(run, case, reference=reference)
    return dict(pass_check=result['pass_check'], failures=result['failures'],
                compared_ticks=result['reference_ticks'], input_delay=3,
                net=result, reference=result['reference'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('case', choices=['next', 'prev', 'actor_cancel', 'delivery_cancel'])
    parser.add_argument('reference', type=Path)
    parser.add_argument('run', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    try:
        result = compare(args.case, args.reference, args.run)
    except (KeyError, ValueError, TypeError, OSError) as exc:
        result = dict(pass_check=False, failures=[str(exc)])
    args.out.write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(dict(pass_check=result['pass_check'], failures=len(result['failures']),
                          first=result['failures'][:6]), indent=2))
    raise SystemExit(0 if result['pass_check'] else 1)
