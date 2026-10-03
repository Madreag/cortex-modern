"""Keep a predeclared owner row explicit until its native scenario lands."""
import argparse
import json
from pathlib import Path
import subprocess
import sys


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--scenario', required=True)
    parser.add_argument('--owner', default='rows lane')
    parser.add_argument('--size')
    parser.add_argument('--port')
    parser.add_argument('--scratch-root')
    parser.add_argument('--fullstate-every')
    parser.add_argument('--dry-run', action='store_true')
    options = parser.parse_args(argv)
    scenario = options.repo/'tools/e2e'/f'{options.scenario}.json'
    driver = options.repo/'tools/e2e_video.py'
    if scenario.is_file() and driver.is_file():
        forwarded = []
        index = 0
        while index < len(argv):
            if argv[index] == '--owner': index += 2; continue
            forwarded.append(argv[index]); index += 1
        return subprocess.run([sys.executable, '-B', str(driver), *forwarded]).returncode
    reason = f'{options.owner}: native scenario has not landed: {scenario}'
    if options.dry_run:
        print(f'owner row declared; execution will write FAIL: {reason}')
        return 0
    options.out.mkdir(parents=True, exist_ok=False)
    result = dict(schema=1, scenario=options.scenario, passed=False, verdict='requires-missing', status='FAIL',
                  reason=reason, owner=options.owner, checklist=[], counts=dict(executed=0, failed=1))
    (options.out/'review.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(reason)
    return 1


if __name__ == '__main__':
    raise SystemExit(main())
