"""Cloudflare-first relay declarations and credential evidence; credentials remain in memory."""
from __future__ import annotations

import hashlib
import gzip
import json
import os
from pathlib import Path
import re
import stat
import tarfile
import time
import uuid
import zipfile

CONFIG = 'D:/mx/coturn-20260920/turn-config-cloudflare.json'
LOGIN_REASON = "the engine's relay selftests print the TURN username through GNS verbose output (engine row A63.1)"
SAFE_LOGIN = dict(path='capabilities/relay-safe-login.json', engine_row='A63.1', box='ALLY|EDITH')
ITEM_15 = 'relay = Cloudflare through the directory\'s mint (primary), our coturn as the fixed-pair alternative'
LOGIN_FIELD = re.compile(rb'"(username|credential)"\s*:\s*"((?:\\.|[^"\\])+)"')
GNS_USER = re.compile(rb"long-term credentials for user ['\"]([^'\"\r\n]+)")
INI_LOGIN = re.compile(rb'(Network(?:Player)?Turn(?:User|Pass)|CC_TEST_TURN_(?:USER|PASS))["\']?\s*[:=]\s*["\']?([^\s"\'\r\n,}]+)')
HEX_VIEW = re.compile(rb'(?:[0-9a-fA-F]{2}){8,}')
CHUNK = 1 << 20


def decoded_views(data):
    pending=[data];seen=set()
    while pending:
        value=pending.pop();digest=hashlib.sha256(value).digest()
        if digest in seen:continue
        seen.add(digest);yield value
        unescaped=value.replace(b'\\"',b'"').replace(b'\\\\',b'\\')
        if unescaped!=value:pending.append(unescaped)
        if value.count(b'\0')>len(value)//5:
            pending.append(value.decode('utf-16-le',errors='ignore').encode('utf-8'))
        for match in HEX_VIEW.finditer(value):
            encoded=match[0]
            for offset in (0,1):
                candidate=encoded[offset:len(encoded)-((len(encoded)-offset)%2)]
                if candidate:pending.append(bytes.fromhex(candidate.decode('ascii')))


def login_fields(data):
    fields=set()
    for value in decoded_views(data):
        fields.update(match[1].decode('ascii') for match in LOGIN_FIELD.finditer(value)
                      if match[2]!=b'redacted' and set(match[2])!={ord('x')})
        fields.update(match[1].decode('ascii') for match in INI_LOGIN.finditer(value)
                      if match[2]!=b'redacted' and not match[2].startswith(b'__ACCEPTANCE_'))
        if GNS_USER.search(value):fields.add('GNS username')
    return sorted(fields)


def arms(mode):
    name = 'S1.relay-compare' if mode == 'compare' else 'S1.turn-'+mode
    return [dict(id=name+'-cloudflare', backend='cloudflare', primary=True,
                 turn='turn:turn.cloudflare.com:3478?transport=udp',
                 credential_source=CONFIG, credential_transport=['CC_TEST_TURN_USER','CC_TEST_TURN_PASS'],
                 blocked_reason=LOGIN_REASON, native_evidence='engine GNS lines; no provider log is available'),
            dict(id=name, backend='coturn', primary=False, turn='turn:{turn}?transport=udp',
                 credential_source='D:/mx/coturn-20260920/turnserver-fixed.conf')]


def declaration():
    return dict(item_15=ITEM_15,
        cross_arms=[dict(path='directory-relay', backend='cloudflare', primary=True),
                    dict(path='relay', backend='coturn', primary=False)],
        hotspot=dict(scenario='mp-relay-hotspot', rows=list('abcdef'), state='AWAITING',
                     reason='awaiting until the user approves a hotspot session',
                     operator='lead-tools/hotspot_relay.sh <row> [--network lte|5g]',
                     fixed_directory_rows=list('abc'), lane_directory_rows=list('def'),
                     prerequisite='rows a-c require the Cloudflare key deployed on the Mac fixed directory'),
        credential_rule='RED until the sanitizer proves 0 logins in every kept file; replay recording stays enabled',
        safe_login_capability=SAFE_LOGIN)


class CredentialBook:
    def __init__(self): self.needles = {}

    def add(self, kind, value):
        if isinstance(value,str) and value:
            self.needles[value.encode('utf-8')]=kind
            self.needles[value.encode('utf-16-le')]=kind

    def add_turn_config(self, config):
        for key in ('api_token','turn_key_id','static_auth_secret'):
            self.add('backend-'+key,config.get(key))

    def add_offer(self, offer):
        for server in offer.get('iceServers',[]):
            self.add('minted-username',server.get('username'))
            self.add('minted-credential',server.get('credential'))

    def fields(self, data):
        kinds={kind for value in decoded_views(data) for needle,kind in self.needles.items() if needle in value}
        return sorted(kinds),login_fields(data)


