"""Retain large text records losslessly once their writers have closed them."""
from __future__ import annotations

import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import zipfile
import zlib


def retire_diagnostic(path, own, kind):
    path=Path(path); own=Path(own).resolve()
    if path.is_symlink() or not path.resolve().is_relative_to(own): raise ValueError('diagnostic leaves its owning instance')
    digest=hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda:stream.read(1024*1024),b''):digest.update(block)
    receipt=dict(type='diagnostic_retired',kind=kind,path=str(path.relative_to(own)),bytes=path.stat().st_size,sha256=digest.hexdigest())
    path.unlink()
    with (own/'retired-diagnostics.jsonl').open('a',encoding='utf-8') as stream:stream.write(json.dumps(receipt)+'\n')


class DiagnosticWindow:
    def __init__(self, own, captures=2, pngs=10):
        self.own=Path(own).resolve();self.captures=captures;self.pngs=pngs;self.completed=[];self.next_png_scan=0

    def completed_capture(self,path):
        path=Path(path)
        if path in self.completed:return
        self.completed.append(path)
        while self.captures is not None and len(self.completed)>self.captures:
            old=self.completed.pop(0)
            for part in sorted(old.glob('*.txt.gz')):retire_diagnostic(part,self.own,'fullstate dump; native hashes and scope retained')

    def poll_pngs(self):
        import time
        if time.monotonic()<self.next_png_scan:return
        self.next_png_scan=time.monotonic()+2
        complete=[]
        for path in (self.own/'engine/feel').glob('*.png'):
            if path.is_symlink():raise ValueError('presentation image is a link')
            try:
                with path.open('rb') as stream:
                    if stream.read(8)!=b'\x89PNG\r\n\x1a\n':continue
                    stream.seek(-12,2)
                    if stream.read()!=b'\x00\x00\x00\x00IEND\xaeB`\x82':continue
                complete.append(path)
            except OSError:continue
        complete.sort(key=lambda p:(p.stat().st_mtime_ns,p.name))
        for path in complete[:-self.pngs]:
            try:retire_diagnostic(path,self.own,'periodic presentation PNG; frame records retained')
            except PermissionError:continue


def presentation_records(index):
    """Validate the explicitly retained native window; never imply full-run coverage."""
    index=Path(index); document=json.loads(index.read_text(encoding='utf-8'))
    previous=document['dropped_lines']
    if len(document['parts'])>document['retained_chunks']: raise ValueError('presentation window exceeds its declared bound')
    for part in document['parts']:
        path=index.parent/part['path']
        if path.is_symlink() or path.resolve().parent!=index.parent.resolve(): raise ValueError('presentation chunk leaves its directory')
        if part['first_sequence']!=previous or part['bytes']>document['chunk_bytes']: raise ValueError('presentation sequence or chunk bound differs')
        crc=size=count=0
        with gzip.open(path,'rb') as stream:
            for line in stream:
                crc=zlib.crc32(line,crc); size+=len(line); count+=1
                yield json.loads(line)
        if (crc,size,count)!=(part['crc32'],part['bytes'],part['lines']): raise ValueError('presentation chunk integrity differs')
        previous+=count
        if previous-1!=part['last_sequence']: raise ValueError('presentation last sequence differs')
    if previous!=document['total_lines']: raise ValueError('presentation retained count differs')


class LobbyWatch:
    """Whether an engine is in a rematch lobby: from its 'returning to lobby' line until its next round starts."""
    ENTERED=re.compile(r'\[net-match-service-e2e\] rematch: match \d+ over .*returning to lobby')
    LEFT=re.compile(r'\[net-lockstep\] start round=\d+ frame=')

    def __init__(self,path):
        self.path=Path(path); self.offset=0; self.pending=b''; self.in_lobby=False

    def poll(self):
        if not self.path.is_file(): return self.in_lobby
        with self.path.open('rb') as stream:
            stream.seek(self.offset); block=stream.read(1024*1024); self.offset=stream.tell()
        lines=(self.pending+block).split(b'\n'); self.pending=lines.pop()
        for raw in lines:
            text=raw.decode('utf-8',errors='replace')
            if self.ENTERED.search(text): self.in_lobby=True
            elif self.LEFT.search(text): self.in_lobby=False
        return self.in_lobby


