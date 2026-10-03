"""Generate this Linux run's real version-refusal UI inputs; retain the captures and their hashes."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def version_manifest(capture_root):
    capture_root = Path(capture_root)
    capture_path = capture_root/'capture.json'
    capture = json.loads(capture_path.read_text(encoding='utf-8')) if capture_path.is_file() else {}
    mapping = {kind: {who: str(capture_root/kind/('client' if who == 'joiner' else who)/'stdout.log')
                      for who in ('host', 'joiner')} for kind in ('build', 'protocol')}
    paths = [Path(value) for group in mapping.values() for value in group.values()]
    return dict(mapping, schema=1, platform=sys.platform, source_sha=capture.get('source', {}).get('tip'),
                capture=dict(path=str(capture_path), sha256=digest(capture_path) if capture_path.is_file() else None),
                input_sha256={str(path): digest(path) for path in paths if path.is_file()})


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--port', type=int, default=50300)
    parser.add_argument('--dry-run', action='store_true')
    options = parser.parse_args(argv)
    required = [options.repo/'tools/e2e_video.py', options.repo/'tools/e2e/mp-version-mismatch.json']
    if any(not path.is_file() for path in required): raise ValueError('version-refusal driver or scenario is absent')
    if options.dry_run:
        print(f'GENERATED INPUT {options.out}/version-ui.json from this run\'s Linux build/protocol captures')
        return 0
    options.out.mkdir(parents=True, exist_ok=False)
    capture = options.out/'version-capture'
    command = [sys.executable, '-B', str(options.repo/'tools/e2e_video.py'), '--repo', str(options.repo),
               '--out', str(capture), '--scenario', 'mp-version-mismatch', '--port', str(options.port),
               '--port-block', f'{options.port}-{options.port+9}', '--scratch-root', str(options.out)]
    with (options.out/'generate.log').open('w', encoding='utf-8') as log:
        code = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT).returncode
    manifest = version_manifest(capture)
    manifest['generator_exit_code'] = code
    (options.out/'version-ui.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    return code


if __name__ == '__main__':
    raise SystemExit(main())
