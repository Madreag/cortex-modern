"""The two-machine proof: one lockstep match between this box (EROL-PC) and EDITH over the internet, measured by the
feel driver, and EDITH's own single-player tick budget.

    python tools/edith_cross.py --scenario mp-host-join --direction host-here|host-edith --path direct|relay|ip
                                --runs N --out D:/mx/opus-edith-cross-20260926/<run> [--dry-run]
    python tools/edith_cross.py --scenario sp-soak [--minutes 10] --out D:/mx/opus-edith-cross-20260926/<run>

The local peer starts through run_sim_test.make_run. The EDITH peer starts inside the owner's interactive session:
this file is copied to EDITH, tools/edith/session1.ps1 becomes D:/mx/session1/run.ps1 and the cortex-session1 task
runs it, which calls this file with --remote-peer, again through make_run and the private-desktop runner with
CCCP_HEADLESS=1. Both boxes use the same absolute run directory, so after the fetch the feel driver's own reducers
(feel_measure.reduce_timing_case and item9a_gates) read the pair exactly as they read one of its local arms.

Paths: direct = ICE with the engine's public STUN list, DirectOnly, the rendezvous through a session directory on this
box's loopback that `ssh -R` also opens on EDITH's loopback (signalling only; the match's packets take the route ICE
selects); relay = ICE RelayOnly through the TURN server named for each side; ip = a plain -net-join to the host's public
address, ICE off. One verdict line per run: frames/desyncs per peer, holds, waits over 50 ms, % waiting, the route lines.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / 'edith'))
from remote_box import SSH_NOISE, WINDOWS_TAR, RemoteBox  # noqa: E402  (shipped beside this file on EDITH)

HERE = Path(__file__).resolve().parent
# The calling lane's own scratch root, the same absolute path on both boxes.
LANE = os.environ.get('CC_EDITH_CROSS_LANE', 'opus-edith-cross-20260926')
SCRATCH = Path('D:/mx') / LANE  # the same absolute path on both boxes
PAYLOAD = SCRATCH / 'payload'
SESSION1_SCRIPT = 'D:/mx/session1/run.ps1'
TASK = 'cortex-session1'
REPO = Path('D:/Projects/takeover-build')
GAME_PORT, DIRECTORY_PORT = 49860, 49875  # the lane's block is 49860-49879 on both boxes
BRIDGE_UDP, BRIDGE_TCP = 49876, 49877
# EDITH reserves TCP 49675-49974 (netsh int ipv4 show excludedportrange), so the tunnel's loopback ends there sit outside it.
EDITH_TCP = {DIRECTORY_PORT: 49985, BRIDGE_TCP: 49986}
ADDRESS = {'here': '68.3.162.151', 'edith': '24.251.145.96'}
MACHINE = {'here': 'EROL-PC', 'edith': 'EDITH'}
# The TURN URL each side can reach; EDITH reaches this site only through its public address.
TURN = {'here': 'turn:192.168.50.122:3479?transport=udp', 'edith': 'turn:68.3.162.151:3479?transport=udp'}
# The Cloudflare key stays in this file on this box: only its path is passed, to the run's own directory.
CLOUDFLARE_TURN_CONFIG = Path('D:/mx/coturn-20260920/turn-config-cloudflare.json')
TURN_CONF = Path('D:/mx/coturn-20260920/turnserver-fixed.conf')
BOX_LOG = Path('D:/mx/inventory-confirming-2-20260926/steps.log')
SECRET_KEYS = ('NetworkTurnPass', 'NetworkPlayerTurnPass')
# The Linux box (BOXES.md): the lane's directory there, its clone of the tree at the tip and that tree's gcc build.
LINUX_SSH = '3090'
LINUX_LANE = f'/home/erol/cortex-workers/{LANE}'
LINUX_REPO = f'{LINUX_LANE}/repo'
LINUX_BINARY = f'{LINUX_REPO}/build-gcc/CortexCommand'
# The soak (tools/soak_two_peer.py's defaults): an autosave a minute on the host, three 1.5 s stalls of the client, the memory
# census every minute and the full-state hash every second.
SOAK_AUTOSAVE_S, SOAK_HOLDS, SOAK_STALL_MS, SOAK_CENSUS_TICKS, SOAK_FULLSTATE_EVERY = 60, 3, 1500, 3600, 60
MATCH_TICKS = 1200
TICK_MS = 1000 / 60
SCRATCH_LIMIT = 4_000_000_000
MST = dt.timezone(dt.timedelta(hours=-7))
SCENARIOS = {'mp-host-join': 'two-peer service match (host and join), the feel driver measuring both peers',
             'sp-soak': 'single-player FeelBaseline duel on EDITH alone: the sim tick budget'}
DRY_RUN = False


def stamp():
    return dt.datetime.now(MST).strftime('%Y-%m-%d %H:%M:%S MST')


def say(message):
    print(f'[edith-cross] {message}', flush=True)


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def harness(tools):
    """The repo's own harness: this file's directory here, the engine tree's tools on EDITH."""
    if str(tools) not in sys.path:
        sys.path.insert(0, str(tools))
    import feel_measure
    import run_sim_test
    from feel import records, report
    return argparse.Namespace(feel=feel_measure, run=run_sim_test, records=records, report=report)


# --- one peer, on either box ---------------------------------------------------------------------------------------

def match_spec(peer, root, port, role_flags, settings, repo=REPO, ticks=MATCH_TICKS, timeout=420, record=False, lean=True):
    """The feel driver's two-peer arm (feel_measure.launch_case) with the real link in place of the fake lag. Its '-off'
    form by default: at 24dcda80ad the feel recorder stops a match with the [net-plane] ASSERT (FeelAfterPresent reads
    the coordinator inside an open plane window), and item9a_gates reads its clock from the live-hash wall_ms rows.
    lean keeps the relay compare's records (tick hashes, live hashes, the match report); the matrix form adds the
    per-tick sim dump and the controller dump, which cost the tick budget on both boxes."""
    root, run_out = Path(root), Path(root) / peer
    trace = root / f'{peer}_trace.json'
    flags = ['-seed', '42', '-max-ticks', str(ticks), '-tick-hashes', '-net-live-tick-hashes', str(root / f'{peer}-live.jsonl'),
             '-out', str(trace), '-input-script', str(root / 'input.txt'),
             *([] if lean else ['-controller-debug-dump', str(root / f'{peer}_controller.jsonl'), '-controller-debug-ticks', f'1-{ticks}']),
             '-feel-render-settings', str(run_out / 'runtime/Userdata/FeelRender.ini'),
             *(['-feel-measure', str(run_out / 'feel')] if record else []), '-net-match-service-e2e', '-net-port', str(port), '-net-match-ticks', str(ticks), '-net-match-humans', '2',
             '-net-match-peers', '2', '-net-match-cpu-slots', '0', '-net-match-service-preset', 'Determinism FeelBaseline',
             '-net-match-service-module', 'UserScenes.rte', '-net-match-auto-delay', '-net-local-prediction', 'on',
             '-net-reconnect-ticket', str(root / f'{peer}.ticket'), '-net-match-report', str(root / f'{peer}_report.json'),
             *role_flags]
    env = dict(CCCP_HEADLESS='1', PYTHONDONTWRITEBYTECODE='1')
    if not lean:
        env.update(CC_TRACE_PREVIEW_EVENT='1', CC_SIM_DUMP=f'1:{ticks}')
    expected = [str(trace)] + ([] if lean else [str(trace) + '.simdump.txt', str(root / f'{peer}_controller.jsonl')])
    return dict(peer=peer, root=str(root), repo=str(repo), flags=flags, env=env, settings=settings, ticks=ticks, cap=60,
                timeout=timeout, expected=expected, reduce=None, record=record)