class NativeFaultEffects:
    """H4 substitution effects are distinct from installing a fault selector."""
    def __init__(self,path,cases,incarnation):
        self.path=Path(path); self.incarnation=incarnation; self.offset=0; self.number=0
        self.cases={c['id']:c for c in cases if c.get('incarnation',0)==incarnation}
        self.armed={}; self.seen=set(); self.pending=b''

    def poll(self,observed_ms):
        if not self.path.is_file(): return []
        with self.path.open('rb') as stream:
            stream.seek(self.offset); block=stream.read(1024*1024); self.offset=stream.tell()
        lines=(self.pending+block).split(b'\n'); self.pending=lines.pop(); output=[]
        for raw in lines:
            self.number+=1; text=raw.decode('utf-8',errors='replace').strip()
            if '[cross-fault] ' in text:
                try: receipt=json.loads(text.split('[cross-fault] ',1)[1])
                except ValueError: continue
                if receipt.get('id') in self.cases and receipt.get('applied'):
                    self.armed[receipt['action']]=receipt['id']
            match=re.search(r'\[net-h4-fault\] (ack-drop|ack-duplicate|commit-drop):',text)
            if not match or match[1] not in self.armed: continue
            id=self.armed[match[1]]
            if id in self.seen: continue
            self.seen.add(id)
            output.append(dict(id=id,incarnation=self.incarnation,action=match[1],path=str(self.path),line=self.number,
                               text=text,observed_payload_ms=observed_ms))
        return output


class RecoveryLedger:
    """Observe one peer on its owning payload clock, which survives engine restarts.

    A native fault-begin is flushed before the fault acts. The previous drained
    read bounds its start from below; the read that sees a terminal record bounds
    completion from above. Deadline checks use this conservative duration, never
    subtraction between native process clocks or clocks on different boxes.
    """
    def __init__(self,path,peer,clock_domain,begin_ms,cases):
        self.path=Path(path); self.peer=peer; self.clock_domain=clock_domain
        self.begin_ms=begin_ms; self.cases={case['id']:case for case in cases}
        self.starts={}; self.applied=set(); self.completed=set(); self.resets=set()

    def write(self,id,incarnation,phase,lower,upper,native):
        record=dict(id=id,peer=self.peer,incarnation=incarnation,phase=phase,clock_domain=self.clock_domain,
                    lower_wall_ms=lower,upper_wall_ms=upper,wall_ms=lower if phase=='fault_applied' else upper,
                    native=native,source_sequence=native.get('sequence'))
        with self.path.open('a',encoding='utf-8') as stream: stream.write(json.dumps(record)+'\n')

    def observe(self,rows,incarnation,lower,upper):
        for row in rows:
            id=row.get('id')
            if id not in self.cases: continue
            if row.get('type')=='fault_begin':
                self.starts.setdefault(id,dict(lower=lower,upper=upper,native=row,incarnation=incarnation))
            elif row.get('type')=='fault' and row.get('applied') and id not in self.applied:
                start=self.starts.setdefault(id,dict(lower=self.begin_ms,upper=upper,native=row,incarnation=incarnation))
                start['upper']=upper
                self.applied.add(id)
                self.write(id,incarnation,'fault_applied',start['lower'],start['upper'],row)
                if row.get('action') in ('live-stall', 'draw-stall', 'late-script-stall') and 'completed_wall_ms' in row:
                    self.fault_reset(id, incarnation, lower, upper, row)
            elif row.get('type') == 'fault_reset' and row.get('send_recv_armed') is True:
                self.fault_reset(id, incarnation, lower, upper, row)
            elif row.get('type') == 'fault_effect' and row.get('effect_observed') and row.get('reset'):
                self.fault_reset(id, incarnation, lower, upper, row)
            elif row.get('type')=='recovery':
                phase=row.get('recovery_phase','unmapped')
                self.write(id,incarnation,phase,lower,upper,row)
                if id in self.applied and row.get('terminal') and phase in self.cases[id]['outcomes']:
                    self.completed.add(id)

    def external_start(self,case,incarnation,lower,upper,progress):
        id=case['id']; self.starts[id]=dict(lower=lower,upper=upper,native=progress,incarnation=incarnation)
        self.applied.add(id)
        self.write(id,incarnation,'fault_applied',lower,upper,dict(action=case['action'] if 'action' in case else 'crash-restart',actual=progress,applied=True))
        self.fault_reset(id, incarnation, lower, upper, dict(action=case.get('action', 'crash-restart'), process_terminated=True))

    def fault_reset(self, id, incarnation, lower, upper, native):
        if id in self.applied and id not in self.resets:
            self.write(id, incarnation, 'fault_reset', lower, upper, native)
            self.resets.add(id)

    def restart_inputs(self,incarnation):
        return {id:dict(engine_after_wall_ms=0,effect_finished=True,origin=self.starts[id]) for id in self.applied-self.completed
                if self.cases[id].get('return_incarnation',self.cases[id]['incarnation'])==incarnation}


