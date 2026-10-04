"""Dispatch one frozen acceptance section; each engine stays on its declared box."""
from __future__ import annotations

import argparse
import copy
import json
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys

import acceptance_collection as collection
import acceptance_remote as remote


def freeze_helpers(inventory, root):
    inventory,root=Path(inventory).resolve(),Path(root).resolve()
    destination=root/'inventory'
    if destination.exists(): raise ValueError('this collection already froze its helpers')
    destination.mkdir()
    selected=[*inventory.glob('*.py'),inventory/'boxes.json']
    selected += [path for path in (inventory/'acceptance-v1').rglob('*') if path.is_file()
                 and path.suffix in ('.py','.json','.sh','.zsh','.md') and '__pycache__' not in path.parts]
    receipts={}
    for source in selected:
        relative=source.relative_to(inventory)
        target=destination/relative;target.parent.mkdir(parents=True,exist_ok=True)
        content=source.read_bytes();target.write_bytes(content)
        receipts[relative.as_posix()]=hashlib.sha256(content).hexdigest()
    gate=inventory.parent/'build_gate.ps1'
    shutil.copy2(gate,root/'build_gate.ps1')
    receipts['../build_gate.ps1']=hashlib.sha256(gate.read_bytes()).hexdigest()
    collection.write(root/'frozen-helpers.json',dict(source=str(inventory),files=receipts))
    collection.write(root/'HELPERS-SHA256.json',{key:value for key,value in receipts.items() if '/' not in key and key.endswith('.py')})
    return receipts


def cross_manifest(repo, inventory, root, mac_lane, linux_lane, *, prepare=False):
    split, _ = remote.inventory_modules(inventory)
    boxes, _ = split.load_manifest(Path(inventory)/'boxes.json')
    by_name={box.name:split.execution_box(box) for box in boxes}
    document=collection.read(Path(repo)/'tools/cross_peers/boxes.json')
    local_game=copy.deepcopy(next(box for box in document['boxes'] if box['kind']=='windows-local'))
    document=copy.deepcopy(document)
    document['driver']=dict(name='EROL-PC',kind='coordinator',directory_port=49918)
    current_schedule=collection.read(Path(root)/'split-plan.json') if (Path(root)/'split-plan.json').is_file() else {}
    guard_owner=current_schedule.get('source_sha','')+':'+Path(root).name
    if prepare:
        schedule=collection.read(Path(root)/'split-plan.json')
        for name in ('Z13','EDITH'):
            remote.prepare_box(repo,next(box for box in boxes if box.name==name),root,inventory,
                               schedule['source_sha'],schedule['exe_sha256'])
    for box in document['boxes']:
        if box['name']=='EROL-PC': box.update(name='Z13',kind='windows-task')
        if box['name'] in ('Z13','EDITH'):
            native=by_name[box['name']]
            box.update(tree=native.repo,executable=native.exe,ssh=native.ssh,runner=native.task,
                task_script=native.session_script,python='python',scratch=f'D:/mx/{Path(root).name}-cross/{{lane}}',
                directory_port=49985,environment=split.runner_environment(native))
            box.pop('guard_file',None);box.pop('exclusive_marker',None)
            if native.path_prepend: box['path_prepend']=native.path_prepend
        else:
            lane=mac_lane if box['name']=='Mac' else linux_lane
            box.update(tree=lane+'/repo',executable=lane+'/repo/build-gcc/CortexCommand',scratch=lane+'/cross/{lane}',
                       build_receipt=lane+'/evidence/build.json')
            box['guard_file']=lane+'/exit.txt' if box['name']=='Mac' else '/home/erol/cortex-workers/opus-run-cross-linux-20260928/BOX-FREE-FOR-CROSS'
            box.setdefault('environment',{}).update(CC_ACCEPTANCE_BOX_OWNER=guard_owner,
                                                    CC_ACCEPTANCE_CROSS_READY=box['guard_file'])
    for peer in document['instances']:
        if peer['box']=='EROL-PC': peer.update(name='z13',box='Z13')
    collection.write(Path(root)/'cross-boxes.json',document)
    window=copy.deepcopy(document);window.pop('driver',None)
    local_game.update(tree=str(Path(repo).resolve()),executable=str(Path(repo).resolve()/'Cortex Command.exe'),
                      scratch=f'D:/mx/{Path(root).name}-cross/{{lane}}')
    local_game.pop('guard_file',None)
    window['boxes']=[local_game if box['name']=='Z13' else box for box in window['boxes']]
    for peer in window['instances']:
        if peer['box']=='Z13':peer.update(box='EROL-PC',name='erol')
    collection.write(Path(root)/'cross-boxes-window.json',window)
    return document