def soak_spec(root, ticks, repo=REPO):
    """The feel driver's single-player arm with only the feel recorder on (no tick hashes, no sim dump), so the tick
    it times is the game's own."""
    root, run_out = Path(root), Path(root) / 'sp'
    flags = ['-seed', '42', '-max-ticks', str(ticks), '-out', str(root / 'sp_trace.json'), '-input-script', str(root / 'input.txt'),
             '-feel-render-settings', str(run_out / 'runtime/Userdata/FeelRender.ini'), '-feel-measure', str(run_out / 'feel'),
             '-scenario', 'FeelBaseline']
    return dict(peer='sp', root=str(root), repo=str(repo), flags=flags, env=dict(CCCP_HEADLESS='1', PYTHONDONTWRITEBYTECODE='1'),
                settings={}, ticks=ticks, cap=60, timeout=int(ticks / 60) + 900, expected=[], reduce='tick-budget', record=True)


def prepare_peer(h, spec):
    run_out = Path(spec['root']) / spec['peer']
    run = h.run.make_run(Path(spec['repo']), spec['flags'], run_out, timeout=spec['timeout'], env=spec['env'],
                         expected=[Path(path) for path in spec['expected']])
    h.feel.private_settings(run, spec['cap'])
    if spec.get('record'):
        (run_out / 'feel').mkdir()
    h.feel.stage_baseline(run, spec['ticks'], 2)
    if spec['settings']:
        h.run.seed_settings(run, spec['settings'])
    return run


def redact(h, run, spec, spec_path=None):
    """The TURN password leaves the runtime and the spec once the engine has read it."""
    secrets = {key: 'redacted' for key in SECRET_KEYS if key in spec['settings']}
    if not secrets:
        return
    h.run.seed_settings(run, secrets)
    spec['settings'].update(secrets)
    if spec_path:
        write_json(spec_path, spec)


def remote_peer(spec_path):
    """Runs on EDITH inside session 1: one peer through the runner, then its records packed for the fetch."""
    spec_path = Path(spec_path)
    spec = json.loads(spec_path.read_text(encoding='utf-8'))
    h = harness(Path(spec['repo']) / 'tools')
    root = Path(spec['root'])
    print(f'{stamp()} {spec["peer"]} start', flush=True)
    run = prepare_peer(h, spec)
    try:
        run.start()
        run.finish()
    finally:
        run.close()
        redact(h, run, spec, spec_path)
    record = run.record
    write_json(root / f'{spec["peer"]}-record.json', record)
    print(f'{stamp()} {spec["peer"]} exit={record.get("exit_code")} timed_out={record.get("timed_out")} '
          f'exe={str(record.get("exe_sha256"))[:16]}', flush=True)
    if spec.get('reduce') == 'tick-budget':
        budget = tick_budget(h.records.record_path(root / 'sp/feel/raw.jsonl'), h.records.open_record)
        budget.update(exit_code=record.get('exit_code'), exe_sha256=record.get('exe_sha256'), machine=os.environ.get('COMPUTERNAME'))
        write_json(root / 'tick-budget.json', budget)
        print(f'{stamp()} tick budget {json.dumps(budget)}', flush=True)
    h.records.compress_case_records(root)
    lane_root = next((parent for parent in Path(root).parents if parent.name == LANE), Path(root).parents[-3])
    print(f'{stamp()} scratch bytes {h.feel.scratch_bytes(lane_root, SCRATCH_LIMIT * 10)}', flush=True)
    return 0 if record.get('exit_code') == 0 else 1


def tick_budget(raw, open_record):
    """The sim's share of each main-loop iteration from the engine's feel records: an iteration runs its sim ticks
    before its draw, so the ticks' time is the iteration's begin to its frame's draw begin (or to its end when it drew
    nothing). The engine's pace counters (sim_ms_per_tick) count only lockstep ticks, so a single-player run has no
    other per-tick record."""
    one, multi, idle, total_ms, total_ticks = [], [], [], 0.0, 0
    first = last = previous = frame = None
    with open_record(raw) as stream:
        for line in stream:
            row = json.loads(line)
            kind = row.get('type')
            if kind == 'frame':
                frame = row
                continue
            if kind != 'iteration':
                continue
            drawn, frame = frame, None
            if not row.get('active'):
                previous = row['tick']
                continue
            tick = row['tick']
            if first is None:
                first = (tick, row['begin_ms'])
            last = (tick, row['end_ms'])
            if previous is not None:
                step = tick - previous
                drew = drawn is not None and row['begin_ms'] <= drawn['draw_begin_ms'] <= row['end_ms']
                spent = (drawn['draw_begin_ms'] if drew else row['end_ms']) - row['begin_ms']
                if step == 1:
                    one.append((spent, tick))
                elif step > 1:
                    multi.append((spent / step, tick, step))
                elif not drew:
                    idle.append(spent)
                if step >= 1:
                    total_ms += spent
                    total_ticks += step
            previous = tick
    ordered = sorted(value for value, _ in one)
    pick = lambda share: ordered[min(len(ordered) - 1, int(len(ordered) * share))] if ordered else None
    worst = max(one, default=(None, None))
    wall_s = (last[1] - first[1]) / 1000 if first and last else None
    return dict(method='feel records: one-tick iterations, begin_ms to the frame draw_begin_ms (or end_ms); the engine counts '
                       'sim time (pace sim_ms_per_tick) only in lockstep',
                first_tick=first[0] if first else None, last_tick=last[0] if last else None, wall_s=wall_s,
                wall_tps=(last[0] - first[0]) / wall_s if wall_s else None, one_tick_iterations=len(one),
                mean_ms=statistics.fmean(ordered) if ordered else None, p50_ms=pick(.5), p90_ms=pick(.9), p99_ms=pick(.99),
                p999_ms=pick(.999), max_ms=worst[0], max_tick=worst[1],
                over_budget_ticks=sum(value > TICK_MS for value in ordered), budget_ms=TICK_MS,
                multi_tick_iterations=len(multi), multi_tick_max_ms_per_tick=max((row[0] for row in multi), default=None),
                multi_tick_worst=max(multi, default=None), all_ticks_mean_ms=total_ms / total_ticks if total_ticks else None,
                loop_overhead_ms=statistics.fmean(idle) if idle else None, idle_iterations=len(idle))


# --- EDITH over ssh --------------------------------------------------------------------------------------------------

def box():
    return RemoteBox('edith', TASK, SESSION1_SCRIPT, dry_run=DRY_RUN, say=say)


def run_local(argv, timeout=120, check=True, what=None):
    return box().run_local(argv, timeout, check, what)


def ssh(command, timeout=120, check=True):
    return box().ssh(command, timeout, check)


def scp_to(local, remote):
    box().scp_to(local, remote)


def scp_from(remote, local):
    box().scp_from(remote, local)


def remote_mkdir(path):
    box().mkdir(path)


