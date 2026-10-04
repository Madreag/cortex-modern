"""Coordinator helpers for one game engine per Windows session task.

Only credential values cross an SSH stdin/named-pipe channel. The central credential files,
task payloads, command lines and shipment manifests never contain those values.
"""
from __future__ import annotations

import base64
import contextlib
import json
import os
from pathlib import Path
import queue
import subprocess
import threading
import time
import uuid

import acceptance_remote as dispatch
from acceptance_collection import read, write


def receive_values(address):
    from multiprocessing.connection import Client
    deadline = time.monotonic() + 60
    while True:
        try:
            with Client(address, family='AF_PIPE') as connection:
                return json.loads(connection.recv_bytes())
        except (FileNotFoundError, ConnectionRefusedError):
            if time.monotonic() >= deadline:
                raise RuntimeError('the private session input pipe did not open') from None
            time.sleep(.1)


def broker(address):
    from multiprocessing.connection import Listener
    import sys
    values = sys.stdin.buffer.read()
    # A session task that never starts must not leave a process holding credentials indefinitely.
    timer = threading.Timer(180, lambda: os._exit(124)); timer.daemon = True; timer.start()
    with Listener(address, family='AF_PIPE') as listener:
        print('READY', flush=True)
        with listener.accept() as connection:
            connection.send_bytes(values)
    timer.cancel()
    return 0


class Delivery:
    def __init__(self, box, rb, values):
        self.address = r'\\.\pipe\cortex-acceptance-' + uuid.uuid4().hex
        command = f'python {rb.ps_quote(box.repo + "/tools/acceptance_remote.py")} --broker {rb.ps_quote(self.address)}'
        self.process = subprocess.Popen(['ssh','-o','BatchMode=yes',box.ssh,command], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        self.process.stdin.write(json.dumps(values).encode()); self.process.stdin.close()
        ready = queue.Queue()
        threading.Thread(target=lambda: ready.put(self.process.stdout.readline()), daemon=True).start()
        try:
            if ready.get(timeout=60).strip() != b'READY':
                raise RuntimeError('private session input broker refused')
        except BaseException:
            self.close(); raise

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=15)
        for stream in (self.process.stdout, self.process.stderr):
            if stream: stream.close()