def inspect_checkpoint(path):
    """Check archive CRCs and named identity; do not claim restoration or admission validity."""
    path = Path(path)
    result = dict(path=str(path), passed=False, scope='CRC, required entries, world/descriptor/manifest identity; not a restore or admission-seal check')
    def properties(text):
        values = {}
        for line in text.splitlines():
            if match := re.match(r'^\s*([A-Za-z]+)\s*=\s*(.*?)\s*$', line):
                if match[1] in values and match[1] in {'SavedTick','MatchId','SessionId','RoundId','ManifestSchema','ConfigHash','ConfigPayload','RestoreSchema'}:
                    raise ValueError('duplicate checkpoint property '+match[1])
                values[match[1]] = match[2]
        return values
    try:
        with path.open('rb') as source:
            digest = hashlib.sha256()
            for block in iter(lambda:source.read(1024*1024),b''): digest.update(block)
            result['sha256'] = digest.hexdigest()
        result['bytes'] = path.stat().st_size
        with zipfile.ZipFile(path) as archive:
            names = archive.namelist()
            required = {'Save.ini','Index.ini','Save Mat.png','Save FG.png','Save BG.png','Restore.ini'}
            if len(names) != len(set(names)) or not required <= set(names): raise ValueError('missing or duplicate archive entries')
            if bad := archive.testzip(): raise ValueError('CRC mismatch: '+bad)
            descriptor = properties(archive.read('Restore.ini').decode('utf-8-sig'))
            save = archive.read('Save.ini').decode('utf-8-sig')
            world_tick = re.search(r'(?m)^\s*SimUpdateCount\s*=\s*(\d+)\s*$', save)
            tick = int(descriptor['SavedTick']); match_id = descriptor['MatchId']
            if tick < 1 or descriptor.get('RestoreSchema') != '1': raise ValueError('unsupported descriptor schema or tick')
            if path.name != f'{match_id}-{tick}.ccsave' or not world_tick or int(world_tick[1]) != tick:
                raise ValueError('archive/world/descriptor tick identity differs')
            if 'RuntimeGlobals = ' not in save or 'LuaStateGraph = ' not in save: raise ValueError('missing runtime or script graph')
            result.update(match_id=match_id,tick=tick,session=descriptor['SessionId'],source_round=int(descriptor['RoundId']),
                          module_hash=descriptor.get('ModuleManifestHash'),world_structure_hash=descriptor.get('WorldStructureHash'))
        manifest_path = path.with_suffix('.ccmanifest')
        manifest = properties(manifest_path.read_text(encoding='utf-8-sig'))
        if any(manifest.get(key) != descriptor.get(key) for key in ('MatchId','SessionId','RoundId','SavedTick')):
            raise ValueError('manifest and descriptor identity differ')
        if manifest.get('ManifestSchema') not in ('2','3') or not manifest.get('ConfigHash') or not manifest.get('ConfigPayload'):
            raise ValueError('manifest lacks config payload/hash or has unsupported schema')
        result.update(passed=True,manifest_path=str(manifest_path),config_hash=manifest['ConfigHash'],
                      manifest_sha256=hashlib.sha256(manifest_path.read_bytes()).hexdigest())
    except (OSError, ValueError, KeyError, zipfile.BadZipFile, RuntimeError) as error:
        result['reason'] = str(error)
    return result