def exe_hashes(repo, remote_repo=None):
    local = hashlib.sha256((Path(repo) / 'Cortex Command.exe').read_bytes()).hexdigest()
    return local, box().sha256(Path(remote_repo or repo) / 'Cortex Command.exe')


def ship_driver():
    remote_mkdir(PAYLOAD / 'edith')
    scp_to(Path(__file__), PAYLOAD / 'edith_cross.py')
    scp_to(HERE / 'edith/remote_box.py', PAYLOAD / 'edith/remote_box.py')


def wait_task_idle(budget_s=900):
    return box().wait_task_idle(budget_s)


def start_session1(root, spec, label):
    """Ships the spec and the rendered payload, then starts the one session-1 task."""
    root = Path(root)
    spec_path = root / f'{spec["peer"]}-spec.json'
    if DRY_RUN:
        say(f'dry-run: EDITH {spec["peer"]} spec {spec_path} through {TASK} ({SESSION1_SCRIPT}): {" ".join(spec["flags"])}')
        say(f'dry-run: EDITH {spec["peer"]} settings ' + json.dumps({key: ('redacted' if key in SECRET_KEYS else value)
                                                                     for key, value in spec['settings'].items()}))
        return
    local_spec = root / f'{label}-spec.local.json'
    write_json(local_spec, spec)
    script = (HERE / 'edith/session1.ps1').read_text(encoding='utf-8')
    for key, value in dict(REPO=spec['repo'], LOG=root / 'session1.log', DONE=root / 'session1.done',
                           DRIVER=PAYLOAD / 'edith_cross.py', SPEC=spec_path).items():
        script = script.replace('{{' + key + '}}', Path(value).as_posix() if key != 'REPO' else str(value))
    local_script = root / f'{label}-session1.ps1'
    local_script.write_text(script, encoding='utf-8')
    scp_to(local_spec, spec_path)
    redacted = dict(spec, settings={key: ('redacted' if key in SECRET_KEYS else value) for key, value in spec['settings'].items()})
    write_json(local_spec, redacted)
    box().start_task(local_script)
    say(f'{label}: {spec["peer"]} started on EDITH through {TASK}')


def soak_stalls(ticks):
    return [ticks * (index + 1) // (SOAK_HOLDS + 1) for index in range(SOAK_HOLDS)]


def soak_flags(peer, ticks):
    """What the soak adds to a match peer: soak_two_peer's census, full-state hashing, the host's autosaves, the client's stalls."""
    flags = ['-memory-census-ticks', str(SOAK_CENSUS_TICKS), '-net-fullstate-hash-every', str(SOAK_FULLSTATE_EVERY)]
    if peer == 'host':
        return flags + ['-net-autosave-seconds', str(SOAK_AUTOSAVE_S)]
    return flags + [part for tick in soak_stalls(ticks) for part in ('-net-test-live-stall', f'{tick}:{SOAK_STALL_MS}')]


class LinuxPeer:
    """A match peer on the Linux box: its spec with this box's run root mapped to the lane's there, this file's remote-peer path
    run over ssh (the POSIX runner, the tree's gcc build), its files fetched back here when it ends."""

    def __init__(self, spec, local_root, log_path):
        self.local_root = Path(local_root)
        self.remote_root = f'{LINUX_LANE}/runs/{self.local_root.parent.name}/{self.local_root.name}'
        local = str(self.local_root)

        def posix(value):
            return value.replace(local, self.remote_root).replace('\\', '/') if isinstance(value, str) and local in value else value
        self.spec = dict(spec, root=self.remote_root, repo=LINUX_REPO, flags=[posix(flag) for flag in spec['flags']],
                         expected=[posix(path) for path in spec['expected']], env=dict(spec['env'], CCCP_TEST_BINARY=LINUX_BINARY))
        self.peer, self.record, self.log_path, self.process = spec['peer'], {}, Path(log_path), None
        if DRY_RUN:
            say(f'dry-run: Linux {self.peer} under {self.remote_root}: ' + ' '.join(self.spec['flags']))
            return
        spec_path = self.local_root / f'{self.peer}-spec.linux.json'
        write_json(spec_path, self.spec)
        remote_spec = f'{self.remote_root}/{self.peer}-spec.json'
        payload = f'{LINUX_LANE}/payload'
        run([*SSH_LINUX, f'mkdir -p {self.remote_root} {payload}/edith'])
        for source, target in ((spec_path, remote_spec), (self.local_root / 'input.txt', f'{self.remote_root}/input.txt'),
                               (self.local_root / 'input-schedule.json', f'{self.remote_root}/input-schedule.json'),
                               (Path(__file__), f'{payload}/edith_cross.py'), (HERE / 'edith/remote_box.py', f'{payload}/edith/remote_box.py')):
            run(['scp', '-q', '-o', 'BatchMode=yes', str(source), f'{LINUX_SSH}:{target}'])
        command = (f'cd {LINUX_REPO} && export CCCP_HEADLESS=1 PYTHONDONTWRITEBYTECODE=1 CCCP_TEST_BINARY={LINUX_BINARY} '
                   f'CC_EDITH_CROSS_LANE={LANE} DISPLAY=${{DISPLAY:-:0}} && python3 {payload}/edith_cross.py --remote-peer {remote_spec}')
        self.process = subprocess.Popen([*SSH_LINUX, command], stdin=subprocess.DEVNULL, stdout=self.log_path.open('w'),
                                        stderr=subprocess.STDOUT, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        say(f'{self.peer} started on the Linux box (ssh pid {self.process.pid})')

    def poll(self):
        return self.process.poll() if self.process else 0

    def finish(self):
        return self.process.wait() if self.process else 0

    def close(self):
        if not self.process:
            return
        if self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=30)
        # The tar stream only; its runtime is a link into the tree, so it never travels.
        with subprocess.Popen([*SSH_LINUX, f'tar -C {self.remote_root} --exclude=./{self.peer}/runtime -cf - .'],
                              stdout=subprocess.PIPE, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0)) as source:
            subprocess.run([WINDOWS_TAR, '-xf', '-', '-C', str(self.local_root)], stdin=source.stdout, check=False,
                           creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        record = self.local_root / f'{self.peer}-record.json'
        self.record = json.loads(record.read_text(encoding='utf-8')) if record.is_file() else {}


SSH_LINUX = ['ssh', '-o', 'BatchMode=yes', LINUX_SSH]


def run(argv):
    result = subprocess.run(argv, capture_output=True, text=True, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    if result.returncode != 0:
        raise RuntimeError(f'{argv[0]} failed ({result.returncode}): {SSH_NOISE.sub("", result.stderr)[-300:]}')
    return result.stdout


class LinuxTunnel:
    """ssh -R: the Linux box's 127.0.0.1:<the directory port> reaches the directory on this box's loopback (signalling only)."""

    def __init__(self, log_path):
        self.log_path, self.process = Path(log_path), None

    def open(self):
        argv = ['ssh', '-N', '-o', 'ExitOnForwardFailure=yes', '-o', 'BatchMode=yes', '-R',
                f'127.0.0.1:{DIRECTORY_PORT}:127.0.0.1:{DIRECTORY_PORT}', LINUX_SSH]
        self.process = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=self.log_path.open('w'), stderr=subprocess.STDOUT,
                                        creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        time.sleep(4)
        probe = run([*SSH_LINUX, f"python3 -c \"import socket; s=socket.socket(); s.settimeout(3); print('open' if s.connect_ex(('127.0.0.1', {DIRECTORY_PORT})) == 0 else 'closed')\""]).strip()
        if self.process.poll() is not None or probe != 'open':
            raise RuntimeError(f'the ssh -R tunnel did not open on the Linux box (probe {probe}); see {self.log_path}')

    def close(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=10)


def soak_verdict(root, ticks):
    """soak_two_peer's own judgement of the fetched pair: history and pace per engine (through pace_verdict), holds after returns
    (excused only for a slow machine whose sim does not fit), autosaves owed and published, and every own-seat hold of the client
    that follows an autosave by at most 300 ticks (H7c)."""
    sys.path.insert(0, str(HERE))
    import soak_two_peer as soak
    history = soak.acceptance_history(Path(root), ticks)
    holds = {peer: soak.return_hold_violations((Path(root) / peer / 'stdout.log').read_text(encoding='utf-8-sig', errors='replace'))
             for peer in ('host', 'client')}
    kept = {peer: soak.excused_return_holds(Path(root), rows)[1] for peer, rows in holds.items()}
    host_log = (Path(root) / 'host/stdout.log').read_text(encoding='utf-8', errors='replace')
    client_log = (Path(root) / 'client/stdout.log').read_text(encoding='utf-8', errors='replace')
    autosaves = sum(line.startswith('[autosave] tick=') and 'capture_ms=' in line for line in host_log.splitlines())
    owed = int(ticks // (60 * SOAK_AUTOSAVE_S)) - 1
    saves = [int(match) for match in re.findall(r'^\[autosave\] tick=(\d+)', host_log, re.M)]
    own_holds = [int(match) for match in re.findall(r'hold of this seat at (\d+)', client_log)]
    after_saves = [tick for tick in own_holds if any(0 <= tick - save <= 300 for save in saves)]
    pace = {peer: dict(windows=len(rows), failed=[(row['first'], round(row['wall_tps'] or 0, 2), row.get('sim_ms_per_tick')) for row in rows if not row['passed']])
            for peer, rows in history['pace_windows'].items()}
    checks = dict(complete_history_and_pace=history['pass'], no_hold_after_return=not any(kept.values()), autosaves=autosaves >= max(0, owed),
                  no_hold_after_autosave=not after_saves)
    return dict(passed=all(checks.values()), checks=checks, errors_not_pace=[e for e in history['errors'] if 'pace window' not in e], pace=pace,
                holds_after_returns=kept, autosaves=f'{autosaves}/{owed}', client_own_holds=own_holds, client_holds_after_autosaves=after_saves,
                stalls=soak_stalls(ticks))


def wait_done(root, budget_s):
    """Waits on the payload's done file; each ssh call stays under ten minutes."""
    return box().wait_done(Path(root) / 'session1.done', budget_s)


def fetch(root, names_like, excludes):
    """Packs the EDITH peer's files (never its runtime, whose Data is a junction) and unpacks them here."""
    box().fetch_tar(root, names_like, excludes)


# --- the rendezvous on this box ---------------------------------------------------------------------------------------

def make_cert(root):
    """A throwaway certificate for the loopback directory, pinned by both peers (tools/feel/relay_join.py's recipe)."""
    import ipaddress
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'localhost')])
    now = dt.datetime.now(dt.timezone.utc)
    certificate = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
                   .serial_number(x509.random_serial_number()).not_valid_before(now - dt.timedelta(minutes=1))
                   .not_valid_after(now + dt.timedelta(days=1)).add_extension(x509.SubjectAlternativeName([
                       x509.DNSName('localhost'), x509.IPAddress(ipaddress.ip_address('127.0.0.1'))]), critical=False)
                   .sign(key, hashes.SHA256()))
    cert, key_path = Path(root) / 'cert.pem', Path(root) / 'key.pem'
    cert.write_bytes(certificate.public_bytes(serialization.Encoding.PEM))
    key_path.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    return cert, key_path, hashlib.sha256(certificate.public_bytes(serialization.Encoding.DER)).hexdigest()