def run_section(options):
    plan=collection.read(options.root/'acceptance-plan.json')
    rows=[row for row in plan['rows'] if row['section']==options.section]
    rows.sort(key=lambda row:(not row.get('window_required'),row['share'],row['id']))
    print(f'section {options.section}: {len(rows)} required rows; variant selected from the section start receipt',flush=True)
    if options.dry_run:
        for row in rows:
            print(f'  {row["share"]}/{row["id"]} driver={row["box"]} engines={json.dumps(row["engine_boxes"],sort_keys=True)} window={row["window_required"]}')
        return 0
    schedule=collection.read(options.root/'split-plan.json')
    if str(options.section) not in schedule.get('section_receipts',{}):
        collection.start_section(options.root,options.section,inventory_root=options.inventory)
        schedule=collection.read(options.root/'split-plan.json')
    codes=[]
    def owned(spec):
        share=collection.Share(options.root,spec['share'],options.inventory)
        try:
            code=collection.run(share,spec['id'],options.repo,options.inventory/'acceptance-v1')
        except Exception as error:
            log=share.receipt_dir(spec['id'])/'driver-error.log'
            log.parent.mkdir(parents=True,exist_ok=True);log.write_text(f'{type(error).__name__}: {error}\n',encoding='utf-8')
            if not (share.receipt_dir(spec['id'])/'command.json').is_file(): share.begin(spec['id'],[])
            code=share.finish(spec['id'],1,log,str(error),not_run=True)['exit_code']
        codes.append(code)
        print(f'{spec["share"]}/{spec["id"]}: exit={code}',flush=True)
    front=[row for row in rows if options.section==4 and row.get('window_required') and row['runner']=='direct']
    for spec in front: owned(spec)
    if any(row['runner']=='run_split' for row in rows):
        argv=[sys.executable,'-B',str(options.inventory/'run_split.py'),'--repo',str(options.repo),'--helpers',str(options.repo),
            '--manifest',str(options.inventory/'boxes.json'),'--exe',schedule['exe_sha256'],'--out',str(options.root),
            '--acceptance-plan',str(options.root/'acceptance-plan.json'),'--collection-plan',str(options.root/'split-plan.json'),
            '--section',str(options.section),'--asan-repo',options.asan_repo]
        result=subprocess.run(argv,cwd=options.repo)
        codes.append(result.returncode)
    for spec in rows:
        if spec['runner'] not in ('run_split','remote-stream') and spec not in front: owned(spec)
    # Successful dispatch can include declared deferrals/review waits. Their own row receipts stay pending.
    return 1 if any(code not in (0,3) for code in codes) else 0


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo',type=Path,required=True)
    parser.add_argument('--root',type=Path,required=True)
    parser.add_argument('--inventory',type=Path,required=True)
    parser.add_argument('--section',type=int,choices=range(1,7))
    parser.add_argument('--asan-repo',default='D:/Projects/z13-inventory-asan')
    parser.add_argument('--dry-run',action='store_true')
    parser.add_argument('--cross-boxes',action='store_true')
    parser.add_argument('--freeze-helpers',action='store_true')
    parser.add_argument('--mac-lane')
    parser.add_argument('--linux-lane')
    options=parser.parse_args(argv)
    if options.freeze_helpers:
        freeze_helpers(options.inventory,options.root);return 0
    if options.cross_boxes:
        if not options.mac_lane or not options.linux_lane: parser.error('cross boxes require the two completed stream lanes')
        cross_manifest(options.repo,options.inventory,options.root,options.mac_lane,options.linux_lane,prepare=not options.dry_run)
        return 0
    if options.section is None: parser.error('--section is required')
    return run_section(options)


if __name__=='__main__':
    raise SystemExit(main())