class RecordWriter:
    """Bound compressed chunks without discarding a record or following a link."""

    def __init__(self, base, chunk_bytes=4 * 1024 * 1024, total_bytes=256 * 1024 * 1024):
        self.base = Path(base)
        if chunk_bytes <= 0 or total_bytes <= 0:
            raise ValueError('record budgets must be positive')
        self.chunk_bytes, self.total_bytes = chunk_bytes, total_bytes
        self.parts, self.stream, self.size, self.total, self.lines = [], None, 0, 0, 0
        self.index = self.base.with_name(self.base.name + '.index.json')
        if self.index.exists():
            raise FileExistsError(self.index)

    def write(self, row):
        data = (json.dumps(row, allow_nan=False, separators=(',', ':')) + '\n').encode('utf-8')
        if self.total + len(data) > self.total_bytes:
            raise RuntimeError(f'record byte budget exceeded: {self.base}')
        if self.stream is not None and self.size + len(data) > self.chunk_bytes:
            self._seal()
        if self.stream is None:
            self.path = self.base.with_name(f'{self.base.name}.{len(self.parts):05d}.jsonl.gz')
            self.raw = self.path.open('xb')
            self.stream = gzip.GzipFile(filename='', fileobj=self.raw, mode='wb', compresslevel=3, mtime=0)
            self.digest = hashlib.sha256()
            self.size = self.lines = 0
        self.stream.write(data)
        self.digest.update(data)
        self.size += len(data); self.total += len(data); self.lines += 1

    def _seal(self):
        if self.stream is None:
            return
        self.stream.close(); self.raw.close(); self.stream = None
        self.parts.append(dict(path=self.path.name, bytes=self.size, lines=self.lines,
                               sha256=self.digest.hexdigest(), compressed_bytes=self.path.stat().st_size))
        temporary = self.index.with_suffix('.tmp')
        temporary.write_text(json.dumps(dict(version=1, complete=False, parts=self.parts,
                                             uncompressed_bytes=self.total)), encoding='utf-8')
        temporary.replace(self.index)

    def close(self):
        self._seal()
        self.index.write_text(json.dumps(dict(version=1, complete=True, parts=self.parts,
                                             uncompressed_bytes=self.total)), encoding='utf-8')

    def __enter__(self):
        return self

    def __exit__(self, *exception):
        self.close()
        if exception[0] is not None:
            value = json.loads(self.index.read_text(encoding='utf-8')); value['complete'] = False
            self.index.write_text(json.dumps(value), encoding='utf-8')


def read_records(index):
    index = Path(index)
    document = json.loads(index.read_text(encoding='utf-8'))
    for part in document['parts']:
        path = index.parent / part['path']
        if path.is_symlink() or path.parent.resolve() != index.parent.resolve():
            raise ValueError('record chunk leaves its directory')
        digest, size, lines = hashlib.sha256(), 0, 0
        with gzip.open(path, 'rb') as stream:
            for raw in stream:
                digest.update(raw); size += len(raw); lines += 1
                yield json.loads(raw)
        if digest.hexdigest() != part['sha256'] or size != part['bytes'] or lines != part['lines']:
            raise ValueError(f'record chunk digest or count differs: {path}')


def record_path(path):
    path = Path(path)
    packed = path.with_name(path.name + '.gz')
    return packed if not path.is_file() and packed.is_file() else path


def open_record(path, mode='rt', **kwargs):
    path = record_path(path)
    return gzip.open(path, mode, **kwargs) if path.suffix == '.gz' else path.open(mode, **kwargs)


def compress_closed_record(path, root):
    """Pack a writer-sealed file, verify it, then retire only that owned plain file."""
    path, root = Path(path), Path(root).resolve()
    if path.is_symlink() or not path.resolve().is_relative_to(root):
        raise ValueError('record leaves its owning instance')
    # Both sides resolved: a root reached through a linked directory (macOS /tmp is /private/tmp) names the same files.
    path = path.resolve()
    size_before = path.stat().st_size
    destination = path.with_name(path.name + '.gz')
    temporary = path.with_name(path.name + '.gz.partial')
    digest, size = hashlib.sha256(), 0
    with path.open('rb') as source, temporary.open('xb') as sink:
        with gzip.GzipFile(filename='', fileobj=sink, mode='wb', compresslevel=3, mtime=0) as packed:
            for block in iter(lambda: source.read(1024*1024), b''):
                digest.update(block); size += len(block); packed.write(block)
    restored, restored_size = hashlib.sha256(), 0
    with gzip.open(temporary, 'rb') as source:
        for block in iter(lambda: source.read(1024*1024), b''):
            restored.update(block); restored_size += len(block)
    if size != size_before or path.stat().st_size != size_before or size != restored_size or digest.digest() != restored.digest():
        raise RuntimeError('sealed record changed or compression did not preserve it')
    if destination.exists(): raise FileExistsError(destination)
    temporary.replace(destination)
    receipt = dict(original=str(path.relative_to(root)), retained=str(destination.relative_to(root)), original_bytes=size,
                   original_sha256=digest.hexdigest(), compressed_bytes=destination.stat().st_size)
    with (root / 'compressed-records.jsonl').open('a', encoding='utf-8') as index:
        index.write(json.dumps(receipt)+'\n')
    path.unlink()
    return receipt