class Tunnel:
    """ssh -R: EDITH's 127.0.0.1:<its directory port> reaches the directory on this box's loopback (signalling only); with the
    bridge, EDITH's 127.0.0.1:<bridge TCP port> reaches this box's end of the relay bridge too."""

    def __init__(self, log_path, bridge=False):
        self.log_path, self.process, self.bridge = Path(log_path), None, bridge

    def open(self):
        forwards = [DIRECTORY_PORT] + ([BRIDGE_TCP] if self.bridge else [])
        argv = ['ssh', '-N', '-o', 'ExitOnForwardFailure=yes',
                *[part for port in forwards for part in ('-R', f'127.0.0.1:{EDITH_TCP[port]}:127.0.0.1:{port}')], 'edith']
        if DRY_RUN:
            say('dry-run: ' + ' '.join(argv))
            return
        self.process = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=self.log_path.open('w'), stderr=subprocess.STDOUT,
                                        creationflags=subprocess.CREATE_NO_WINDOW)
        time.sleep(4)
        probe = ssh(f"$c = New-Object Net.Sockets.TcpClient; try {{ $c.Connect('127.0.0.1', {EDITH_TCP[DIRECTORY_PORT]}); 'open' }} "
                    f"catch {{ 'closed' }} finally {{ $c.Close() }}").strip()
        if self.process.poll() is not None or probe != 'open':
            raise RuntimeError(f'the ssh -R tunnel did not open on EDITH (probe {probe}); see {self.log_path}')

    def close(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=10)


def read_exact(connection, size):
    data = b''
    while len(data) < size:
        chunk = connection.recv(size - len(data))
        if not chunk:
            return None
        data += chunk
    return data


def pump_frames(connection, deliver, counts, key):
    """TCP frames (2-byte length + datagram) from the tunnel, each handed on as one datagram."""
    import struct
    try:
        while (header := read_exact(connection, 2)) is not None:
            datagram = read_exact(connection, struct.unpack('>H', header)[0])
            if datagram is None:
                return
            deliver(datagram)
            counts[key] += 1
    except OSError:
        return


def bridge_edith(bind, udp_port=None, tcp_port=None):
    """Runs on EDITH in the ssh session: the engine's TURN socket talks UDP to <bind>:<udp_port> (EDITH's own LAN address:
    the ICE sockets are bound to interface addresses, so a loopback TURN server is never tried); each source address
    gets its own TCP stream through the ssh -R forward to this box's end, which speaks UDP to the TURN server."""
    import socket
    import struct
    import threading
    udp_port, tcp_port = udp_port or BRIDGE_UDP, tcp_port or EDITH_TCP[BRIDGE_TCP]
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((bind, udp_port))
    flows, counts = {}, dict(up=0, down=0, flows=0)
    print(f'{stamp()} bridge listening udp {bind}:{udp_port} -> tcp 127.0.0.1:{tcp_port}', flush=True)
    last = time.monotonic()
    while True:
        try:
            datagram, source = sock.recvfrom(65535)
        except ConnectionResetError:
            continue
        stream = flows.get(source)
        if stream is None:
            stream = socket.create_connection(('127.0.0.1', tcp_port))
            stream.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            flows[source] = stream
            counts['flows'] += 1
            threading.Thread(target=pump_frames, args=(stream, lambda data, to=source: sock.sendto(data, to), counts, 'down'), daemon=True).start()
        stream.sendall(struct.pack('>H', len(datagram)) + datagram)
        counts['up'] += 1
        if time.monotonic() - last > 10:
            print(f'{stamp()} bridge {json.dumps(counts)}', flush=True)
            last = time.monotonic()


