"""Build the pinned GNS checkout with the checked-in TURN patch."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('repo', 'source', 'build', 'install', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--dependency', default='')
    parser.add_argument('--generator', default='Ninja')
    parser.add_argument('--cmake-arg', action='append', default=[])
    parser.add_argument('--jobs', type=int, default=8)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    patch = args.repo / 'external/patches/gns-turn-lifetime.patch'
    with (args.out / 'gns-build.log').open('w', encoding='utf-8') as log:
        def run(command):
            subprocess.run([str(value) for value in command], stdout=log, stderr=subprocess.STDOUT, check=True)
        if not args.source.exists():
            run(['git', 'clone', '--depth', '1', '--branch', 'v1.6.0', 'https://github.com/ValveSoftware/GameNetworkingSockets.git', args.source])
        revision = subprocess.check_output(['git', '-C', str(args.source), 'rev-parse', 'HEAD'], text=True).strip()
        if revision != '2cb93a06350bb065db53abdb0d87cf297e0bfd34':
            raise ValueError('GNS checkout differs from the pinned upstream')
        run(['git', '-C', args.source, 'apply', patch.resolve()])
        options = ['-DCMAKE_BUILD_TYPE=Release', '-DBUILD_SHARED_LIB=OFF', '-DBUILD_STATIC_LIB=ON', '-DBUILD_TESTS=OFF',
                   '-DBUILD_EXAMPLES=OFF', '-DBUILD_TOOLS=OFF', '-DENABLE_ICE=ON', '-DUSE_CRYPTO=OpenSSL', '-DUSE_STEAMWEBRTC=OFF',
                   '-DLTO=OFF', '-DCMAKE_INSTALL_PREFIX=' + str(args.install.resolve())]
        if args.dependency:
            options.append('-DCMAKE_PREFIX_PATH=' + args.dependency)
        run(['cmake', '-S', args.source, '-B', args.build, '-G', args.generator, *options, *args.cmake_arg])
        run(['cmake', '--build', args.build, '--config', 'Release', '--parallel', str(args.jobs)])
        run(['cmake', '--install', args.build, '--config', 'Release'])
    library = next(iter(args.install.glob('lib/*GameNetworkingSockets_s.*')))
    record = dict(upstream=revision, patch_sha256=hashlib.sha256(patch.read_bytes()).hexdigest(),
                  library=str(library.resolve()), library_sha256=hashlib.sha256(library.read_bytes()).hexdigest(), lifetime_version=3)
    (args.out / 'gns-build.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
    print('[gns-build] complete lifetime_version=3', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
