"""Refresh the Windows peers and verify every native build declared by an acceptance plan row.

The completed Mac and Linux streams own their builds. This entry point uses those exact trees;
Windows inputs travel through the same inventory helper used by the section dispatcher.
"""
from __future__ import annotations

import argparse
from dataclasses import replace
import json
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import shlex
import sys

from acceptance_collection import read, write
import acceptance_remote as remote
import cross_peers as cross


def resolve(plan_path, row_id, boxes_path):
    plan=read(plan_path)
    rows=[row for row in plan['rows'] if row['id']==row_id]
    if len(rows)!=1: raise ValueError('the refresh needs one exact plan row')
    row=rows[0]
    cross.set_lane(os.environ.get('CC_CROSS_PEERS_LANE','acceptance-refresh'))
    cross.MAC_GUARD=os.environ.get('CC_CROSS_PEERS_MAC_GUARD')
    manifest=cross.load_boxes(Path(boxes_path),row.get('roster','four-way'))
    peers=[peer for peer in manifest['instances'] if peer['name']==row.get('host')]
    if len(peers)!=1: raise ValueError('the plan host is absent from its box manifest')
    selected={name for name,count in row['engine_boxes'].items() if count}
    if selected!={box['name'] for box in manifest['boxes']}:
        raise ValueError('the plan row and refresh box identities differ')
    driver=cross.coordinator(manifest)
    if driver.get('kind')=='coordinator' and 'EROL-PC' in selected:
        raise ValueError('a driver-only refresh cannot declare an EROL-PC game peer')
    return dict(row=row,source_sha=plan['generated']['head'],host_box=peers[0]['box'],driver=driver,manifest=manifest)


def refresh(options):
    selected=resolve(options.plan,options.row,options.boxes)
    tip=cross.command(['git','-C',str(options.repo),'rev-parse',options.tip]).strip()
    if selected['source_sha']!=tip: raise ValueError('plan source differs from refresh tip')
    boxes=selected['manifest']['boxes']
    declaration=dict(row=options.row,host=selected['row']['host'],host_box=selected['host_box'],
                     driver=selected['driver']['name'],source_sha=tip,
                     boxes=[dict(name=box['name'],kind=box['kind'],tree=box['tree'],task=box.get('runner')) for box in boxes])
    if options.dry_run:
        print(json.dumps(declaration,indent=2))
        print('DRY REFRESH: plan host selected; no builds, copies, tasks or engines started')
        return 0
    root=options.work.resolve();root.mkdir(parents=True,exist_ok=False)
    write(root/'declaration.json',declaration)
    split,rb=remote.inventory_modules(options.inventory)
    inventory_boxes,_=split.load_manifest(options.inventory/'boxes.json')
    by_name={box.name:box for box in inventory_boxes}
    source_build=read(options.repo/'tools/cross_peers/build.json')
    executable=split.sha256_file(options.repo/'Cortex Command.exe')
    if (source_build.get('commit')!=tip or source_build.get('executable_sha256')!=executable
            or cross.command(['git','-C',str(options.repo),'rev-parse','HEAD']).strip()!=tip):
        raise ValueError('the refresh source has no measured executable receipt at the declared tip')
    write(root/'build-receipt.json',source_build)
    for box in boxes:
        if box['kind']!='windows-task':continue
        name=PureWindowsPath(box['tree']).name.lower()
        if name in ('z13-dev-build','z13-rows-build','ally-build-src'):
            raise ValueError('an acceptance refresh cannot write a development or rows engine tree')
        native=replace(by_name[box['name']],repo=box['tree'],exe=box['executable'],tools_root=box['tree']+'/tools',
                       ssh=box['ssh'],task=box['runner'],session_script=box['task_script'])
        remote.prepare_box(options.repo,native,root,options.inventory,tip,executable,declared_tree=True)
    values={}
    for box in boxes:
        destination=root/'preflight'/box['name'];destination.mkdir(parents=True)
        native_root=str(PurePosixPath(box['scratch'])/(root.name+'-preflight'))
        payload=destination/'payload.json';write(payload,dict(box=box,specs=[],pin='pending'))
        if box['kind']=='windows-local':
            cross.preflight_payload(payload)
            observed=read(destination/'preflight.json')
        else:
            mkdir=(f'New-Item -ItemType Directory -Force -Path {cross.quote_ps(native_root)} | Out-Null'
                   if box['kind']=='windows-task' else f'mkdir -p {shlex.quote(native_root)}')
            cross.command(['ssh',box['ssh'],mkdir])
            cross.stage_remote(box,payload,native_root+'/payload.json')
            cross.command(cross.remote_command(box,[box['python'],box['tree']+'/tools/cross_peers.py','--preflight',native_root+'/payload.json']),timeout=180)
            target=destination/'preflight.json'
            cross.command(['scp','-q',f'{box["ssh"]}:{native_root}/preflight.json',str(target)],timeout=120)
            observed=read(target)
        values[box['name']]=observed
    cross.require_distinct_machines(values)
    reference=values[selected['host_box']]
    for box in boxes:
        value=values[box['name']];build=value.get('build',{})
        if (build.get('commit')!=tip or build.get('executable_sha256')!=value.get('executable_sha256')
                or any(value.get(key)!=reference.get(key) for key in ('content','modules','fixture'))
                or box['kind'].startswith('windows') and value.get('executable_sha256')!=executable):
            raise ValueError(f'{box["name"]}: native build, executable or content differs from the plan')
    write(root/'result.json',dict(passed=True,**declaration,preflights=values))
    print(f'REFRESH PASS host={selected["host_box"]} driver={selected["driver"]["name"]} boxes={len(boxes)}')
    return 0


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tip');parser.add_argument('previous',nargs='?')
    parser.add_argument('--plan',type=Path,required=True);parser.add_argument('--row',required=True)
    parser.add_argument('--boxes',type=Path,required=True)
    parser.add_argument('--repo',type=Path,default=Path(os.environ.get('CC_CROSS_PEERS_REPO','D:/Projects/alias-walk')))
    parser.add_argument('--inventory',type=Path,default=Path(os.environ.get('CC_INVENTORY_DIR','D:/Projects/reviews/takeover-20260909/grok-workers/lead-tools/inventory')))
    parser.add_argument('--work',type=Path)
    parser.add_argument('--lane',default=os.environ.get('CC_CROSS_PEERS_LANE','acceptance-refresh'))
    parser.add_argument('--mac-guard',default=os.environ.get('CC_CROSS_PEERS_MAC_GUARD'))
    parser.add_argument('--dry-run',action='store_true')
    options=parser.parse_args(argv)
    options.work=options.work or Path('D:/mx')/options.lane/'refresh'
    os.environ['CC_CROSS_PEERS_LANE']=options.lane
    if options.mac_guard:os.environ['CC_CROSS_PEERS_MAC_GUARD']=options.mac_guard
    try:return refresh(options)
    except (OSError,ValueError,KeyError,RuntimeError) as error:
        print('REFRESH FAIL: '+str(error),file=sys.stderr);return 3


if __name__=='__main__':raise SystemExit(main())