class BridgeHere:
    """This box's end of the relay bridge: each tunnel stream becomes one UDP flow to the TURN server."""

    def __init__(self, turn_url, log_path, bind):
        import socket
        host, port = re.match(r'turn:([^:?]+):(\d+)', turn_url).groups()
        self.target, self.log_path = (host, int(port)), Path(log_path)
        self.counts = dict(up=0, down=0, flows=0)
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(('127.0.0.1', BRIDGE_TCP))
        self.listener.listen(8)
        self.remote, self.bind = None, bind

    def serve(self):
        import socket
        import struct
        import threading
        while True:
            try:
                stream, _ = self.listener.accept()
            except OSError:
                return
            stream.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            udp.connect(self.target)
            self.counts['flows'] += 1

            def back(udp=udp, stream=stream):
                try:
                    while True:
                        datagram = udp.recv(65535)
                        stream.sendall(struct.pack('>H', len(datagram)) + datagram)
                        self.counts['down'] += 1
                except OSError:
                    return
            threading.Thread(target=pump_frames, args=(stream, udp.send, self.counts, 'up'), daemon=True).start()
            threading.Thread(target=back, daemon=True).start()

    def open(self):
        import threading
        threading.Thread(target=self.serve, daemon=True).start()
        argv = ['ssh', 'edith', f"python '{(PAYLOAD / 'edith_cross.py').as_posix()}' --bridge-edith --bridge-bind {self.bind}"]
        self.remote = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=self.log_path.open('w'), stderr=subprocess.STDOUT,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
        time.sleep(4)
        if self.remote.poll() is not None:
            raise RuntimeError(f'the EDITH end of the relay bridge exited; see {self.log_path}')

    def close(self):
        if self.remote and self.remote.poll() is None:
            self.remote.terminate()
            self.remote.wait(timeout=10)
        ssh("Get-CimInstance Win32_Process -Filter \"Name='python.exe'\" | Where-Object { $_.CommandLine -like '*edith_cross.py*--bridge-edith*' } "
            "| ForEach-Object { Stop-Process -Id $_.ProcessId -Force }", check=False)
        self.listener.close()
        with self.log_path.open('a') as stream:
            stream.write(f'{stamp()} bridge here {json.dumps(self.counts)} target={self.target[0]}:{self.target[1]}\n')


def wait_session(directory, budget_s, alive):
    deadline = time.monotonic() + budget_s
    while time.monotonic() < deadline:
        try:
            rows = directory.list_sessions(DIRECTORY_PORT)
        except Exception:  # the directory answers once the host registers; transient refusals are retried
            rows = []
        if rows:
            return rows[0]['session_id']
        if not alive():
            return None
        time.sleep(1)
    return None


def turn_login():
    for line in TURN_CONF.read_text(encoding='utf-8', errors='replace').splitlines():
        stripped = line.strip()
        if stripped.startswith('user=') and ':' in stripped:
            user, secret = stripped[5:].split(':', 1)
            return user, secret
    raise RuntimeError(f'{TURN_CONF} has no user=<name>:<password> line')


def network_settings(path, side, pin, login, peer='host'):
    if path == 'ip':
        return {'NetworkIceEnable': '0'}
    rendezvous = {'SessionDirectoryUrl': f'127.0.0.1:{DIRECTORY_PORT if side == "here" else EDITH_TCP[DIRECTORY_PORT]}', 'SessionDirectoryCertSha256': pin,
                  'SessionDirectoryInstallKey': f'edith-cross-{side}-install'}
    if path == 'direct':
        return {**rendezvous, 'NetworkIceEnable': '1', 'NetworkConnectionMode': 'DirectOnly', 'NetworkHostRelayMode': 'Off'}
    if path == 'directory-relay':
        # The host asks the directory for the relay; the client may take only the relay it was handed.
        return {**rendezvous, 'NetworkIceEnable': '1', 'NetworkHostRelayMode': 'Directory',
                'NetworkConnectionMode': 'Automatic' if peer == 'host' else 'RelayOnly'}
    user, secret = login
    return {**rendezvous, 'NetworkIceEnable': '1', 'NetworkStunServers': '', 'NetworkConnectionMode': 'RelayOnly',
            'NetworkHostRelayMode': 'Fixed', 'NetworkTurnServers': TURN[side], 'NetworkTurnUser': user, 'NetworkTurnPass': secret,
            'NetworkPlayerTurnServers': TURN[side], 'NetworkPlayerTurnUser': user, 'NetworkPlayerTurnPass': secret}


# --- the checks before any launch ----------------------------------------------------------------------------------

def box_free():
    """No engine here while the confirming inventory's feel matrix (its S3 stream) runs."""
    if not BOX_LOG.is_file():
        return True
    marks = [line for line in BOX_LOG.read_text(encoding='utf-8', errors='replace').splitlines() if re.search(r'\] S3 (start|exit)', line)]
    return not (marks and marks[-1].rstrip().endswith('S3 start'))


def box_load():
    """Compilers, linkers and engines running on this box right now (other lanes' builds skew the local peer's tick)."""
    names = subprocess.run(['pwsh', '-NoProfile', '-Command', "(Get-Process cl, link, 'Cortex Command*' -ErrorAction SilentlyContinue).ProcessName"],
                           capture_output=True, text=True, creationflags=subprocess.CREATE_NO_WINDOW).stdout.splitlines()
    names = [name.strip() for name in names if name.strip()]
    return {name: names.count(name) for name in sorted(set(names))}


def wait_quiet(budget_s):
    """Waits (bounded) until no compiler or linker runs here; returns what was running when the run starts."""
    deadline = time.monotonic() + budget_s
    while (load := box_load()) and any(name in load for name in ('cl', 'link')) and time.monotonic() < deadline:
        say(f'a build runs on this box {load}; waiting')
        time.sleep(30)
    return load


def wait_box(budget_s):
    deadline = time.monotonic() + budget_s
    while not box_free():
        if time.monotonic() > deadline:
            return False
        say(f'the inventory feel matrix holds this box ({BOX_LOG}); waiting')
        time.sleep(60)
    return True


# --- one match ---------------------------------------------------------------------------------------------------