class CaptureSealer:
    """Pack completed captures while an engine runs, without touching active dumps.

    ActivityMan emits fullstate-scope after FullStateHashLine returns and closes
    every section file. Repeated keys with several queued paths are ambiguous;
    those captures remain raw until the owning process exits.
    """
    def __init__(self, own, captures=2):
        self.announced_own=Path(os.path.abspath(own))
        self.own=Path(own).resolve()
        self.log=self.own/'engine/stdout.log'
        self.offset=0; self.pending=b''; self.contexts={}; self.sealed=set()
        self.window=DiagnosticWindow(self.own, captures=captures)

    def poll(self):
        self.window.poll_pngs()
        if not self.log.is_file(): return
        with self.log.open('rb') as stream:
            stream.seek(self.offset); block=stream.read(4*1024*1024); self.offset+=len(block)
        lines=(self.pending+block).split(b'\n'); self.pending=lines.pop()
        for raw in lines:
            line=raw.decode('utf-8',errors='replace').strip()
            if match:=re.match(r'^\[fullstate-context\] tick=(\d+) round=(\d+) label=(\S+) path=(.+)$',line):
                key=match.group(1,2,3)
                # Announcements precede directory creation. Resolve physical
                # ownership only after the matching writer-complete scope.
                path=Path(os.path.abspath(match[4]))/match[1]
                if not path.is_relative_to(self.announced_own/'fullstate'):
                    raise ValueError(f'capture context leaves its owning instance: {path}; owner={self.announced_own}')
                self.contexts.setdefault(key,[]).append(path)
            elif match:=re.match(r'^\[fullstate-scope\] tick=(\d+) round=(\d+) label=(\S+) per_peer=',line):
                paths=self.contexts.get(match.group(1,2,3),[])
                if len(paths)!=1 or paths[0] in self.sealed: continue
                path=paths[0]
                if not path.is_dir(): continue
                path=path.resolve(strict=True)
                if not path.is_relative_to(self.own/'fullstate'):
                    raise ValueError(f'completed capture leaves its owning instance: {path}; owner={self.own}')
                for section in sorted(path.glob('*.txt')):
                    compress_closed_record(section,self.own)
                self.sealed.add(path)
                self.window.completed_capture(path)


def compress_case_records(root):
    root = Path(root).resolve()
    paths = [*root.glob('*_controller.jsonl'), *root.glob('*.simdump.txt'), *root.glob('*/feel/raw.jsonl')]
    records = []
    for path in paths:
        if path.is_symlink() or not path.resolve().is_relative_to(root):
            raise ValueError(f'record leaves its run directory: {path}')
        packed = path.with_name(path.name + '.gz')
        digest, size = hashlib.sha256(), 0
        with path.open('rb') as source, packed.open('xb') as target:
            with gzip.GzipFile(filename='', fileobj=target, mode='wb', compresslevel=9, mtime=0) as stream:
                while block := source.read(1024 * 1024):
                    digest.update(block)
                    size += len(block)
                    stream.write(block)
        check, restored_size = hashlib.sha256(), 0
        with gzip.open(packed, 'rb') as stream:
            while block := stream.read(1024 * 1024):
                check.update(block)
                restored_size += len(block)
        if restored_size != size or check.digest() != digest.digest():
            raise RuntimeError(f'compressed record did not reproduce its original bytes: {path}')
        records.append(dict(original=str(path), retained=str(packed), original_bytes=size,
                            original_sha256=digest.hexdigest(), compressed_bytes=packed.stat().st_size))
        path.unlink()
    if records:
        (root / 'compressed-records.json').write_text(json.dumps(records, indent=2) + '\n', encoding='utf-8')
    return records
