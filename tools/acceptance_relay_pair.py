"""Run the existing relay contracts from EROL-PC with ALLY and EDITH game peers."""
from __future__ import annotations

import json
import os
from pathlib import Path
import re
import shutil
import time

from acceptance_collection import read, write
from acceptance_peer_session import Pair
import acceptance_remote as dispatch


def declared(options, mode):
    if (options.host_box,options.client_box) != ('ALLY','EDITH'):
        raise ValueError('relay game roles must be ALLY host and EDITH client')
    if not options.inventory or not options.collection_root:
        raise ValueError('remote relay peers require the inventory and the collection root')
    seconds = options.seconds if mode == 'hold' else 20
    return dict(mode=mode, driver='EROL-PC', host='ALLY', client='EDITH', engine_boxes={'ALLY':1,'EDITH':1},
        credentials='read on EROL-PC; values delivered through SSH stdin and a one-use named pipe',
        connection_mode='RelayOnly', game_ticks=seconds*60,
        native_selftest=mode if mode in ('hold','renew') else None,
        native_scope='the existing in-process TURN test precedes the independent two-box game match')


def run(options, mode, repo):
    if getattr(options,'backends',None):
        from acceptance_relay_matrix import run as run_matrix
        return run_matrix(options,mode,repo)
    declaration = declared(options,mode)
    if getattr(options,'dry_run',False):
        print(json.dumps(declaration)); return 0
    import edith_cross as cross
    import test_directory_ice_join as directory
    from turn_relay_checks import read_login
    from acceptance_relay_policy import CredentialBook, sweep_retained,directory_config,COTURN_CONFIG
    book=getattr(options,'credential_book',None) or CredentialBook()
    backend=getattr(options,'directory_backend',None) or directory_config(
        dict(directory_turn_config_path=str(getattr(options,'login_conf',Path(COTURN_CONFIG)))),options.out,book)
    user,password = getattr(options,'login_override',None) or read_login(getattr(options,'login_conf',Path(COTURN_CONFIG)))
    book.add('TURN username',user);book.add('TURN password',password)
    backend_name=backend['backend']
    root = options.out.resolve(); root.mkdir(parents=True,exist_ok=False)
    write(root/'plan.json',declaration)
    turn = options.turn if options.turn.startswith(('turn:','turns:')) else 'turn:'+options.turn+'?transport=udp'
    ticks = declaration['game_ticks']; timeout = max(420,int(ticks/60)+180)
    directory_port = getattr(options,'port',49496); game_port = getattr(options,'game_port',49497)
    checks = {}
    try:
        with Pair(repo,options.collection_root,options.inventory,options.host_box,options.client_box) as pair:
            if mode in ('hold','renew'):
                task_root = root/'native-selftest'
                argv = [mode,'--turn',options.turn,'--out',str(task_root/'run')]
                if mode == 'hold': argv += ['--seconds',str(options.seconds)]
                native = dict(argv=argv)
                task,done = pair.launch('host',task_root,'relay-selftest',native,
                    dict(environment=dict(CC_TEST_TURN_USER=user,CC_TEST_TURN_PASS=password)),timeout=timeout,book=book)
                state = pair.finish('host',task,done,timeout,task_root,book=book)
                selftest = read(task_root/'run/result.json')
                checks['native_selftest'] = dict(required=True,passed=state.startswith('done rc=0') and selftest.get('passed') is True,
                    product='native-selftest/run/result.json',scope=declaration['native_scope'])
            match = root/'pair'; match.mkdir()
            h = cross.harness(Path(repo)/'tools')
            cross.looped_input(h,match/'input.txt',ticks)
            from e2e.directory import serve
            service_context=serve(match/'directory',directory_port,(49400,49499),turn_config=backend,secret_book=book)
            tokens=service_context.__enter__();pin=tokens['DIRECTORY_PIN']
            tasks = {}
            try:
                pair.directory_tunnel(directory_port)
                for role in ('host','client'):
                    session = None
                    if role == 'client':
                        deadline=time.monotonic()+60
                        while time.monotonic()<deadline:
                            listings=directory.list_sessions(directory_port)
                            if listings:
                                session=listings[0]['session_id'];break
                            time.sleep(.5)
                        if not session: raise RuntimeError('ALLY did not publish its directory row within 60 s')
                    box,_,_,_=pair.boxes[role]
                    peer_root = root/'peers'/role/'match'
                    settings=dict(SessionDirectoryUrl=f'127.0.0.1:{directory_port}',SessionDirectoryCertSha256=pin,
                        SessionDirectoryInstallKey='acceptance-'+pair.cid+'-'+role,NetworkIceEnable='1',
                        NetworkStunServers='',NetworkConnectionMode='RelayOnly',NetworkHostRelayMode='Directory',
                        NetworkTurnServers='',NetworkPlayerTurnServers='',NetworkTurnUser='',NetworkTurnPass='',
                        NetworkPlayerTurnUser='',NetworkPlayerTurnPass='')
                    spec=cross.match_spec(role,peer_root,game_port,
                        ['-net-host','-net-ice','on'] if role=='host' else ['-net-join-session',session,'-net-ice','on'],
                        settings,repo=Path(repo),ticks=ticks,timeout=timeout,lean=True)
                    if getattr(options,'fullstate_every',0):
                        spec['flags'] += ['-net-fullstate-hash-every',str(options.fullstate_every)]
                    if getattr(options,'rendezvous_log',0):
                        spec['flags'] += ['-net-rendezvous-log',str(options.rendezvous_log)]
                    spec['input_files']={'input.txt':(match/'input.txt').read_text()}
                    task,done=pair.launch(role,root/'peers'/role,'relay-peer',spec,
                        dict(settings={}),
                        timeout=timeout,book=book)
                    tasks[role]=(task,done)
                for role,(task,done) in tasks.items():
                    state=pair.finish(role,task,done,timeout,root/'peers'/role,book=book)
                    checks[role+'_task']=dict(required=True,passed=bool(re.search(r'\brc=0\b',state)),receipt=state)
                    box=pair.boxes[role][0]
                    write(root/'identities'/f'{box.name}.json',read(root/'peers'/role/'identity.json'))
                    # Raw archives and their hashes remain under peers/. Analysis receives only the role's native files.
                    origin=root/'peers'/role/'match'
                    for path in origin.iterdir():
                        if not path.name.startswith(role): continue
                        target=match/path.name
                        if path.is_dir(): shutil.copytree(path,target,dirs_exist_ok=False)
                        else: shutil.copy2(path,target)
                scan=sweep_retained(root,book)
                if not scan['safe_to_copy']:raise ValueError('relay sanitizer refused analysis copies')
                shutil.copy2(match/'directory/service.log',match/'service.log')
                verdict=cross.analyze_match(h,match,dict(name='acceptance-relay-pair',started=cross.stamp(),finished=cross.stamp(),
                    direction='host-ally',path='relay' if backend_name=='coturn' else 'directory-relay',ticks=ticks,port=game_port,machines={'host':'ALLY','client':'EDITH'},
                    soak=False,source_sha=pair.source,local_peer='host',feel_records=False,instrumentation='lean',
                    driver_box='EROL-PC',session_id=session,relay_secret_scan=scan['passed'],remote_state='both terminal receipts retained',note=None))
                checks['pair']=dict(required=True,passed=verdict['passed'],product='pair/verdict.json')
                if getattr(options,'fullstate_every',0):
                    from compare_sim_traces import compare_fullstate
                    fullstate=compare_fullstate(match/'host/stdout.log',match/'client/stdout.log')
                    checks['fullstate']=dict(required=True,**fullstate)
                passed=all(check.get('passed') is True for check in checks.values())
            finally:
                service_context.__exit__(None,None,None)
    finally:
        scan=sweep_retained(root,book)
        write(root/'secret-scan.json',scan)
    checks['no_logins_in_kept_files']=dict(required=True,passed=scan['passed'],receipt='secret-scan.json')
    passed=passed and scan['passed']
    result=dict(passed=passed,checks=checks,driver_box='EROL-PC',engine_boxes=declaration['engine_boxes'],
                backend=backend_name,credential_mode='directory '+backend_name+' backend')
    write(root/'result.json',result)
    write(root/'secret-scan.json',sweep_retained(root,book,previous=scan))
    return 0 if passed else 1