def run_match(h, options, index, login):
    name = f'{options.direction}-{path_label(options)}-{index}'
    root = (options.out / name).resolve()
    port = GAME_PORT + (index - 1) % 10
    host_side = 'here' if options.direction == 'host-here' else 'edith'
    sides = {'host': host_side, 'client': 'edith' if host_side == 'here' else 'here'}
    local_peer = 'host' if host_side == 'here' else 'client'
    remote_peer_name = 'client' if local_peer == 'host' else 'host'
    machines = {peer: 'LINUX-3090' if peer == 'client' and options.client_box == 'linux' else MACHINE[side] for peer, side in sides.items()}
    say(f'{name}: host={machines["host"]} client={machines["client"]} path={options.path} port={port} {stamp()}')
    if DRY_RUN:
        root = options.out / name
    else:
        root.mkdir(parents=True, exist_ok=False)
        looped_input(h, root / 'input.txt', match_ticks(options))
    remote_mkdir(root)
    if not DRY_RUN:
        scp_to(root / 'input.txt', root / 'input.txt')
        scp_to(root / 'input-schedule.json', root / 'input-schedule.json')
    directory = service = pin = None
    if options.path != 'ip':
        import test_directory_ice_join as directory
        if DRY_RUN:
            pin = '<pin>'
            say(f'dry-run: session directory on 127.0.0.1:{DIRECTORY_PORT} with a fresh certificate under {root}')
        else:
            cert, key, pin = make_cert(root)
            service = directory.start_service(root, DIRECTORY_PORT, cert, key,
                                              ('--turn-config', str(options.turn_config), '--turn-max-ttl', str(options.relay_ttl))
                                              if options.path == 'directory-relay' else ())

    def role(peer, session_id=None):
        ice = ['-net-ice', 'off' if options.path == 'ip' else 'on']
        if peer == 'host':
            return ['-net-host', '-net-replay-out', str(root / 'match.ccreplay'), *ice]
        if options.path == 'ip':
            return ['-net-join', ADDRESS[sides['host']], *ice]
        return ['-net-join-session', session_id or '<session id>', *ice]

    def spec(peer, session_id=None):
        made = match_spec(peer, root, port, role(peer, session_id), network_settings(options.path, sides[peer], pin, login, peer),
                          repo=options.remote_repo or options.repo if sides[peer] == 'edith' else options.repo,
                          ticks=match_ticks(options), timeout=match_timeout(options), record=options.feel_records,
                          lean=options.instrumentation == 'lean')
        if options.soak:
            made['flags'] += soak_flags(peer, match_ticks(options))
        return made

    load = {} if DRY_RUN else wait_quiet(options.quiet_wait)
    started, local, local_record, session_id, note = stamp(), None, {}, None, None
    try:
        if local_peer == 'host':
            local = launch_local(h, spec('host'))
            if options.path != 'ip':
                session_id = '<session id>' if DRY_RUN else wait_session(directory, 60, lambda: local.poll() is None)
                if not session_id:
                    note = 'the host published no directory row within 60 s'
            else:
                time.sleep(0 if DRY_RUN else 2)
            if not note:
                start_session1(root, spec('client', session_id), name)
        else:
            start_session1(root, spec('host'), name)
            if options.path != 'ip':
                session_id = '<session id>' if DRY_RUN else wait_session(directory, 120, lambda: True)
                if not session_id:
                    note = 'the EDITH host published no directory row within 120 s'
            else:
                time.sleep(0 if DRY_RUN else 20)
            if not note:
                local = LinuxPeer(spec('client', session_id), root, root / 'client-linux.log') if options.client_box == 'linux' else launch_local(h, spec('client', session_id))
        if local is not None and not DRY_RUN and not note:
            local.finish()
        remote_state = wait_done(root, match_timeout(options) + 300) if not (note and local_peer == 'host') else 'not started'
    finally:
        if local is not None and not DRY_RUN:
            local.close()
            local_spec = spec(local_peer, session_id)
            if not isinstance(local, LinuxPeer):
                redact(h, local, local_spec)
            local_record = local.record
            write_json(root / f'{local_peer}-record.json', local_record)
            write_json(root / f'{local_peer}-spec.json', dict(local_spec, settings={key: ('redacted' if key in SECRET_KEYS else value)
                                                                                   for key, value in local_spec['settings'].items()}))
        if service is not None:
            service.terminate()
            service.wait(timeout=10)
    if DRY_RUN:
        return dict(name=name, dry_run=True)
    if remote_state != 'not started':
        fetch(root, [f'{remote_peer_name}*', 'session1*'], [f'{remote_peer_name}/runtime'])
    if options.soak:
        verdict = soak_verdict(root, match_ticks(options))
        write_json(root / 'soak-verdict.json', verdict)
        say(f'{name}: SOAK {"PASS" if verdict["passed"] else "FAIL"} {json.dumps(verdict["checks"])} autosaves={verdict["autosaves"]} '
            f'client_holds_after_autosaves={verdict["client_holds_after_autosaves"]}')
    return analyze_match(h, root, dict(name=name, started=started, finished=stamp(), direction=options.direction, path=options.path, ticks=match_ticks(options),
                                       port=port, machines=machines,
                                       local_peer=local_peer, session_id=session_id, remote_state=remote_state, note=note,
                                       feel_records=options.feel_records, instrumentation=options.instrumentation, box_here_at_start=load))


def path_label(options):
    return options.path + ('-bridge' if options.relay_bridge else '')


def launch_local(h, spec):
    if DRY_RUN:
        say(f'dry-run: local {spec["peer"]} through make_run: {" ".join(spec["flags"])}')
        return None
    run = prepare_peer(h, spec)
    run.start()
    say(f'{spec["peer"]} started here (pid {run.record.get("pid")})')
    return run


def peer_log(root, peer):
    path = Path(root) / peer / 'stdout.log'
    return path.read_text(encoding='utf-8-sig', errors='replace') if path.is_file() else ''


def live_ticks(path):
    from feel.retained_resume import read_live_hashes
    return {row['tick'] for row in read_live_hashes(Path(path)) if 'tick' in row}


def read_json(path):
    path = Path(path)
    try:
        return json.loads(path.read_text(encoding='utf-8-sig')) if path.is_file() else {}
    except ValueError:
        return {}


def find_key(node, key):
    if isinstance(node, dict):
        if key in node:
            return node[key]
        for value in node.values():
            found = find_key(value, key)
            if found is not None:
                return found
    elif isinstance(node, list):
        for value in node:
            found = find_key(value, key)
            if found is not None:
                return found
    return None


ROUTE = re.compile(r'\[net-ice\][^\n]*(?:selected|candidate|fail|timeout|retrying)[^\n]*|\[net-route\][^\n]*|\[net-transport\][^\n]*'
                   r'|\[net-session\] admission refused[^\n]*|\[net-match-service-e2e\] setup failed[^\n]*'
                   r'|[^\n]*(?:ProblemDetectedLocally|ClosedByPeer|ConnectionState|connect(?:ion)? (?:failed|timed out|refused))[^\n]*', re.I)


