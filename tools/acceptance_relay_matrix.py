"""The primary Cloudflare and alternative coturn contracts behind one required relay parent row."""
from __future__ import annotations

import copy
from pathlib import Path

from acceptance_collection import read, write
import acceptance_relay_policy as policy


def run(options, mode, repo):
    from acceptance_relay_pair import declared, run as pair_run
    declared(options,mode)
    declarations=policy.arms(mode)
    if options.dry_run:
        import json
        print(json.dumps(dict(mode=mode,driver='EROL-PC',engine_boxes={'ALLY':1,'EDITH':1},arms=declarations)))
        return 0
    schedule=read(Path(options.collection_root)/'split-plan.json')
    try: proof=read(Path(options.collection_root)/policy.SAFE_LOGIN['path'])
    except (OSError,ValueError): proof={}
    if not policy.safe_login_proof(proof,schedule['source_sha'],schedule['exe_sha256']):
        print('AWAITING: '+policy.LOGIN_REASON)
        return 3
    from session_directory.session_directory import TurnCredentialProvider
    root=Path(options.out).resolve();root.mkdir(parents=True,exist_ok=False)
    book=policy.CredentialBook()
    backend=policy.directory_config(dict(directory_turn_config_path=policy.CONFIG),root,book)
    login,offer=policy.mint_login(backend,dict(validated=True),TurnCredentialProvider(backend).mint)
    book.add_offer(offer)
    results=[]
    for arm in declarations:
        current=copy.copy(options)
        current.backends=None;current.backend=arm['backend'];current.credential_book=book
        current.out=root/arm['backend']
        if arm['backend']=='cloudflare':
            current.turn=arm['turn'] if mode=='compare' else 'turn.cloudflare.com:3478'
            current.login_override=login;current.directory_backend=backend
        code=pair_run(current,mode,repo)
        result=read(current.out/'result.json')
        results.append(dict(id=arm['id'],backend=arm['backend'],required=True,passed=code==0 and result.get('passed') is True,
                            product=f'{arm["backend"]}/result.json'))
        for path in (current.out/'identities').glob('*.json'):
            target=root/'identities'/path.name
            if target.exists() and read(target)!=read(path):
                # Each run owns a receipt with its timestamp; executable and source identity must still agree.
                left,right=read(target),read(path)
                for key in ('source_sha','exe_sha256','box'):
                    if left.get(key)!=right.get(key): raise ValueError('relay arms ran with different execution identities')
            write(target,read(path))
    scan=policy.scan_retained(root,book)
    result=dict(passed=all(row['passed'] for row in results) and scan['passed'],arms=results,
                checks=dict(no_logins_in_kept_files=dict(required=True,passed=scan['passed'],receipt='secret-scan.json')))
    write(root/'result.json',result)
    write(root/'secret-scan.json',policy.scan_retained(root,book,previous=scan))
    return 0 if result['passed'] else 1
