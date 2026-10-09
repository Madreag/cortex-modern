"""Check the FP policy and its native-callback failure boundaries."""

import argparse
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    passed = True
    for name in ('fp-environment', 'fp-environment-native-drift', 'fp-environment-error-drift'):
        result = subprocess.run([str(args.binary.resolve()), '-' + name + '-selftest'],
                                cwd=args.binary.resolve().parent, capture_output=True, text=True, timeout=60)
        text = result.stdout + result.stderr
        (args.out / (name + '.log')).write_text(text, encoding='utf-8')
        if name == 'fp-environment':
            ok = result.returncode == 0 and '[fp-environment-selftest] PASS\n' in text and 'FAIL' not in text
        else:
            ok = result.returncode != 0 and '[fp-environment] invalid at Lua' in text
        print(f"[fp-callback-detector] {'PASS' if ok else 'FAIL'} {name}", flush=True)
        passed = passed and ok
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
