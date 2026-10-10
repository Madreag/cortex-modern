"""Run one relayed match peer through the platform runner or its Mac app."""
import argparse
import datetime
import json
import os
from pathlib import Path
import plistlib
import subprocess
import sys
import time
import uuid
from types import SimpleNamespace
from urllib.request import Request, urlopen
import ssl


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('repo', 'binary', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--role', choices=('host', 'client'), required=True)
    parser.add_argument('--directory', required=True)
    parser.add_argument('--pin', required=True)
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--ticks', type=int, default=108600)
    parser.add_argument('--app-template', type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(args.repo / 'tools'))
    import feel_measure
    import posix_test_runner as posix
    runtime = args.out / 'runtime'
    if os.name == 'nt':
        import win32_test_runner as runner
        runtime.mkdir()
        for name in ('Userdata', 'Temp', 'Mods', 'ScreenShots'):
            (runtime / name).mkdir()
        subprocess.run(['pwsh', '-NoProfile', '-File', str(Path(__file__).with_name('prepare_runtime.ps1')), '-Runtime', str(runtime), '-Data', str(args.repo / 'Data')], check=True, creationflags=subprocess.CREATE_NO_WINDOW)
        (runtime / 'Userdata/Settings.ini').write_text(posix.DEFAULT_SETTINGS, encoding='utf-8')
        (args.out / 'runtime.json').write_text(json.dumps({'settings_overrides': {}}), encoding='utf-8')
    else:
        posix.prepare_runtime(args.repo, args.out, binary=args.binary)
    run = SimpleNamespace(cwd=runtime, out=args.out)
    feel_measure.private_settings(run, 60)
    feel_measure.stage_baseline(run, args.ticks, 2)
    install_key = uuid.uuid4().hex
    values = dict(posix.SETTINGS_OVERRIDES, SessionDirectoryUrl=args.directory, SessionDirectoryCertSha256=args.pin,
                  SessionDirectoryInstallKey=install_key, NetworkIceEnable='1', NetworkStunServers='',
                  NetworkConnectionMode='RelayOnly', NetworkHostRelayMode='Directory',
                  NetworkPlayerName=args.role)
    settings = runtime / 'Userdata/Settings.ini'
    settings.write_text(posix.apply_settings_overrides(settings.read_text(), values), encoding='utf-8')
    flags = ['-seed', '42', '-max-ticks', str(args.ticks), '-out', str(args.out / 'trace.json'),
             '-net-live-tick-hashes', str(args.out / 'live.jsonl'), '-feel-render-settings', str(runtime / 'Userdata/FeelRender.ini'),
             '-net-match-service-e2e', '-net-port', str(args.port), '-net-match-ticks', str(args.ticks), '-net-match-humans', '2',
             '-net-match-peers', '2', '-net-match-cpu-slots', '0', '-net-match-service-preset', 'Determinism FeelBaseline',
             '-net-match-service-module', 'UserScenes.rte', '-net-match-auto-delay', '-net-local-prediction', 'on',
             '-net-reconnect-ticket', str(args.out / 'seat.private'), '-net-match-report', str(args.out / 'match.json'),
             '-net-ice', 'on', '-net-rendezvous-log', '6']
    if args.role == 'host':
        flags += ['-net-host']
    else:
        context = ssl.create_default_context()
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
        request = Request('https://' + args.directory.removeprefix('https://') + '/v1/sessions', headers={'X-Install-Key': install_key})
        with urlopen(request, context=context, timeout=10) as response:
            rows = json.loads(response.read())
        rows = rows.get('sessions', rows) if isinstance(rows, dict) else rows
        if len(rows) != 1:
            raise RuntimeError('The isolated directory does not have exactly one host')
        flags += ['-net-join-session', rows[0]['session_id']]
    begun = datetime.datetime.now(datetime.timezone.utc).isoformat()
    if args.app_template:
        app = runtime / 'CortexFight.app'
        (app / 'Contents/MacOS').mkdir(parents=True)
        info = plistlib.loads((args.app_template / 'Contents/Info.plist').read_bytes())
        info.pop('LSEnvironment', None)
        executable = info['CFBundleExecutable']
        (app / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
        (app / 'Contents/MacOS' / executable).symlink_to(args.binary)
        environment = dict(os.environ)
        environment.pop('CCCP_HEADLESS', None)
        result = subprocess.run(['open', '-W', '-n', '-o', str(args.out / 'stdout.log'), '--stderr', str(args.out / 'stderr.log'), str(app), '--args', *flags], env=environment, timeout=2200)
        code = result.returncode
    else:
        record = runner.run([str(args.binary), '-headless', *flags], cwd=runtime, out=args.out / 'native', timeout=2200, env={'CCCP_HEADLESS': '1'})
        code = record.get('exit_code', record.get('returncode', 1))
    (args.out / 'peer.json').write_text(json.dumps(dict(started_utc=begun, finished_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(), exit=code, ticks=args.ticks, role=args.role), indent=2) + '\n', encoding='utf-8')
    (args.out / 'peer.exit').write_text(str(code), encoding='utf-8')
    return code


if __name__ == '__main__':
    raise SystemExit(main())
