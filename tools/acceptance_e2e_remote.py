"""Use the unchanged paired video scenario logic with one remote runner on each Windows box."""
from __future__ import annotations

import base64
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

from acceptance_collection import read, write
import acceptance_remote as dispatch
from acceptance_peer_session import Pair

SECRET_TOKENS = ('TURN_USER','TURN_PASS')


class RemoteCapture:
    def __init__(self, options):
        self.options = options
        self.pair = Pair(options.repo,options.collection_root,options.inventory,options.host_box,options.client_box)
        self.secrets = {f'__ACCEPTANCE_{name}__':options.tokens[name] for name in SECRET_TOKENS if name in options.tokens}
        options.tokens.update({name:f'__ACCEPTANCE_{name}__' for name in SECRET_TOKENS if name in options.tokens})

    def make_run(self, repo, args, out, timeout, env=None, **kwargs):
        if kwargs:
            raise ValueError('remote relay captures do not declare retained-runtime reuse')
        return RemoteRun(self,repo,args,out,timeout,env or {})

    def directory_tunnel(self, port):
        if port: self.pair.directory_tunnel(port)

    def close(self): self.pair.close()


class RemoteRun:
    def __init__(self, capture, repo, args, out, timeout, environment):
        from run_sim_test import RUNTIME_SETTINGS
        self.capture,self.pair=capture,capture.pair
        self.repo,self.out,self.timeout=Path(repo).resolve(),Path(out).resolve(),timeout
        self.role=self.out.name
        if self.role not in ('host','client'): raise ValueError('remote capture declares one host and one client')
        self.cwd=self.out/'runtime'
        (self.cwd/'Userdata').mkdir(parents=True,exist_ok=False)
        (self.cwd/'Userdata/Settings.ini').write_text('SettingsMan\n'+''.join(f'\t{k} = {v}\n' for k,v in RUNTIME_SETTINGS.items()),encoding='utf-8')
        self.argv=[str(self.repo/'Cortex Command.exe'),'-headless',*args]
        self.env=dict(environment)
        self.record={};self.started=False;self.finished=False;self.sent={}

    def start(self):
        root=self.out.parent
        files={}
        # Scenario scripts are text. They travel through the same private channel as credential values.
        for base in (root/(self.role+'-stage'),self.cwd):
            for path in base.rglob('*'):
                if path.is_file():
                    text=self.pair.mapped(self.role,path.read_text(encoding='utf-8'))
                    for token,value in self.capture.secrets.items(): text=text.replace(token,value)
                    files[path.relative_to(root).as_posix()]=text
        native=dict(root=str(root),out=str(self.out),role=self.role,argv=self.argv[2:],environment=self.env,
                    timeout=self.timeout,repo=str(self.repo),secrets=list(self.capture.secrets))
        task,done=self.pair.launch(self.role,root/'.sessions'/self.role,'capture-peer',native,
                                  dict(files=files,secrets=self.capture.secrets),timeout=self.timeout+120)
        self.task,self.done=task,done;self.started=True
        self.deadline=time.monotonic()+self.timeout+240
        return self

    def synchronize(self):
        box,remote,rb,_=self.pair.boxes[self.role]
        target=Path(self.pair.mapped(self.role,str(self.out)))
        raw=remote.ssh(f'python {rb.ps_quote(box.repo+"/tools/acceptance_remote.py")} --snapshot {rb.ps_quote(target.as_posix())}',timeout=30)
        records=json.loads(raw)
        for relative, encoded in records.items():
            path=(self.out.parent/relative).resolve()
            if not path.is_relative_to(self.out.parent): raise ValueError('native snapshot escaped the declared run')
            path.parent.mkdir(parents=True,exist_ok=True)
            data=base64.b64decode(encoded,validate=True)
            # Publish atomically: the scenario thread may be reading a frame index or done receipt now.
            temporary=path.with_name(path.name+'.incoming-'+self.role)
            temporary.write_bytes(data);os.replace(temporary,path)
        signals={}
        candidates=list(self.out.parent.glob('*-stage/probe/done.json'))+list(self.out.parent.glob('*-stage/gameplay-started.json'))
        candidates+=list(Path(self.capture.options.out).glob('*-directory/listed.json'))
        for path in candidates:
            relative=path.relative_to(self.pair.root).as_posix()
            data=path.read_bytes();digest=hashlib.sha256(data).hexdigest()
            if self.sent.get(relative)==digest: continue
            # Reject a partial native JSON write; it will be retried at the next sample.
            try: json.loads(data)
            except ValueError: continue
            signals[relative]=base64.b64encode(data).decode();self.sent[relative]=digest
        if signals:
            destination=self.pair.boxes[self.role][3]
            command=f'python {rb.ps_quote(box.repo+"/tools/acceptance_remote.py")} --publish {rb.ps_quote(destination.as_posix())}'
            subprocess.run(['ssh','-o','BatchMode=yes',box.ssh,command],input=json.dumps(signals),text=True,
                           capture_output=True,timeout=30,check=True)

    def finish(self):
        box,remote,rb,_=self.pair.boxes[self.role]
        while time.monotonic()<self.deadline:
            self.synchronize()
            ended=remote.read_text(self.done,timeout=30)
            if ended is not None: break
            time.sleep(.5)
        else:
            self.terminate('remote capture task exceeded its declared budget')
            raise TimeoutError('remote capture task did not finish')
        source=Path(self.pair.mapped(self.role,str(self.out.parent)))
        raw=self.out.parent/'.native'/self.role
        dispatch.fetch_evidence(box,remote,rb,source,raw)
        for path in raw.iterdir():
            if path.name not in (self.role,self.role+'-stage'): continue
            target=self.out.parent/path.name
            if path.is_dir(): shutil.copytree(path,target,dirs_exist_ok=True)
            else: shutil.copy2(path,target)
        self.record=read(raw/self.role/'record.json')
        self.record['remote_task_exit']=ended.strip()
        write(Path(self.capture.options.out)/'identities'/f'{box.name}.json',read(raw/'.sessions'/self.role/'identity.json'))
        if not re.search(r'\brc=0\b',ended):
            self.record['error']='remote session task failed: '+ended.strip()
        self.finished=True
        return self.record

    def terminate(self, reason='scenario drop'):
        if not self.started or self.finished: return
        box,remote,rb,_=self.pair.boxes[self.role]
        stop=self.task/'stop.json'
        # The task reads only its own stop file, then asks its runner to terminate its owned job.
        encoded=base64.b64encode(json.dumps(dict(reason=reason)).encode()).decode()
        remote.ssh(f'[IO.File]::WriteAllBytes({rb.ps_quote(stop.as_posix())},[Convert]::FromBase64String({rb.ps_quote(encoded)}))')

    def close(self):
        if self.started and not self.finished: self.terminate('coordinator closed the capture')


