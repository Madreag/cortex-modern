"""Stage the two retained save inputs, preserving their bytes and provenance; never create old saves."""
import argparse
import json
from pathlib import Path
import shutil

from verdict_artifact import sha256


def stage(out, original7=None, fork0920=None, dry_run=False):
    rows = []
    if not dry_run: out.mkdir(parents=True, exist_ok=False)
    for label, source in (('original7', original7), ('fork0920', fork0920)):
        destination = out/(label + '.ccsave')
        row = dict(label=label, path=destination.name, source=str(source) if source else None)
        if source and source.is_file():
            row.update(sha256=sha256(source), bytes=source.stat().st_size, mtime_ns=source.stat().st_mtime_ns)
            if not dry_run: shutil.copy2(source, destination)
        else:
            row.update(passed=False, reason=f'retained {label} save is not installed at the declared input source')
        rows.append(row)
    document = dict(schema=1, inputs=rows, passed=all('sha256' in row for row in rows))
    if not dry_run: (out/'inputs.json').write_text(json.dumps(document, indent=2)+'\n', encoding='utf-8')
    return document


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--original-7', type=Path)
    parser.add_argument('--fork-0920', type=Path)
    parser.add_argument('--dry-run', action='store_true')
    options = parser.parse_args(argv)
    result = stage(options.out, options.original_7, options.fork_0920, options.dry_run)
    print(json.dumps(result, indent=2))
    # Missing retained input is an explicit product failure of the save row. The chain still ships the declared manifest.
    return 0 if options.dry_run or result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