def analyze_match(h, root, meta):
    records = {peer: read_json(root / f'{peer}-record.json') for peer in ('host', 'client')}
    complete = all(row.get('exit_code') == 0 and row.get('evidence_complete') and not row.get('timed_out') for row in records.values())
    manifest = dict(meta, mode='two-machine service e2e, EROL-PC <-> EDITH over the internet', ticks=meta.get('ticks', MATCH_TICKS), lag_ms=0, cap_hz=60,
                    instrumentation=meta['feel_records'], loss_percent=0, silent_tick=None, live_stalls=None, autosave_seconds=None,
                    per_peer_lag_ms={'host': 0, 'client': 0}, launches_complete=complete,
                    exe={peer: row.get('exe_sha256') for peer, row in records.items()})
    write_json(root / 'manifest.json', manifest)
    h.records.compress_case_records(root)
    try:
        result = h.feel.reduce_timing_case(root)
        result['peers']['client'] = h.feel.timing_peer(root, 'client')
        result['item9a_pass'] = all(value['pass_check'] for value in result['peers'].values())
    except Exception as error:  # a pair that never matched still gets its verdict line
        result = dict(name=meta['name'], peers={}, proof={}, off_wire_pass=False, reduction_error=f'{type(error).__name__}: {error}')
    write_json(root / 'feel-report.json', result)
    peers = {}
    for peer in ('host', 'client'):
        log = peer_log(root, peer)
        live = root / f'{peer}-live.jsonl'
        ticks = live_ticks(live)
        waits = [(int(frame), int(ms)) for frame, ms in re.findall(r'\[net-frame-wait\] frame=(\d+) wait_ms=(\d+)', log)]
        metrics = result.get('peers', {}).get(peer, {}).get('metrics', {})
        report = read_json(root / f'{peer}_report.json')
        pace = find_key(report, 'pace') or {}
        peers[peer] = dict(machine=meta['machines'][peer], exit_code=records[peer].get('exit_code'), timed_out=records[peer].get('timed_out'),
                           exe=str(records[peer].get('exe_sha256'))[:16], live_ticks=len(ticks), last_tick=max(ticks, default=None),
                           waits_over_50=sum(ms > 50 for _, ms in waits), steady_waits_over_50=sum(ms > 50 for frame, ms in waits if frame > 300),
                           waits=len(waits), longest_steady_wait_ms=metrics.get('longest_stall_ms'),
                           waiting_percent=(100 * metrics['net_wait_ms'] / metrics['steady_wall_ms']) if metrics.get('steady_wall_ms') else None,
                           wall_tps=metrics.get('steady_wall_tps'), sim_ms_per_tick=pace.get('sim_ms_per_tick'),
                           tick_overruns=find_key(report, 'local_tick_overruns'), input_delays=metrics.get('peer_input_delays'),
                           holds=len(re.findall(r'\[net-match\] hold peer=\d+ frame=\d+', log)),
                           route=[line.strip()[:220] for line in ROUTE.findall(log)][:12])
    passes = (result.get('proof') or {}).get('live_passes', {}).get('host/client') or []
    compared = sum(row['compared_ticks'] for row in passes)
    mismatched = sum(row['mismatched_ticks'] + row['mismatched_applied_input_ticks'] for row in passes)
    holds = sum(row['holds'] for row in peers.values())
    # A soak plans its holds: they are judged by its own rules, every other run by holds == 0.
    soak_plan = read_json(root / 'soak-verdict.json')
    hold_judgement = None
    if soak_plan:
        import soak_two_peer
        hold_judgement = soak_two_peer.soak_hold_judgement(root, soak_plan.get('stalls', []))
    holds_pass = hold_judgement['passed'] if hold_judgement else holds == 0
    passed = bool(complete and result.get('off_wire_pass') and passes and mismatched == 0 and holds_pass)
    verdict = dict(name=meta['name'], passed=passed, compared_ticks=compared, desyncs=mismatched, holds=holds, hold_judgement=hold_judgement,
                   trace_pair_pass=(result.get('proof') or {}).get('sim_gated_pass'), peers=peers, manifest=manifest)
    write_json(root / 'verdict.json', verdict)
    cell = lambda key, fmt='{}': '/'.join('-' if peers[peer][key] is None else fmt.format(peers[peer][key]) for peer in ('host', 'client'))
    route = next((line for peer in (meta['local_peer'], 'host', 'client') for line in peers[peer]['route'] if 'selected' in line or 'RouteAllowed' in line), None)
    route = route or next((f'{peer}: {line}' for peer in ('client', 'host') for line in peers[peer]['route']), None)
    say(f'RUN {meta["name"]} {"PASS" if passed else "FAIL"} host@{peers["host"]["machine"]} {peers["host"]["live_ticks"]}/{mismatched} '
        f'client@{peers["client"]["machine"]} {peers["client"]["live_ticks"]}/{mismatched} compared={compared} desyncs={mismatched} '
        f'holds={holds}' + (f' (planned {len(hold_judgement["planned"])}, explained {len(hold_judgement["explained"])}, unexplained {len(hold_judgement["unexplained"])})' if hold_judgement else '') +
        f' exits={cell("exit_code")} waits>50ms={cell("waits_over_50")} (steady {cell("steady_waits_over_50")}) '
        f'longest_ms={cell("longest_steady_wait_ms", "{:.0f}")} waiting%={cell("waiting_percent", "{:.2f}")} tps={cell("wall_tps", "{:.1f}")} '
        f'sim_ms/tick={cell("sim_ms_per_tick", "{:.2f}")} delays={cell("input_delays")} route={route or meta.get("note") or "none logged"}')
    return verdict


# --- the soak -------------------------------------------------------------------------------------------------------

def match_ticks(options):
    return int(round(options.match_minutes * 60 * 60)) if options.match_minutes else MATCH_TICKS


