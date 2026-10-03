"""Capture the declared Z13 host and Mac client in their desktop sessions, then join their native evidence."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time

from acceptance_collection import read, write
import acceptance_remote as remote_driver

PORT = 49412


def ssh(command, *, timeout=120):
    return subprocess.run(['ssh', '-o', 'BatchMode=yes', 'Erol-Mac', command], capture_output=True,
                          text=True, timeout=timeout, check=True).stdout.strip()


def scp(source, destination):
    subprocess.run(['scp', '-q', str(source), str(destination)], check=True, timeout=900)


def run(options):
    split, rb = remote_driver.inventory_modules(options.inventory)
    boxes, _ = split.load_manifest(options.inventory/'boxes.json')
    host = next(box for box in boxes if box.name == options.host_box)
    if host.name != 'Z13' or host.kind != 'windows-task':
        raise ValueError('the acceptance cross capture requires the Z13 session task')
    plan = dict(driver='EROL-PC', host=host.name, client='Mac', host_runner=host.task,
                client_runner='launchd GUI session', port=PORT, scenario='mp-host-join-cross',
                mac_tree=options.mac_lane + '/repo', builds='reuse the completed acceptance streams')
    if options.dry_run:
        print(json.dumps(plan)); return 0
    schedule = read(options.root/'split-plan.json')
    source, executable, cid = (schedule[key] for key in ('source_sha', 'exe_sha256', 'collection_id'))
    out = options.out.resolve(); out.mkdir(parents=True, exist_ok=True)
    mac = options.mac_lane.rstrip('/')
    ssh(f'test -f {shlex.quote(mac + "/exit.txt")} && test "$(git -C {shlex.quote(mac + "/repo")} rev-parse HEAD)" = {shlex.quote(source)}')
    host, remote, rb, remote_root = remote_driver.prepare_box(options.repo, host, options.root, options.inventory, source, executable)
    ip_script = "$a=Get-NetAdapter -Physical | Where-Object Status -eq 'Up'; Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.InterfaceIndex -in $a.ifIndex -and $_.IPAddress -like '192.168.50.*' } | Select-Object -ExpandProperty IPAddress"
    addresses = remote.ssh(ip_script).strip().splitlines()
    if len(addresses) != 1 or not re.fullmatch(r'192\.168\.50\.\d{1,3}', addresses[0].strip()):
        raise ValueError('Z13 has no single physical LAN address reachable by the Mac; no adapter was changed')
    address = addresses[0].strip()
    own = remote_root/'S6/S6.cross-host-join'
    host_payload = dict(kind='driver', box='Z13', source_sha=source, collection_id=cid, repo=host.repo,
        executable=host.exe, exe_sha256=executable, identity=(own/'identity-Z13.json').as_posix(), timeout=1800,
        commands=[['python','-B',host.repo+'/tools/e2e_video.py','--repo',host.repo,'--out',(own/'host-half').as_posix(),
                   '--scenario','mp-host-join-cross','--peer','host','--size','960x540','--port',str(PORT)]])
    done = remote_driver.task_payload(host, remote, rb, own, host_payload, out/'host-task', wait=False)
    # The second Mac GUI job has its own lane and label; it uses the already-built source/executable.
    client_lane = mac + '/cross-capture'
    client_repo = mac + '/repo'
    q = shlex.quote
    script = ('#!/bin/zsh\nset -u\n'
              f'cd {q(client_repo)}\nexport CCCP_HEADLESS=1 PYTHONDONTWRITEBYTECODE=1\n'
              f'export CCCP_TEST_BINARY={q(client_repo + "/build-gcc/CortexCommand")} HOST_ADDRESS={q(address)}\n'
              f'/opt/homebrew/bin/python3 -B tools/acceptance_identity.py --box Mac --repo {q(client_repo)} '
              f'--exe "$CCCP_TEST_BINARY" --build-receipt {q(mac + "/evidence/build.json")} '
              f'--source-sha {q(source)} --collection-id {q(cid)} --out {q(client_lane + "/evidence/identity.json")}\n'
              'rc=$?\nif [ "$rc" = 0 ]; then\n'
              f' /opt/homebrew/bin/python3 -B tools/e2e_video.py --repo {q(client_repo)} --out {q(client_lane + "/evidence/client-half")} '
              f'--scenario mp-host-join-cross --peer client --size 960x540 --port {PORT}\n rc=$?\nfi\n'
              f'printf "%s\\n" "$rc" > {q(client_lane + "/exit.txt")}\nexit "$rc"\n')
    script_path = out/'client-stream.zsh'; script_path.write_text(script, encoding='utf-8', newline='\n')
    ssh(f'test ! -e {q(client_lane)} && mkdir -p {q(client_lane + "/evidence")}')
    scp(script_path, f'Erol-Mac:{client_lane}/stream.zsh')
    launched = ssh(f'/opt/homebrew/bin/python3 {q(client_repo + "/tools/macos/acceptance_launch.py")} '
        f'--lane {q(client_lane)} --source-sha {q(source)} --collection-id {q(cid)} --label-suffix s6 '
        f'--stream-sha256 {hashlib.sha256(script_path.read_bytes()).hexdigest()}')
    write(out/'mac-launch.json', json.loads(launched))
    ended = remote.wait_done(done, 1920)
    deadline = time.monotonic() + 1920
    client_code = None
    while time.monotonic() < deadline:
        value = ssh(f'if [ -f {q(client_lane + "/exit.txt")} ]; then cat {q(client_lane + "/exit.txt")}; fi')
        if value:
            client_code = int(value); break
        time.sleep(5)
    remote_driver.fetch_evidence(host, remote, rb, own, out)
    identity = read(out/'identity-Z13.json')
    write(options.root/'S6/evidence/identity-Z13.json', identity)
    # Same validated file-by-file transfer used for Windows evidence, with a POSIX pack invocation.
    archive = client_lane + '/evidence.tar'; manifest = client_lane + '/evidence-files.json'
    ssh(f'/opt/homebrew/bin/python3 {q(client_repo + "/tools/acceptance_remote.py")} --pack {q(client_lane)} '
        f'--archive {q(archive)} --manifest {q(manifest)}', timeout=900)
    fetched = out/'fetched'; fetched.mkdir(exist_ok=True)
    scp(f'Erol-Mac:{archive}', fetched/'evidence.tar')
    scp(f'Erol-Mac:{manifest}', fetched/'evidence-files.json')
    remote_driver.unpack_evidence(fetched/'evidence.tar', fetched, read(fetched/'evidence-files.json'))
    command = [sys.executable,'-B',str(options.repo/'tools/e2e_video.py'),'--merge-peer-captures',str(out/'host-half'),
               str(fetched/'evidence/client-half'),'--out',str(out/'run')]
    merged = subprocess.run(command, cwd=options.repo, timeout=1800)
    host_code = re.search(r'\brc=(-?\d+)', ended)
    return 0 if host_code and int(host_code[1]) == 0 and client_code == 0 and merged.returncode == 0 else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--inventory', type=Path, required=True)
    parser.add_argument('--host-box', required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--mac-lane', required=True)
    parser.add_argument('--dry-run', action='store_true')
    return run(parser.parse_args(argv))


if __name__ == '__main__':
    raise SystemExit(main())