class Pair:
    def __init__(self, repo, root, inventory, host='ALLY', client='EDITH'):
        self.repo, self.root, self.inventory = Path(repo).resolve(), Path(root).resolve(), Path(inventory).resolve()
        split, self.rb = dispatch.inventory_modules(inventory)
        boxes, _ = split.load_manifest(self.inventory/'boxes.json')
        self.schedule = read(self.root/'split-plan.json')
        self.source, self.executable, self.cid = (self.schedule[key] for key in ('source_sha','exe_sha256','collection_id'))
        if (host, client) != ('ALLY', 'EDITH'):
            raise ValueError('the declared relay pair is ALLY host and EDITH client')
        self.boxes = {}
        for role, name in (('host', host), ('client', client)):
            box = next(box for box in boxes if box.name == name)
            if box.kind != 'windows-task' or box.max_engines < 1:
                raise ValueError(f'{name}: one engine through its session task is required')
            self.boxes[role] = dispatch.prepare_box(self.repo, box, self.root, self.inventory, self.source, self.executable)
        self.deliveries, self.tunnels, self.pending = [], {}, []

    def mapped(self, role, value):
        box, _, _, root = self.boxes[role]
        if isinstance(value, dict): return {key:self.mapped(role,item) for key,item in value.items()}
        if isinstance(value, list): return [self.mapped(role,item) for item in value]
        if not isinstance(value, str): return value
        for old, new in ((str(self.repo),box.repo), (str(self.root),root.as_posix())):
            value = value.replace(old,new).replace(old.replace('\\','/'),new.replace('\\','/'))
        return value

    def directory_tunnel(self, port):
        for role, (box, remote, rb, _) in self.boxes.items():
            key = (role, port)
            if key in self.tunnels: continue
            output = self.root/'.windows'/box.name/f'directory-{port}.log'
            handle = output.open('ab')
            process = subprocess.Popen(['ssh','-N','-o','BatchMode=yes','-o','ExitOnForwardFailure=yes',
                '-o','ServerAliveInterval=15','-R',f'127.0.0.1:{port}:127.0.0.1:{port}',box.ssh],
                stdin=subprocess.DEVNULL, stdout=handle, stderr=subprocess.STDOUT,
                creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            self.tunnels[key] = (process, handle)
            deadline = time.monotonic()+30
            while time.monotonic() < deadline:
                if process.poll() is not None: raise RuntimeError(f'{box.name}: directory tunnel refused; {output}')
                probe = remote.ssh(f"$c=[Net.Sockets.TcpClient]::new(); try {{ $c.Connect('127.0.0.1',{port}); 'open' }} catch {{ 'closed' }} finally {{ $c.Dispose() }}")
                if probe.strip() == 'open': break
                time.sleep(1)
            else: raise RuntimeError(f'{box.name}: private directory tunnel did not open')

    def launch(self, role, local_task, kind, native, values=None, timeout=900, *, book=None):
        box, remote, rb, _ = self.boxes[role]
        task = Path(self.mapped(role,str(Path(local_task).resolve())))
        payload = dict(kind=kind, box=box.name, repo=box.repo, executable=box.exe, exe_sha256=self.executable,
            source_sha=self.source, collection_id=self.cid, identity=(task/'identity.json').as_posix(), timeout=timeout,
            native=self.mapped(role,native))
        delivery = Delivery(box,rb,values or {})
        self.deliveries.append(delivery)
        payload['values_pipe'] = delivery.address
        done = dispatch.task_payload(box,remote,rb,task,payload,Path(local_task)/'control',wait=False)
        if not hasattr(self,'pending'):self.pending=[]
        artifact_root=Path(self.mapped(role,native['root'])) if kind=='capture-peer' else task
        self.pending.append(dict(role=role,task=task,done=done,timeout=timeout,destination=Path(local_task),root=artifact_root,book=book))
        return task, done

    def complete(self, task):
        self.pending=[entry for entry in getattr(self,'pending',[]) if entry['task']!=task]

    def cancel(self, role, task, done, timeout, destination, *, book=None, root=None):
        box,remote,rb,_=self.boxes[role]
        stop=Path(task)/'stop.json'
        encoded=base64.b64encode(json.dumps(dict(reason='coordinator closed its owned peer')).encode()).decode()
        remote.ssh(f'[IO.File]::WriteAllBytes({rb.ps_quote(stop.as_posix())},[Convert]::FromBase64String({rb.ps_quote(encoded)}))')
        ended=remote.wait_done(done,120)
        if ended=='TIMEOUT':
            write(Path(destination)/'cleanup-incomplete.json',dict(passed=False,reason='owned native task did not stop; no evidence copied'))
            raise TimeoutError(f'{box.name}: owned native task did not stop for relay cleanup')
        if book is not None:
            gate=dispatch.sanitize_remote(box,remote,rb,root or task,book,cleanup=True)
            write(Path(destination)/'cancel-secret-scan.json',gate)
        self.complete(task)
        return ended

    def finish(self, role, task, done, timeout, destination, *, book=None):
        box, remote, rb, _ = self.boxes[role]
        result = remote.wait_done(done, timeout+120)
        if result == 'TIMEOUT':
            self.cancel(role,task,done,timeout,destination,book=book)
            raise TimeoutError(f'{box.name}: session task exceeded its budget; stopped and swept')
        dispatch.fetch_evidence(box,remote,rb,task,destination,book=book)
        self.complete(task)
        return result

    def close(self):
        with contextlib.ExitStack() as cleanup:
            for delivery in self.deliveries:cleanup.callback(delivery.close)
            for process,handle in self.tunnels.values():
                cleanup.callback(handle.close)
                if process.poll() is None:
                    cleanup.callback(process.wait,timeout=15);cleanup.callback(process.terminate)
            for task in list(getattr(self,'pending',[])):cleanup.callback(self.cancel,**task)

    def __enter__(self): return self
    def __exit__(self,*_): self.close()


def snapshot(root):
    """Small native gate evidence only; full media is fetched after the engine ends."""
    root = Path(root).resolve()
    candidates = [root/'stdout.log', root/'launch.json', root/'video/frames.jsonl', root/'video/events.jsonl',
                  root/'record.json', root/'video/manifest.json']
    candidates += list(root.parent.glob('*-stage/probe/*.json'))
    result = {}
    for path in candidates:
        if path.is_file() and not path.is_symlink() and path.stat().st_size <= 16*1024*1024:
            result[path.relative_to(root.parent).as_posix()] = base64.b64encode(path.read_bytes()).decode()
    return result


def publish(root, records):
    root = Path(root).resolve()
    for relative, value in records.items():
        path = (root/relative).resolve()
        if not path.is_relative_to(root) or path.suffix != '.json' or path.name not in ('done.json','gameplay-started.json','listed.json'):
            raise ValueError('only declared peer gate signals may be mirrored')
        data = base64.b64decode(value,validate=True)
        json.loads(data)
        path.parent.mkdir(parents=True,exist_ok=True)
        path.write_bytes(data)


def execute(spec):
    values = receive_values(spec['values_pipe'])
    if spec['kind'] == 'relay-peer':
        import edith_cross
        native = spec['native']
        from relay_private import require_public
        require_public(values.get('settings',{}))
        native['settings'].update(values.get('settings',{}))
        Path(native['root']).mkdir(parents=True,exist_ok=True)
        for name, text in native.pop('input_files',{}).items():
            (Path(native['root'])/name).write_text(text,encoding='utf-8')
        path = Path(native['root'])/(native['peer']+'-spec.json')
        # The directory issues the login; no settings or task file carries one.
        h = edith_cross.harness(Path(native['repo'])/'tools')
        run = edith_cross.prepare_peer(h,native)
        try:
            run.start()
            stop=Path(spec['identity']).parent/'stop.json'
            while run.poll() is None:
                if stop.is_file():run.terminate(reason='coordinator closed its owned peer')
                time.sleep(.05)
            record=run.finish()
        finally:
            run.close(); edith_cross.redact(h,run,native)
        write(Path(native['root'])/(native['peer']+'-record.json'),record)
        write(path,native)
        return 0 if record.get('exit_code') == 0 else 1
    if spec['kind'] == 'relay-selftest':
        from turn_relay_rows import main
        from relay_private import inherited_environment
        import sys
        sys.argv = ['turn_relay_rows.py',*spec['native']['argv'],'--stop-file',str(Path(spec['identity']).parent/'stop.json')]
        with inherited_environment(values.get('environment',{})):return main()
    if spec['kind'] == 'capture-peer':
        from acceptance_e2e_remote import execute_peer
        return execute_peer(spec, values)
    raise ValueError('unknown Windows peer payload')