def directory_config(run, root, book):
    if 'directory_turn_config' in run:
        raise ValueError('TURN backend keys must be read by path, never from a scenario JSON')
    path=run.get('directory_turn_config_path')
    if run.get('directory_turn_config_fixture'):
        if run['directory_turn_config_fixture']!='cloudflare-refused':
            raise ValueError('unknown directory refusal fixture')
        path=Path(root)/'cloudflare-refused.json'
        path.parent.mkdir(parents=True,exist_ok=True)
        # An unknown but syntactically valid id/token exercises the provider refusal, not an absent backend.
        config=dict(backend='cloudflare',turn_key_id=uuid.uuid4().hex,api_token=uuid.uuid4().hex+uuid.uuid4().hex)
        path.write_text(json.dumps(config)+'\n',encoding='utf-8')
        return config
    if not path: return None
    config=json.loads(Path(path).read_text(encoding='utf-8'))
    if not isinstance(config,dict) or config.get('backend')!='cloudflare':
        raise ValueError('the primary relay requires a Cloudflare backend file')
    book.add_turn_config(config)
    return config


def safe_login_proof(proof, source, executable):
    return (proof.get('pass') is True and proof.get('engine_row')=='A63.1'
            and proof.get('box')=='ALLY|EDITH' and proof.get('source_sha')==source
            and proof.get('exe_sha256')==executable and proof.get('sanitizer_zero_logins') is True)


def mint_login(config, proof, mint):
    # The caller first binds the native proof to its exact source and executable; no mint may precede it.
    if proof.get('validated') is not True: raise ValueError(LOGIN_REASON)
    offer=mint('acceptance-'+uuid.uuid4().hex,86400,int(time.time()))
    servers=offer.get('iceServers',[])
    values=[(server.get('username'),server.get('credential')) for server in servers if server.get('username') and server.get('credential')]
    if not values: raise ValueError('Cloudflare returned no usable TURN login')
    return values[0],offer


def _scan_stream(stream, book):
    kinds=set();fields=set();digest=hashlib.sha256();carry=b''
    overlap=max([65536,*[len(value) for value in book.needles]])
    while block:=stream.read(CHUNK):
        digest.update(block);a,b=book.fields(carry+block);kinds.update(a);fields.update(b)
        carry=(carry+block)[-overlap:]
    return dict(sha256=digest.hexdigest(),kinds=sorted(kinds),fields=sorted(fields))


def scan_retained(root, book, previous=None):
    """Scan every retained byte and every archive member; never clear a previously observed native leak."""
    root=Path(root).resolve();files=[];failures=[];logins=[]
    for directory,dirs,names in os.walk(root,followlinks=False):
        for name in list(dirs)+names:
            path=Path(directory)/name
            if path.is_symlink() or getattr(path.stat(),'st_file_attributes',0) & getattr(stat,'FILE_ATTRIBUTE_REPARSE_POINT',0x400):
                failures.append(dict(path=path.relative_to(root).as_posix(),reason='unscanned reparse point'))
                if name in dirs: dirs.remove(name)
        for name in names:
            path=Path(directory)/name
            if path.is_symlink(): continue
            relative=path.relative_to(root).as_posix()
            try:
                with path.open('rb') as stream: result=_scan_stream(stream,book)
                files.append(dict(path=relative,sha256=result['sha256']))
                entries=[(relative,result)]
                if zipfile.is_zipfile(path):
                    with zipfile.ZipFile(path) as archive:
                        for info in archive.infolist():
                            if not info.is_dir():
                                with archive.open(info) as stream:
                                    entries.append((relative+'!'+info.filename,_scan_stream(gzip.GzipFile(fileobj=stream) if info.filename.endswith('.gz') else stream,book)))
                elif path.suffix.lower() in ('.tar','.tgz') or path.name.endswith('.tar.gz'):
                    with tarfile.open(path,'r:*') as archive:
                        for member in archive:
                            if member.isfile():
                                with archive.extractfile(member) as stream:
                                    entries.append((relative+'!'+member.name,_scan_stream(gzip.GzipFile(fileobj=stream) if member.name.endswith('.gz') else stream,book)))
                elif path.suffix.lower()=='.gz':
                    with gzip.open(path,'rb') as stream:entries.append((relative+'!decoded',_scan_stream(stream,book)))
                for label,entry in entries:
                    if entry['kinds'] or entry['fields']:
                        logins.append(dict(path=label,kinds=entry['kinds'],fields=entry['fields']))
            except (OSError,ValueError,tarfile.TarError,zipfile.BadZipFile) as error:
                failures.append(dict(path=relative,reason=type(error).__name__))
    prior_failure=previous is not None and previous.get('passed') is not True
    passed=bool(files) and not logins and not failures and not prior_failure
    return dict(passed=passed,clean=passed,files_scanned=len(files),files=files,
                files_with_logins=logins,unscanned=failures,prior_native_leak=prior_failure)


def directory_offer(captured, provider):
    root=Path(captured.get('services',{}).get('DIRECTORY_ROOT',''))
    try:
        listing=json.loads((root/'listed.json').read_text(encoding='utf-8'))
        sessions={row['session_id'] for row in listing.get('sessions',[]) if row.get('listen_port')==captured.get('port')}
        lines=(root/'service.log').read_text(encoding='utf-8').splitlines()
    except (OSError,ValueError,KeyError): return False
    for line in lines:
        if 'relay_offer_issued ' not in line: continue
        try: receipt=json.loads(line.split('relay_offer_issued ',1)[1])
        except ValueError: continue
        if receipt.get('session_id') in sessions and receipt.get('provider')==provider: return True
    return False