def execute_peer(spec, values):
    from run_sim_test import make_run
    import e2e_video as video
    native=spec['native'];root=Path(native['root']);out=Path(native['out']);role=native['role']
    environment=dict(native['environment'])
    encoder=video.find_ffmpeg()
    if encoder:
        environment['CCCP_TEST_RECORD_ENCODER']=str(encoder)
        environment['CCCP_TEST_RECORD_CODEC']=video.encoder_codec(encoder)
    runtime_files={}
    for relative,text in values['files'].items():
        path=(root/relative).resolve()
        if not path.is_relative_to(root.resolve()): raise ValueError('scenario file escaped its native run')
        if path.is_relative_to(out/'runtime'):
            runtime_files[path.relative_to(out/'runtime')]=text
        else:
            path.parent.mkdir(parents=True,exist_ok=True);path.write_text(text,encoding='utf-8')
    run=make_run(Path(native['repo']),native['argv'],out,timeout=native['timeout'],env=environment)
    for relative,text in runtime_files.items():
        path=Path(run.cwd)/relative;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(text,encoding='utf-8')
    (out/'video').mkdir()
    task=Path(spec['identity']).parent
    try:
        run.start()
        while run.poll() is None:
            if (task/'stop.json').is_file(): run.terminate(reason=read(task/'stop.json')['reason'])
            video.gameplay_signals(out/'video',root/(role+'-stage'))
            time.sleep(.05)
        record=run.finish()
    finally:
        run.close()
        # Retain the scripts and writable settings with credential values replaced after the engine exits.
        for base in (root/(role+'-stage'),Path(run.cwd)/'Userdata'):
            for path in base.rglob('*'):
                if path.is_file() and path.suffix.lower() in ('.txt','.ini','.json'):
                    text=path.read_text(encoding='utf-8',errors='replace')
                    for token,value in values.get('secrets',{}).items(): text=text.replace(value,token)
                    path.write_text(text,encoding='utf-8')
    console=Path(run.cwd)/'LogConsole.txt'
    if console.is_file(): shutil.copy2(console,out/'console.log')
    write(out/'record.json',record)
    return 0 if record.get('exit_code')==0 else 1