def match_timeout(options):
    # A longer match keeps the per-engine margin the 1200-tick arm has over its own length.
    return max(options.timeout, match_ticks(options) // 60 + 400) if options.match_minutes else options.timeout


def looped_input(h, path, ticks):
    """The feel driver's 1200-tick input pattern repeated to the soak's length."""
    h.feel.input_pattern(path)
    rows = [line.split(' ', 2) for line in path.read_text(encoding='utf-8').splitlines() if line.strip()]
    looped = [f'{int(first) + offset} {min(ticks, int(last) + offset)} {action}'
              for offset in range(0, ticks, MATCH_TICKS) for first, last, action in rows if int(first) + offset <= ticks]
    path.write_text('\n'.join(looped) + '\n', encoding='utf-8')


def run_soak(h, options):
    root = options.out.resolve()
    ticks = int(round(options.minutes * 60 * 60))
    spec = soak_spec(root, ticks, options.repo)
    say(f'soak on EDITH: {ticks} ticks ({options.minutes} min) under {root} {stamp()}')
    if not DRY_RUN:
        root.mkdir(parents=True, exist_ok=False)
        looped_input(h, root / 'input.txt', ticks)
    remote_mkdir(root)
    if not DRY_RUN:
        scp_to(root / 'input.txt', root / 'input.txt')
    start_session1(root, spec, 'soak')
    state = wait_done(root, spec['timeout'] + 1800)
    say(f'soak payload: {state}')
    fetch(root, ['tick-budget.json', 'sp*', 'session1*', 'compressed-records.json'], ['sp/runtime', 'sp/feel'])
    if DRY_RUN:
        return 0
    budget = read_json(root / 'tick-budget.json')
    record = read_json(root / 'sp-record.json')
    fmt = lambda key: '-' if budget.get(key) is None else f'{budget[key]:.2f}'
    say(f'SOAK EDITH sp {budget.get("first_tick")}..{budget.get("last_tick")} exit={record.get("exit_code")} '
        f'sim tick mean={fmt("mean_ms")} ms p50={fmt("p50_ms")} p99={fmt("p99_ms")} p99.9={fmt("p999_ms")} max={fmt("max_ms")} ms '
        f'(tick {budget.get("max_tick")}) over {TICK_MS:.2f} ms={budget.get("over_budget_ticks")} of {budget.get("one_tick_iterations")} '
        f'all-ticks mean={fmt("all_ticks_mean_ms")} wall_tps={fmt("wall_tps")} exe={str(record.get("exe_sha256"))[:16]}')
    return 0 if record.get('exit_code') == 0 and budget.get('mean_ms') is not None else 1


# --- entry -----------------------------------------------------------------------------------------------------------

def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--scenario', choices=sorted(SCENARIOS), help='; '.join(f'{key}: {value}' for key, value in SCENARIOS.items()))
    parser.add_argument('--direction', choices=['host-here', 'host-edith'], default='host-here')
    parser.add_argument('--path', choices=['direct', 'relay', 'directory-relay', 'ip'], default='direct')
    parser.add_argument('--relay-ttl', type=int, default=86400,
                        help="the longest relay credential the run's directory mints (300-86400 s; a short one renews mid-match)")
    parser.add_argument('--turn-config', type=Path, default=CLOUDFLARE_TURN_CONFIG,
                        help="the directory-relay path's backend file (its path only is passed; default the Cloudflare key file)")
    parser.add_argument('--runs', type=int, default=1)
    parser.add_argument('--relay-bridge', action='store_true',
                        help='relay only: EDITH reaches the TURN server through a UDP-over-ssh bridge (its loopback UDP, an ssh -R '
                             'TCP stream, UDP from this box), for a TURN server with no public forward')
    parser.add_argument('--out', type=Path, help=f'a fresh directory under {SCRATCH} (the same path is used on EDITH)')
    parser.add_argument('--minutes', type=float, default=10, help='sp-soak length')
    parser.add_argument('--client-box', choices=['here', 'linux'], default='here',
                        help='with --direction host-edith: the client runs on this box or on the Linux box (EDITH then runs one engine alone)')
    parser.add_argument('--soak', action='store_true', help="the soak's autosaves, client stalls, census and full-state hashing, judged as soak_two_peer judges")
    parser.add_argument('--match-minutes', type=float, help='mp-host-join length in minutes, the feel inputs looped (default: the 1200-tick arm)')
    parser.add_argument('--timeout', type=int, default=420, help='each match engine (seconds)')
    parser.add_argument('--instrumentation', choices=['lean', 'matrix'], default='lean',
                        help='lean: tick hashes, live hashes and the match report (the relay compare); matrix: plus the '
                             'per-tick sim dump and the controller dump (the feel matrix arms)')
    parser.add_argument('--feel-records', action='store_true', help='add -feel-measure to both match peers (the matrix -on arms)')
    parser.add_argument('--repo', type=Path, default=REPO, help='the engine tree on both boxes (its executable must match)')
    parser.add_argument('--remote-repo', type=Path, help="EDITH's engine tree when it is not --repo's path (a firewall-ruled tree holding the same executable)")
    parser.add_argument('--quiet-wait', type=int, default=1200, help='seconds to wait for other builds on this box to end')
    parser.add_argument('--box-wait', type=int, default=2700, help='seconds to wait while the inventory feel matrix holds this box')
    parser.add_argument('--dry-run', action='store_true', help='print the launches, copies and ssh commands instead of running them')
    parser.add_argument('--reanalyze', type=Path, help='re-reduce one fetched match directory (no launch)')
    parser.add_argument('--remote-peer', type=Path, help=argparse.SUPPRESS)
    parser.add_argument('--bridge-edith', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--bridge-bind', default='', help=argparse.SUPPRESS)
    options = parser.parse_args(argv)
    if options.remote_peer is None and not options.bridge_edith and options.reanalyze is None:
        if options.client_box == 'linux' and options.direction != 'host-edith':
            parser.error('--client-box linux runs the client there: the host is on EDITH (--direction host-edith)')
        if options.relay_bridge and options.path != 'relay':
            parser.error('--relay-bridge needs --path relay')
        if not options.scenario or not options.out:
            parser.error('--scenario and --out are required')
        out = options.out.resolve()
        if SCRATCH.resolve() not in out.parents:
            parser.error(f'--out must be under {SCRATCH}')
        if options.runs < 1:
            parser.error('--runs must be positive')
    return parser, options


def main(argv=None):
    global DRY_RUN
    parser, options = parse_args(argv)
    if options.remote_peer is not None:
        return remote_peer(options.remote_peer)
    if options.bridge_edith:
        return bridge_edith(options.bridge_bind)
    if options.reanalyze is not None:
        root = options.reanalyze.resolve()
        return 0 if analyze_match(harness(HERE), root, read_json(root / 'manifest.json'))['passed'] else 1
    DRY_RUN = options.dry_run
    os.environ.update(CCCP_HEADLESS='1', PYTHONDONTWRITEBYTECODE='1')
    h = harness(HERE)
    if not DRY_RUN:
        if options.out.exists() and options.scenario == 'sp-soak':
            parser.error(f'{options.out} exists; every run takes a fresh --out')
        local, remote = exe_hashes(options.repo, options.remote_repo)
        say(f'executable here {local[:16]} on EDITH {remote[:16]}')
        if local != remote:
            say('REFUSED: the executables differ; refresh EDITH per EDITH_SSH_RUNBOOK.md section 7')
            return 4
        h.feel.scratch_bytes(SCRATCH, SCRATCH_LIMIT)
    ship_driver()
    if options.scenario == 'sp-soak':
        return run_soak(h, options)
    if not DRY_RUN and not wait_box(options.box_wait):
        say('REFUSED: the inventory feel matrix still holds this box')
        return 3
    login = turn_login() if options.path == 'relay' else None
    label = f'{options.direction}-{path_label(options)}'
    tunnel = Tunnel(options.out.resolve() / f'tunnel-{label}.log', options.relay_bridge) if options.path != 'ip' else None
    linux_tunnel = LinuxTunnel(options.out.resolve() / f'tunnel-linux-{label}.log') if options.client_box == 'linux' and options.path != 'ip' else None
    bridge = None
    verdicts = []
    try:
        if not DRY_RUN:
            options.out.mkdir(parents=True, exist_ok=True)
        if tunnel:
            tunnel.open()
        if linux_tunnel and not DRY_RUN:
            linux_tunnel.open()
        if options.relay_bridge:
            lan = '192.168.3.55' if DRY_RUN else ssh('(Get-NetIPConfiguration | Where-Object { $_.IPv4DefaultGateway } | Select-Object -First 1).IPv4Address.IPAddress').strip()
            TURN['edith'] = f'turn:{lan}:{BRIDGE_UDP}?transport=udp'
            if DRY_RUN:
                say(f'dry-run: relay bridge EDITH udp {lan}:{BRIDGE_UDP} -> ssh -R tcp {BRIDGE_TCP} -> {TURN["here"]}')
            else:
                bridge = BridgeHere(TURN['here'], options.out.resolve() / f'bridge-{label}.log', lan)
                bridge.open()
        for index in range(1, options.runs + 1):
            if not DRY_RUN and not wait_box(options.box_wait):
                say('REFUSED: the inventory feel matrix holds this box')
                break
            verdicts.append(run_match(h, options, index, login))
            if not DRY_RUN:
                h.feel.scratch_bytes(SCRATCH, SCRATCH_LIMIT)
    finally:
        if bridge:
            bridge.close()
        if tunnel:
            tunnel.close()
        if linux_tunnel:
            linux_tunnel.close()
    if DRY_RUN:
        return 0
    passed = sum(bool(row.get('passed')) for row in verdicts)
    say(f'SUMMARY {options.scenario} {label}: {passed}/{options.runs} PASS {stamp()}')
    write_json(options.out / f'summary-{label}.json', verdicts)
    return 0 if passed == options.runs else 1


if __name__ == '__main__':
    raise SystemExit(main())
